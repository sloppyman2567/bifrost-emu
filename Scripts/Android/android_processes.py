"""Supervise Android service VMs in the calling app's Binder realm: forked by a session Zygote, or fresh."""
import array
import fcntl
import json
import os
from pathlib import Path
import re
import select
import shutil
import secrets
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time


URL = re.compile(r'https?://[^\s\x00-\x1f\x7f]{1,2000}\Z')
OPEN_INTERVAL = 1.0
SAME_LINK_WINDOW = 30.0
PROCESS_NAME = re.compile(r'[A-Za-z0-9_.:-]{1,63}\Z')
TRACE_OPTION = '-Dbifrost.apk.trace.dir='
# Shared storage inside an app's root, and the folders MediaStore keeps images in (FrameworkMediaStore).
MEDIA_STORAGE = ('storage', 'emulated', '0')
MEDIA_DIRECTORIES = ('DCIM', 'Pictures')
MAX_MEDIA_FILE = 256 * 1024 * 1024


def exit_description(code):
    """How a process ended, naming what the number usually means: 127 is the dynamic loader failing before main,
    a negative number is a signal (something outside killed it)."""
    if code < 0:
        try:
            return f"was killed by {signal.Signals(-code).name}"
        except ValueError:
            return f"was killed by signal {-code}"
    if code == 127:
        return "exited with status 127 (the dynamic loader could not start it: a missing or incompatible library)"
    return f"exited with status {code}"


def log_tail(path, start=0, lines=15):
    """The last `lines` lines this start wrote to the broker log (from byte `start`), indented for an error message."""
    try:
        with open(path, "rb") as stream:
            stream.seek(start)
            text = stream.read().decode("utf-8", "replace")
    except OSError:
        return f"  (cannot read {path})"
    tail = text.rstrip().splitlines()[-lines:]
    return "\n".join("  " + line for line in tail) if tail else f"  (no output; full log: {path})"


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


def pictures_directory(environ=None, home=None):
    """The desktop's Pictures folder: XDG_PICTURES_DIR in $XDG_CONFIG_HOME/user-dirs.dirs, read as xdg-user-dir reads it
    ("$HOME/..." or an absolute path; a folder set to $HOME itself is disabled), else ~/Pictures."""
    environ = os.environ if environ is None else environ
    home = Path(home or environ.get('HOME') or Path.home())
    config = Path(environ.get('XDG_CONFIG_HOME') or home / '.config') / 'user-dirs.dirs'
    try:
        lines = config.read_text(errors='replace').splitlines()
    except OSError:
        lines = []
    for line in lines:
        match = re.fullmatch(r'\s*XDG_PICTURES_DIR\s*=\s*"(.*)"\s*', line)
        if not match:
            continue
        value = match.group(1)
        path = home / value[len('$HOME/'):] if value.startswith('$HOME/') else Path(value) if value.startswith('/') else None
        if path is not None and path != home:
            return path
    return home / 'Pictures'


def media_folder(label, package):
    """The app's folder under Pictures/Bifrost: its label as one safe path component, else its package."""
    name = re.sub(r'[\x00-\x1f/\\]+', ' ', label or '').strip().lstrip('.').strip()[:100]
    return name or package


def _open_guest_file(root, parts):
    """Open root/parts... for reading, refusing a symlink or special file at every step: the app owns every folder
    below its root and could swap one for a link to a host file."""
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory)
            os.close(directory)
            directory = child
        descriptor = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=directory)
    finally:
        os.close(directory)
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_MEDIA_FILE:
        os.close(descriptor)
        raise ValueError('Not a regular media file of at most 256 MiB')
    return os.fdopen(descriptor, 'rb')


def publish_media(root, guest_path, destination):
    """Copy a finished MediaStore image out of an app's shared storage into `destination` (~/Pictures/Bifrost/<app>).

    The app is untrusted: only /storage/emulated/0/DCIM/... and /storage/emulated/0/Pictures/... regular files are read,
    never through a symlink, and the copy never replaces a host file: it is written under a temporary name and linked
    as "name.ext", "name (1).ext", ... (the first free one). Returns the response for the guest."""
    if not isinstance(guest_path, str) or len(guest_path) > 4096 or '\x00' in guest_path:
        raise ValueError('Invalid media path')
    parts = guest_path.split('/')
    if parts[:4] != ['', *MEDIA_STORAGE] or len(parts) < 6 or parts[4] not in MEDIA_DIRECTORIES \
            or any(part in ('', '.', '..') for part in parts[4:]):
        raise ValueError('Only images in DCIM/ or Pictures/ of shared storage are published')
    name = parts[-1]
    if name.startswith('.') or len(name.encode()) > 200:
        raise ValueError('Invalid media file name')
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with _open_guest_file(root, [*MEDIA_STORAGE, *parts[4:]]) as source:
        handle, temporary = tempfile.mkstemp(prefix='.bifrost-media-', dir=destination)
        try:
            with os.fdopen(handle, 'wb') as target:
                copied = 0
                while chunk := source.read(1 << 20):
                    copied += len(chunk)
                    if copied > MAX_MEDIA_FILE:
                        raise ValueError('Media file grew past 256 MiB')
                    target.write(chunk)
            os.chmod(temporary, 0o644)
            base, dot, extension = name.rpartition('.') if '.' in name else (name, '', '')
            for index in range(1000):
                candidate = name if index == 0 else f'{base} ({index}){dot}{extension}'
                try:
                    os.link(temporary, destination / candidate)
                except FileExistsError:
                    continue
                return dict(name=candidate)
            raise RuntimeError('No free name for the published media file')
        finally:
            os.unlink(temporary)


def receive_message(connection):
    """One length-prefixed JSON object, as ProcessHost's clients and its Zygote send them."""
    length = struct.unpack('!I', ProcessHost.receive(connection, 4))[0]
    if not 0 < length <= 8192:
        raise ValueError('Invalid message size')
    return json.loads(ProcessHost.receive(connection, length))


class ZygoteChild:
    """A child the session Zygote forked, tracked like a Popen through a pidfd (Linux 5.3, Python 3.9). Its exit
    status belongs to the Zygote, which reaps it; poll() reports only that it has ended."""
    def __init__(self, pid, zygote):
        self.pid, self.returncode = pid, None
        self.pidfd = os.pidfd_open(pid)
        try:  # the Zygote reaps its children, so a reused pid has another parent
            with open(f'/proc/{pid}/stat') as stat:
                if int(stat.read().rsplit(')', 1)[1].split()[1]) != zygote:
                    raise ProcessLookupError('The Zygote child has already ended')
        except BaseException:
            os.close(self.pidfd)
            raise

    def poll(self):
        if self.returncode is None and select.select([self.pidfd], [], [], 0)[0]:
            self.returncode = 0
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None and not select.select([self.pidfd], [], [], timeout)[0]:
            raise subprocess.TimeoutExpired(['zygote child', str(self.pid)], timeout)
        self.returncode = 0
        return self.returncode

    def kill(self):
        try:
            signal.pidfd_send_signal(self.pidfd, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def close(self):
        os.close(self.pidfd)


class ProcessHost:
    def __init__(self, root, emu, emu_args, environment, dex, logs, art_options=(), media_mask=None,
                 activities=None, task_session=None, state=None, activity_finished=None, zygote=False,
                 media_destination=None):
        self.root, self.emu = Path(root), str(emu)
        # Where finished MediaStore images go on the host (~/Pictures/Bifrost/<app>); None refuses publish_media.
        self.media_destination = media_destination
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
            log_path = self.logs / 'broker.log'
            self.broker_log = log_path.open('ab')
            start = self.broker_log.tell()
            self.broker = subprocess.Popen([self.emu, '--asgard-broker', self.socket,
                                           '--asgard-service-parent', str(os.getpid())],
                                          stdout=self.broker_log, stderr=subprocess.STDOUT,
                                          env=self.environment)
            deadline = time.monotonic() + 5
            while not Path(self.socket).exists():
                if self.broker.poll() is not None or time.monotonic() > deadline:
                    reason = ('Asgard Binder broker ' + exit_description(self.broker.returncode)
                              if self.broker.returncode is not None else
                              'Asgard Binder broker did not open ' + self.socket + ' within 5 seconds')
                    self.close()
                    raise RuntimeError(reason + ':\n' + log_tail(log_path, start))
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
        self.zygote = self.zygote_channel = None
        if zygote and not any(option.startswith(TRACE_OPTION) for option in self.art_options):
            self.start_zygote()
        self.thread = threading.Thread(target=self.loop, name='AsgardProcessHost', daemon=True)
        self.thread.start()

    def child_environment(self):
        """The supervisor's capability stays with the session process."""
        environment = dict(self.environment)
        environment['BIFROST_GUEST_ENV'] = ';'.join(
            item for item in environment.get('BIFROST_GUEST_ENV', '').split(';')
            if item and not item.startswith('ASGARD_PROCESS_TOKEN='))
        return environment

    def start_zygote(self):
        """Start the session's Zygote (Guest/Android/Managed/ManagedZygote.java), which connects back once it has
        preloaded. Children start as their own emulators until then, without pidfds, and after any Zygote failure."""
        try:
            os.close(os.pidfd_open(os.getpid()))
        except (AttributeError, OSError):
            return
        self.zygote_token = secrets.token_hex(32)
        path = self.root / 'dev/socket/asgard_zygote'
        path.unlink(missing_ok=True)
        self.zygote_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.zygote_listener.bind(str(path))
        path.chmod(0o600)
        self.zygote_listener.listen(1)
        self.zygote_log = (self.logs / 'zygote.log').open('ab')
        # Bound to this supervisor like the broker; its children end with it (zygote_launcher.c).
        command = [self.emu, *self.emu_args, '--android-runtime', '--android-zygote',
                   '--asgard-service-parent', str(os.getpid()), '--android-binder-broker', self.socket,
                   '--rootfs', str(self.root), '/system/bin/bifrost-zygote', '-Xuse-stderr-logger', *self.art_options,
                   '-Djava.class.path=' + self.dex, 'ManagedZygote', '/dev/socket/asgard_zygote', self.zygote_token]
        self.zygote = subprocess.Popen(command, stdout=self.zygote_log, stderr=subprocess.STDOUT,
                                       env=self.child_environment(), start_new_session=True)
        threading.Thread(target=self.accept_zygote, name='AsgardZygote', daemon=True).start()

    def accept_zygote(self):
        self.zygote_listener.settimeout(120)
        try:
            connection, _ = self.zygote_listener.accept()
        except OSError:
            return
        try:
            connection.settimeout(10)
            peer = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            hello = receive_message(connection)
            if (peer[0] != self.zygote.pid or hello.get('pid') != self.zygote.pid or
                    not secrets.compare_digest(str(hello.get('token', '')), self.zygote_token)):
                raise PermissionError('Unexpected Zygote connection')
        except (OSError, ValueError, EOFError):
            connection.close()
            return
        with self.lock:
            if self.stopped.is_set():
                connection.close()
            else:
                self.zygote_channel = connection

    def fork(self, key, controller, uid, name, log):
        """The child, forked by the Zygote; None to start it as its own emulator. A failure retires the Zygote,
        which ends when its channel closes. Called with the lock held."""
        channel = self.zygote_channel
        if channel is None:
            return None
        try:
            data = json.dumps(dict(key=key, controller=controller, uid=uid, name=name)).encode()
            channel.sendmsg([struct.pack('!I', len(data)) + data],
                            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [log.fileno()]))])
            reply = receive_message(channel)
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            return ZygoteChild(int(reply['pid']), self.zygote.pid)
        except (OSError, ValueError, KeyError, TypeError, RuntimeError, EOFError) as error:
            self.zygote_log.write(f'Zygote retired: {error}\n'.encode())
            self.zygote_log.flush()
            self.zygote_channel = None
            channel.close()
            return None

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
            name = request.get('name')
            if type(name) is not str or not PROCESS_NAME.match(name):
                name = 'app_process'
            log = (self.logs / (key + '.log')).open('ab')
            art_options = []
            for option in self.art_options:
                if option.startswith('-Dbifrost.apk.trace.dir='):
                    path = option.split('=', 1)[1] + '/children/' + key
                    (self.root / path.lstrip('/')).mkdir(parents=True, exist_ok=True)
                    option = '-Dbifrost.apk.trace.dir=' + path
                art_options.append(option)
            # The kernel kills the child with the supervisor (it follows the spawning thread, which
            # lives as long as the host), so an interrupted session leaves no service processes.
            command = [self.emu, *self.emu_args, '--android-runtime', '--android-services',
                       '--asgard-service-parent', str(os.getpid()),
                       '--android-binder-broker', self.socket,
                       '--android-app-id', str(uid), '--rootfs', str(self.root),
                       '/system/bin/app_process64', '-Xuse-stderr-logger', *art_options,
                       '-Djava.class.path=' + self.dex, '/system/bin', 'ManagedLauncher',
                       '--service-child', controller, key]
            try:
                process = self.fork(key, controller, uid, name, log) or subprocess.Popen(
                    command, stdout=log, stderr=subprocess.STDOUT, env=self.child_environment(),
                    start_new_session=True)
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
            if isinstance(process, ZygoteChild):
                process.kill()
            else:
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
                if isinstance(child['process'], ZygoteChild):
                    child['process'].close()
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
            elif request['operation'] == 'publish_media':
                if self.media_destination is None:
                    raise ValueError('Saving pictures to the host is unavailable')
                response = publish_media(self.root, request['path'], self.media_destination)
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
                if isinstance(child['process'], ZygoteChild):
                    child['process'].close()
            self.children.clear()
            channel, self.zygote_channel = getattr(self, 'zygote_channel', None), None
        if channel is not None:
            channel.close()
        if getattr(self, 'zygote', None) is not None:
            self.zygote_listener.close()
            (self.root / 'dev/socket/asgard_zygote').unlink(missing_ok=True)
            try:
                os.killpg(self.zygote.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.zygote.wait()
            self.zygote_log.close()
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
        if self.broker:  # a private broker's service root goes with it
            from asgard import remove_media_root
            remove_media_root(self.socket)
        self.temporary.cleanup()
