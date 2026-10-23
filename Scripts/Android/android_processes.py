"""Supervise fresh Android service VMs in the calling app's Binder realm."""
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import secrets
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time


URL = re.compile(r'https?://[^\s\x00-\x1f\x7f]{1,2000}\Z')
OPEN_INTERVAL = 1.0
SAME_LINK_WINDOW = 30.0


def home_command(arguments):
    """Parse the supported am start intent; leave unresolved intents as errors."""
    if not isinstance(arguments, list) or not arguments or len(arguments) > 64 \
            or any(not isinstance(a, str) or len(a) > 2048 for a in arguments):
        raise ValueError('Invalid activity command')
    if arguments[0] not in ('start', 'start-activity'):
        raise ValueError('Supported activity command: am start')
    action, categories, user = None, set(), 'current'
    index = 1
    while index < len(arguments):
        option = arguments[index]
        if index + 1 >= len(arguments): raise ValueError('Missing activity option value')
        value = arguments[index + 1]; index += 2
        if option == '-a': action = value
        elif option == '-c': categories.add(value)
        elif option == '--user': user = value
        elif option == '-f':
            flags = int(value, 0)
            if not 0 <= flags <= 0xffffffff: raise ValueError('Invalid intent flags')
        else: raise ValueError('Unsupported activity option: ' + option)
    if user not in ('0', 'current'): raise ValueError('Only Android user 0 is available')
    if action != 'android.intent.action.MAIN' or 'android.intent.category.HOME' not in categories \
            or categories - {'android.intent.category.HOME', 'android.intent.category.DEFAULT'}:
        raise ValueError('Activity intent is not resolved')
    return True


def open_url(url, last, opener=None, popen=subprocess.Popen, clock=time.monotonic, which=shutil.which):
    """Open an http(s) link from a guest app in the host's default browser.

    The app is untrusted: only well-formed http/https URLs (never an option, file, javascript or
    other scheme) are opened, at most one per OPEN_INTERVAL, and the same link again within
    SAME_LINK_WINDOW is not opened twice (an ad's redirect loop fires one link repeatedly).
    `last` is [time, url] of the previous open. Returns the response for the guest."""
    if not isinstance(url, str) or not URL.match(url):
        raise ValueError('Only http and https links open in the host browser')
    now = clock()
    if last[1] == url and now - last[0] < SAME_LINK_WINDOW:
        return dict(opened=True, repeated=True)
    if now - last[0] < OPEN_INTERVAL:
        raise RuntimeError('Links open too quickly')
    program = opener or which('xdg-open')
    if program is None:
        raise RuntimeError('No host browser launcher (xdg-open) is installed')
    last[:] = [now, url]
    popen([program, url], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
          stderr=subprocess.DEVNULL, start_new_session=True)
    return dict(opened=True)


class ProcessHost:
    def __init__(self, root, emu, emu_args, environment, dex, logs, art_options=(), media_mask=None,
                 activities=None, task_session=None, state=None, activity_finished=None):
        self.root, self.emu = Path(root), str(emu)
        # Runs Activities of other apps' packages that the guest asks for (android_google.ActivityRelay).
        self.activities = activities
        self.activity_finished = activity_finished
        self.task_session, self.state = task_session, state
        self.task_visible = None
        self.home_active = False
        self.last_url = [-SAME_LINK_WINDOW, None]
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
            self.broker = subprocess.Popen([self.emu, '--asgard-broker', self.socket,
                                           '--asgard-service-parent', str(os.getpid())],
                                          stdout=self.broker_log, stderr=subprocess.STDOUT,
                                          env=self.environment)
            deadline = time.monotonic() + 5
            while not Path(self.socket).exists():
                if self.broker.poll() is not None or time.monotonic() > deadline:
                    self.close()
                    raise RuntimeError('Asgard Binder broker did not start')
                time.sleep(.02)
        if media_mask is not None:
            from asgard import ROOT, ensure_media
            try:
                ensure_media(self.socket, self.emu, ROOT / 'build/android-framework/rootfs',
                             ROOT / 'build/android-art/rootfs', ROOT / 'build/android-adapters', media_mask)
            except BaseException:
                self.close()
                raise
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

    def task(self, visible):
        with self.lock:
            self.task_visible = visible
            if visible: self.home_active = False

    def home(self):
        if self.state is None: raise ValueError('HOME is unavailable outside an app session')
        if not self.home_active:
            from android_launcher import activate_launcher
            activate_launcher(self.state)
            self.home_active = True
        self.task(False)
        return dict(exit_code=0)

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
            elif request['operation'] == 'activity':
                if self.activities is None:
                    raise ValueError('Activities of other packages are unavailable')
                response = self.activities.start(request['directory'])
            elif request['operation'] == 'activity_finished':
                if self.activity_finished is None:
                    raise ValueError('No remote Activity request is active')
                response = self.activity_finished()
            elif request['operation'] == 'open_url':
                response = open_url(request['url'], self.last_url)
            elif request['operation'] == 'home':
                response = self.home()
            elif request['operation'] == 'am':
                home_command(request.get('argv'))
                response = self.home()
            elif request['operation'] == 'poll_task':
                with self.lock:
                    response = {} if self.task_visible is None else dict(visible=self.task_visible)
                    self.task_visible = None
            elif request['operation'] == 'task_state':
                background = request.get('background')
                if type(background) is not bool: raise ValueError('Invalid task state')
                if self.task_session is not None: self.task_session.update(background=background)
                response = {}
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
        if self.activities is not None:
            self.activities.close()
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
            try:
                if self.broker.poll() is None:
                    from asgard import control
                    control(self.socket, stop=True)
                self.broker.wait(timeout=5)
            except (OSError, ValueError, subprocess.TimeoutExpired):
                self.broker.kill()
                self.broker.wait()
            self.broker_log.close()
        self.temporary.cleanup()
