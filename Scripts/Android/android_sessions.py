"""Per-app process supervision and authenticated local launcher control."""
import fcntl
import hmac
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def process_result(pid):
    # Observe without reaping: retaining the leader PID prevents it being
    # reused while we terminate its process group and remaining descendants.
    result = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
    if result is None: return None
    return result.si_status if result.si_code == os.CLD_EXITED else 128 + result.si_status


def become_subreaper():
    """Orphans of this process's descendants come to us, not to the desktop's manager, so processes an app
    started that left its process group (a forked helper that called setsid) can be found and ended."""
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
    except (OSError, AttributeError):
        pass


def end_orphans(program):
    """Kill and reap the children of this process that run `program`: what a closed app's group kill missed."""
    me, found = os.getpid(), []
    for entry in os.listdir("/proc"):
        if not entry.isdigit(): continue
        try:
            stat = Path(f"/proc/{entry}/stat").read_text()
            if int(stat[stat.rfind(")") + 2:].split()[1]) != me: continue
            words = Path(f"/proc/{entry}/cmdline").read_bytes().split(b"\0")
        except (OSError, ValueError, IndexError): continue
        if words and words[0] == os.fsencode(program):
            found.append(int(entry))
    for pid in found:
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
    deadline = time.monotonic() + 3
    for pid in found:
        while time.monotonic() < deadline:
            try:
                if os.waitpid(pid, os.WNOHANG)[0]: break
            except ChildProcessError: break
            time.sleep(0.02)
    return found


def terminate_group(child, program=None):
    try: os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    deadline = time.monotonic() + 5
    while process_result(child.pid) is None and time.monotonic() < deadline:
        time.sleep(0.05)
    try: os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    child.wait()
    if program: end_orphans(program)


def host_closed(log):
    # Android apps (Unity among them) may Process.killProcess themselves while
    # the launcher destroys the Activity after the user closed the window.
    # The marker is printed just before teardown, so the log tail suffices.
    try:
        with open(log.name, "rb") as stream:
            stream.seek(max(0, os.fstat(stream.fileno()).st_size - 65536))
            return b"\nAPP_HOST_CLOSED\n" in b"\n" + stream.read()
    except (AttributeError, OSError):
        return False


def publish(path, value):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        pending = Path(stream.name)
        try:
            json.dump(value, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            stream.close()
            pending.replace(path)
        finally:
            pending.unlink(missing_ok=True)


# System packages provisioned into every app root: WebView (ManagedLauncher)
# and microG (Tools/Android/android_microg.py UIDS).
RESERVED_APP_IDS = frozenset({10001, 19998, 19999})


def read_app_ids(path):
    ids = json.loads(path.read_text()) if path.exists() else {}
    if not isinstance(ids, dict) or any(type(v) is not int or not 10000 <= v <= 19999
                                        or v in RESERVED_APP_IDS for v in ids.values()) \
            or len(set(ids.values())) != len(ids):
        raise ValueError("Invalid app ID registry")
    return ids


def release_app_id(state, package):
    """Free a removed package's ID; a reinstall may receive another one."""
    if not (state / ".app-ids.json").exists(): return
    with (state / ".app-ids.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        path = state / ".app-ids.json"
        ids = read_app_ids(path)
        if ids.pop(package, None) is not None: publish(path, ids)


def app_id(state, package):
    """Persist package identity independently of code updates and process IDs."""
    tools = str(Path(__file__).resolve().parents[2] / "Tools/Android")
    if tools not in sys.path: sys.path.insert(0, tools)
    import android_microg
    if package in android_microg.UIDS: return android_microg.UIDS[package]  # provisioned system apps
    state.mkdir(parents=True, exist_ok=True)
    with (state / ".app-ids.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        path = state / ".app-ids.json"
        ids = read_app_ids(path)
        if package not in ids:
            available = next((v for v in range(10000, 20000)
                              if v not in RESERVED_APP_IDS and v not in ids.values()), None)
            if available is None: raise ValueError("Android app ID range exhausted")
            ids[package] = available
            publish(path, ids)
        return ids[package]


def session_log(session):
    """Prefer guest output once present; show preparation output before that."""
    guest = session.get("log")
    preparation = session.get("preparation_log") or (str(Path(guest).with_name("launcher.log")) if guest else None)
    existing = []
    for path in (guest, preparation):
        if not path: continue
        try:
            if Path(path).is_file():
                existing.append(path)
                if Path(path).stat().st_size: return path
        except OSError: continue
    return existing[0] if existing else None


def control(app, operation="status"):
    path = app / "managed/session.json"
    if not path.is_file(): return {"state": "stopped"}
    saved = json.loads(path.read_text())
    if saved.get("state") not in ("preparing", "running", "stopping"): return saved
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(1)
            client.connect(saved["socket"])
            client.sendall(json.dumps({"operation": operation, "token": saved["token"]}).encode() + b"\n")
            data = client.makefile("rb").readline(65537)
            if len(data) > 65536: raise ValueError("Oversized session reply")
            reply = json.loads(data)
            if reply.get("session_id") != saved["session_id"]: raise ValueError("Session identity changed")
            return reply
    except (OSError, KeyError, ValueError):
        # Never signal a PID from a stale file: it may now belong to another app.
        return dict(saved, state="interrupted", socket=None, token=None)


class Session:
    def __init__(self, app, package, uid, session_id=None):
        self.app = app
        self.task_control = None
        self.id = session_id or uuid.uuid4().hex
        if len(self.id) != 32 or any(c not in "0123456789abcdef" for c in self.id):
            raise ValueError("Invalid session ID")
        self.directory = app / "managed/sessions" / self.id
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = app / "managed/session.json"
        self.stop = threading.Event()
        self.closed = threading.Event()
        self.lock = threading.RLock()
        self.started = time.monotonic()
        self.data = {"session_id": self.id, "package": package, "uid": uid,
                     "state": "preparing", "phase": "Preparing app", "supervisor_pid": os.getpid(), "pid": None,
                     "started_at": time.time(), "exit_code": None,
                     "log": str(self.directory.resolve() / "stdout.log"),
                     "preparation_log": str(self.directory.resolve() / "launcher.log"),
                     "logd": str(self.directory.resolve() / "logd.log"),
                     "token": uuid.uuid4().hex}

    def snapshot(self):
        with self.lock:
            return dict(self.data, elapsed_seconds=round(time.monotonic() - self.started, 2))

    def update(self, **changes):
        with self.lock:
            self.data.update(changes)
            publish(self.path, self.data)
            publish(self.directory / "session.json", self.data)

    def _serve(self):
        while not self.closed.is_set():
            try: client, _ = self.server.accept()
            except socket.timeout: continue
            except OSError: return
            with client:
                try:
                    client.settimeout(1)
                    _, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                    if uid != os.getuid(): continue
                    request = json.loads(client.makefile("rb").readline(4097))
                    if not hmac.compare_digest(str(request.get("token", "")), self.data["token"]): continue
                    if request.get("operation") == "stop": self.stop.set()
                    elif request.get("operation") == "resume":
                        if self.task_control is None:
                            client.sendall(json.dumps(dict(self.snapshot(), error="Task resume is unavailable")).encode() + b"\n")
                            continue
                        self.task_control(True)
                    elif request.get("operation") != "status": continue
                    client.sendall(json.dumps(self.snapshot()).encode() + b"\n")
                except (OSError, ValueError): continue

    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bifrost-session-", dir="/tmp")
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.settimeout(0.2)
        self.data["socket"] = str(Path(self.temporary.name) / "control")
        self.server.bind(self.data["socket"])
        self.server.listen(8)
        self.update()
        self.thread = threading.Thread(target=self._serve, name="app-control", daemon=True)
        self.thread.start()
        return self

    def logs(self):
        # Preserve the previous unversioned logs when upgrading old app state.
        for old, name in (("last-run.log", "stdout.log"), ("last-logd.log", "logd.log")):
            target = self.directory / name
            target.touch(exist_ok=False)
            latest = self.app / "managed" / old
            if latest.exists() and not latest.is_symlink():
                latest.rename(self.directory / ("previous-" + name))
            pending = latest.with_name("." + old + "-" + self.id)
            pending.symlink_to(target.relative_to(latest.parent))
            pending.replace(latest)
        return self.directory / "stdout.log", self.directory / "logd.log"

    def run(self, command, *, stdout, env=None, timeout=0):
        child = None
        old_term = signal.getsignal(signal.SIGTERM)
        def interrupted(signum, frame): raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, interrupted)
        code, reason = 1, "failed"
        try:
            if self.stop.is_set():
                code, reason = 143, "stopped"
            else:
                become_subreaper()
                child = subprocess.Popen(command, stdout=stdout, stderr=subprocess.STDOUT,
                                         env=env, start_new_session=True)
                self.update(state="running", phase="App process running", pid=child.pid)
                deadline = time.monotonic() + timeout if timeout else None
                while True:
                    result = process_result(child.pid)
                    if result is not None:
                        code = result
                        reason = ("exited" if code == 0 else
                                  "closed" if code == 128 + signal.SIGKILL and host_closed(stdout) else
                                  "failed")
                        break
                    if self.stop.wait(0.05):
                        code, reason = 143, "stopped"
                        break
                    if deadline is not None and time.monotonic() >= deadline:
                        code, reason = 124, "timed_out"
                        break
        except KeyboardInterrupt:
            code, reason = 130, "stopped"
        finally:
            signal.signal(signal.SIGTERM, old_term)
            if child is not None:
                self.update(state="stopping")
                terminate_group(child, command[0])
            self.update(state=reason, exit_code=code, ended_at=time.time())
        # A user closing the window is a normal ending: the app's own SIGKILL stays in the record, not in the status.
        return 0 if reason == "closed" else code

    def __exit__(self, kind, error, traceback):
        if error is not None or self.data["state"] == "preparing":
            self.update(state="failed", exit_code=1, error=str(error) if error else "App did not start",
                        ended_at=time.time())
        self.closed.set()
        self.server.close()
        self.thread.join(timeout=1.5)
        self.temporary.cleanup()
