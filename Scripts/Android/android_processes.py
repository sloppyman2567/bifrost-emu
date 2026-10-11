"""Supervise fresh Android service VMs in the calling app's Binder realm."""
import fcntl
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time


class ProcessHost:
    def __init__(self, root, emu, emu_args, environment, dex, logs, art_options=()):
        self.root, self.emu = Path(root), str(emu)
        self.emu_args, self.environment = list(emu_args), dict(environment)
        self.dex, self.logs = dex, Path(logs)
        self.art_options = list(art_options)
        self.logs.mkdir(parents=True, exist_ok=True)
        self.token = secrets.token_hex(32)
        self.environment['BIFROST_GUEST_ENV'] = (self.environment.get('BIFROST_GUEST_ENV', '')
                                                 + ';ASGARD_PROCESS_TOKEN=' + self.token).lstrip(';')
        self.children, self.lock = {}, threading.Lock()
        self.stopped = threading.Event()
        self.temporary = tempfile.TemporaryDirectory(prefix='asgard-processes-')
        self.broker = None
        if '--android-binder-broker' in self.emu_args:
            self.socket = self.emu_args[self.emu_args.index('--android-binder-broker') + 1]
        else:
            self.socket = str(Path(self.temporary.name) / 'binder')
            self.broker_log = (self.logs / 'broker.log').open('ab')
            self.broker = subprocess.Popen([self.emu, '--asgard-broker', self.socket],
                                          stdout=self.broker_log, stderr=subprocess.STDOUT,
                                          env=self.environment)
            deadline = time.monotonic() + 5
            while not Path(self.socket).exists():
                if self.broker.poll() is not None or time.monotonic() > deadline:
                    self.close()
                    raise RuntimeError('Asgard Binder broker did not start')
                time.sleep(.02)
        # One byte-range lock per isolated UID, shared by all app supervisors
        # using this realm. Keep one FD open: POSIX locks are process-scoped.
        self.leases = os.open(self.socket + '.uids', os.O_RDWR | os.O_CREAT, 0o600)
        self.path = self.root / 'dev/socket/asgard_process'
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.unlink(missing_ok=True)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        self.path.chmod(0o600)
        self.listener.listen(8)
        self.listener.settimeout(.2)
        self.thread = threading.Thread(target=self.loop, name='AsgardProcessHost', daemon=True)
        self.thread.start()

    @staticmethod
    def receive(client, size):
        result = bytearray()
        while len(result) < size:
            data = client.recv(size - len(result))
            if not data:
                raise EOFError('Process request ended early')
            result.extend(data)
        return bytes(result)

    def allocate_uid(self):
        used = {child['uid'] for child in self.children.values()}
        for uid in range(99000, 100000):
            if uid in used:
                continue
            try:
                fcntl.lockf(self.leases, fcntl.LOCK_EX | fcntl.LOCK_NB, 1, uid - 99000)
                return uid
            except BlockingIOError:
                pass
        raise RuntimeError('No isolated Android UID available')

    def spawn(self, request):
        key, controller = request['key'], request['controller']
        if len(key) != 32 or any(c not in '0123456789abcdef' for c in key):
            raise ValueError('Invalid process key')
        if not controller.startswith('asgard.process.') or len(controller) > 96:
            raise ValueError('Invalid process controller')
        with self.lock:
            if self.stopped.is_set() or len(self.children) >= 16:
                raise RuntimeError('Android process limit reached')
            if key in self.children:
                raise ValueError('Process key already exists')
            isolated = request.get('isolated') is True
            uid = self.allocate_uid() if isolated else request['uid']
            if not isolated and (type(uid) is not int or not 10000 <= uid <= 19999):
                raise ValueError('Invalid service UID')
            log = (self.logs / (key + '.log')).open('ab')
            art_options = []
            for option in self.art_options:
                if option.startswith('-Dbifrost.apk.trace.dir='):
                    path = option.split('=', 1)[1] + '/children/' + key
                    (self.root / path.lstrip('/')).mkdir(parents=True, exist_ok=True)
                    option = '-Dbifrost.apk.trace.dir=' + path
                art_options.append(option)
            command = [self.emu, *self.emu_args, '--android-runtime', '--android-services',
                       '--android-binder-broker', self.socket,
                       '--android-app-id', str(uid), '--rootfs', str(self.root),
                       '/system/bin/app_process64', '-Xuse-stderr-logger', *art_options,
                       '-Djava.class.path=' + self.dex, '/system/bin', 'ManagedLauncher',
                       '--service-child', controller, key]
            child_environment = dict(self.environment)
            child_environment['BIFROST_GUEST_ENV'] = ';'.join(
                item for item in child_environment.get('BIFROST_GUEST_ENV', '').split(';')
                if item and not item.startswith('ASGARD_PROCESS_TOKEN='))
            try:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                           env=child_environment, start_new_session=True)
            except BaseException:
                log.close()
                if isolated:
                    fcntl.lockf(self.leases, fcntl.LOCK_UN, 1, uid - 99000)
                raise
            self.children[key] = dict(process=process, uid=uid, isolated=isolated, log=log)
            return dict(pid=process.pid, uid=uid)

    @staticmethod
    def terminate(child):
        process = child['process']
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=5)

    def reap(self):
        with self.lock:
            for key, child in list(self.children.items()):
                status = child['process'].poll()
                if status is None:
                    continue
                child['log'].close()
                if child['isolated']:
                    fcntl.lockf(self.leases, fcntl.LOCK_UN, 1, child['uid'] - 99000)
                del self.children[key]

    def serve(self, client):
        client.settimeout(5)
        try:
            peer = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if peer[1] != os.getuid():
                raise PermissionError('Foreign process supervisor client')
            length = struct.unpack('!I', self.receive(client, 4))[0]
            if not 0 < length <= 8192:
                raise ValueError('Invalid process request size')
            request = json.loads(self.receive(client, length))
            if not secrets.compare_digest(request.get('token', ''), self.token):
                raise PermissionError('Invalid process capability')
            if request['operation'] == 'spawn':
                response = self.spawn(request)
            elif request['operation'] == 'stop':
                with self.lock:
                    child = self.children.get(request['key'])
                    if child is not None:
                        self.terminate(child)
                response = dict(stopped=child is not None)
            else:
                raise ValueError('Unknown process operation')
        except (ValueError, KeyError, TypeError, OSError, RuntimeError, EOFError) as error:
            response = dict(error=str(error))
        data = json.dumps(response).encode()
        client.sendall(struct.pack('!I', len(data)) + data)

    def loop(self):
        while not self.stopped.is_set():
            self.reap()
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with client:
                try:
                    self.serve(client)
                except OSError:
                    pass

    def close(self):
        self.stopped.set()
        if hasattr(self, 'listener'):
            self.listener.close()
            if hasattr(self, 'thread'):
                self.thread.join(timeout=6)
        with self.lock:
            for child in self.children.values():
                self.terminate(child)
                child['log'].close()
            self.children.clear()
        if hasattr(self, 'leases'):
            os.close(self.leases)
        if hasattr(self, 'path'):
            self.path.unlink(missing_ok=True)
        if self.broker is not None:
            self.broker.terminate()
            try:
                self.broker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.broker.kill()
                self.broker.wait()
            self.broker_log.close()
        self.temporary.cleanup()
