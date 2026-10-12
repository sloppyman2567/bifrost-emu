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


def terminate_group(child):
    try: os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    deadline = time.monotonic() + 5
    while process_result(child.pid) is None and time.monotonic() < deadline:
        time.sleep(0.05)
    try: os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    child.wait()


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


def app_id(state, package):
    """Persist package identity independently of code updates and process IDs."""
    state.mkdir(parents=True, exist_ok=True)
    with (state / ".app-ids.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        path = state / ".app-ids.json"
        ids = json.loads(path.read_text()) if path.exists() else {}
        if not isinstance(ids, dict) or any(type(v) is not int or not 10000 <= v <= 19999
                                            or v == 10001 for v in ids.values()) \
                or len(set(ids.values())) != len(ids):
            raise ValueError("Invalid app ID registry")
        if package not in ids:
            # 10001 belongs to the provisioned WebView package.
            available = next((v for v in range(10000, 20000) if v != 10001 and v not in ids.values()), None)
            if available is None: raise ValueError("Android app ID range exhausted")
            ids[package] = available
            publish(path, ids)
        return ids[package]


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
                     "state": "preparing", "supervisor_pid": os.getpid(), "pid": None,
                     "started_at": time.time(), "exit_code": None,
                     "log": str(self.directory.resolve() / "stdout.log"),
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
                child = subprocess.Popen(command, stdout=stdout, stderr=subprocess.STDOUT,
                                         env=env, start_new_session=True)
                self.update(state="running", pid=child.pid)
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
                terminate_group(child)
            self.update(state=reason, exit_code=code, ended_at=time.time())
        return code

    def __exit__(self, kind, error, traceback):
        if error is not None or self.data["state"] == "preparing":
            self.update(state="failed", exit_code=1, error=str(error) if error else "App did not start",
                        ended_at=time.time())
        self.closed.set()
        self.server.close()
        self.thread.join(timeout=1.5)
        self.temporary.cleanup()
