#!/usr/bin/env python3
"""Managed Activity launcher build, persistent runtime and host supervision."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
import hashlib
import http.client
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import android_framework
from android_processes import ProcessHost

PRIORITIES = "??VDIWEFS"
LAUNCHER_DIR = ROOT / "Guest/Android/Managed"
LAUNCHER_SOURCES = ("ManagedLauncher.java", "ActivityProbe.java", "Framework/FrameworkServices.java",
                    "Framework/FrameworkPackages.java", "Framework/FrameworkConnectivity.java",
                    "Framework/FrameworkServiceRuntime.java", "Framework/FrameworkTrace.java",
                    "Framework/FrameworkDisplay.java", "Framework/FrameworkInputMethod.java",
                    "Framework/FrameworkProcessRuntime.java", "Framework/FrameworkJobs.java",
                    "Framework/FrameworkContent.java", "Framework/FrameworkNativeServices.java",
                    "Framework/FrameworkWindows.java", "Framework/FrameworkActivities.java",
                    "Framework/FrameworkAppOps.java", "Framework/FrameworkSurfaces.java",
                    "Framework/FrameworkAudio.java", "Framework/FrameworkNotifications.java",
                    "Framework/FrameworkPower.java", "Framework/FrameworkWifi.java")


def add_trace_arguments(parser):
    parser.add_argument("--trace-apk", nargs="?", const="calls", choices=("calls", "methods", "sample"),
                        help="APK framework calls, pending calls, thread stacks and library mappings; "
                             "methods/sample also records a bounded ART Java/JNI trace")
    parser.add_argument("--trace-after", type=int, default=0, help="seconds before ART method capture, 0..3600")
    parser.add_argument("--trace-duration", type=int, default=15, help="ART capture seconds, 1..300 (16 MiB buffer)")


def trace_options(root, mode=None, after=0, duration=15):
    if mode not in (None, "calls", "methods", "sample") or not 0 <= after <= 3600 or not 1 <= duration <= 300:
        raise ValueError("Invalid APK trace mode or capture window")
    if mode is None:
        if after or duration != 15: raise ValueError("Trace window requires --trace-apk")
        return [], None
    directory = Path(tempfile.mkdtemp(prefix="apk-trace-", dir=root / "data/local/tmp"))
    # mkdtemp returns an absolute path even when the state root is relative.
    guest_path = "/" + directory.relative_to(root.absolute()).as_posix()
    return [f"-Dbifrost.apk.trace.dir={guest_path}", f"-Dbifrost.apk.trace.mode={mode}",
            f"-Dbifrost.apk.trace.after={after}", f"-Dbifrost.apk.trace.duration={duration}"], directory


def method_trace_summary(path):
    """Read ART's non-streaming v2/v3 trace (runtime/trace.cc format V1)."""
    data = path.read_bytes()
    marker = data.find(b"*end\n")
    if marker < 0: raise ValueError("Method trace has no finalized header (capture interrupted)")
    metadata, threads, methods = {}, {}, {}
    section = None
    for line in data[:marker].decode("utf-8", errors="replace").splitlines():
        if line.startswith("*"): section = line
        elif section == "*threads" and "\t" in line:
            key, name = line.split("\t", 1)
            threads[int(key)] = name
        elif section == "*methods" and "\t" in line:
            fields = line.split("\t")
            methods[int(fields[0], 16)] = ".".join(fields[1:3]) + (fields[3] if len(fields) > 3 else "")
        elif section == "*version" and "=" in line:
            key, value = line.split("=", 1)
            metadata[key] = value
    binary = marker + len(b"*end\n")
    if len(data) - binary < 18: raise ValueError("Truncated ART binary header")
    magic, version, offset = struct.unpack_from("<4sHH", data, binary)
    if magic != b"SLOW" or version not in (2, 3):
        raise ValueError(f"Unsupported ART method trace version {version}")
    size = struct.unpack_from("<H", data, binary + 16)[0] if version == 3 else 10
    if offset < 18 or size < (14 if version == 3 else 10) or binary + offset > len(data):
        raise ValueError("Invalid ART method trace record/header size")
    start = binary + offset
    count = (len(data) - start) // size
    entries, recent = Counter(), {}
    for position in range(start, start + count * size, size):
        thread, encoded, clock = struct.unpack_from("<HII", data, position)
        action, method = encoded & 3, encoded & ~3
        if action == 0: entries[method] += 1
        wall = struct.unpack_from("<I", data, position + 10)[0] if version == 3 else clock
        events = recent.setdefault(thread, [])
        events.append({"action": ("enter", "exit", "unwind", "reserved")[action],
                       "method": methods.get(method, hex(method)), "clock_us": wall})
        if len(events) > 12: del events[0]
    return {"version": version, "metadata": metadata, "records": count,
            "overflow": metadata.get("data-file-overflow") == "true",
            "partial_record": (len(data) - start) % size != 0,
            "most_entered": [{"method": methods.get(method, hex(method)), "entries": number}
                             for method, number in entries.most_common(40)],
            "last_events_by_thread": [{"trace_thread_id": thread, "name": threads.get(thread), "events": events}
                                      for thread, events in recent.items()]}


def collect_trace(directory, destination):
    """Preserve guest trace files before a temporary root disappears, even on timeout."""
    if directory is None: return None
    target = destination / directory.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory, target)
    calls, errors, pending, threads, slowest = Counter(), Counter(), {}, {}, []
    truncated = False
    method_started = method_stopped = False
    diagnostics = []
    log = target / "calls.jsonl"
    if log.exists():
        with log.open() as source:
            for line in source:
                try: event = json.loads(line)
                except (ValueError, UnicodeError):
                    truncated = True
                    continue
                kind = event.get("event")
                if kind == "call":
                    calls[event["operation"]] += 1
                    pending[event["id"]] = event
                elif kind in ("return", "throw"):
                    pending.pop(event["id"], None)
                    slowest.append({key: event[key] for key in ("operation", "duration_ns", "tid")})
                    slowest = sorted(slowest, key=lambda item: item["duration_ns"], reverse=True)[:20]
                    if kind == "throw": errors[event["operation"] + ": " + event["exception"]] += 1
                elif kind == "thread.stack": threads[event["java_thread_id"]] = event
                elif kind == "trace.truncated": truncated = True
                elif kind == "methods.start": method_started = True
                elif kind == "methods.stop": method_stopped = True
                elif kind == "trace.error": diagnostics.append(event)
    summary = {"scope": "local framework boundaries; Java stacks; mapped libraries",
               "calls": dict(calls.most_common()), "errors": dict(errors.most_common()),
               "unfinished_calls": list(pending.values()), "slowest_calls": slowest,
               "last_thread_stacks": list(threads.values()), "truncated": truncated,
               "diagnostic_errors": diagnostics, "method_capture_started": method_started,
               "method_capture_completed": method_stopped,
               "methods_trace_bytes": (target / "methods.trace").stat().st_size
               if (target / "methods.trace").exists() else None}
    (target / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if (target / "methods.trace").exists():
        try: method_summary = method_trace_summary(target / "methods.trace")
        except (ValueError, struct.error) as error: method_summary = {"incomplete": True, "error": str(error)}
        (target / "methods-summary.json").write_text(json.dumps(method_summary, indent=2) + "\n")
    print(f"APK trace saved: {target}", flush=True)
    return target


def settings_user_key(state, user=0):
    """One persistent SSAID key per virtual device/user, shared by its apps."""
    if user < 0:
        raise ValueError("Invalid settings user")
    directory = state / ".settings"
    if directory.is_symlink():
        raise ValueError("Refusing redirected settings directory")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    target = directory / f"user-{user}.key"
    # Publish a complete key atomically. Independent package locks must not
    # give simultaneous first launches different identities or partial keys.
    if not target.exists():
        with tempfile.NamedTemporaryFile(dir=directory) as temporary:
            temporary.write(os.urandom(32))
            temporary.flush()
            os.fsync(temporary.fileno())
            try:
                os.link(temporary.name, target)
            except FileExistsError:
                pass
    descriptor = os.open(target, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("Settings user key is not a regular file")
        key = source.read(33)
    if len(key) != 32:
        raise ValueError("Invalid settings user key; refusing to change device identity")
    return key


def install_settings_key(root, key, user=0):
    if user < 0 or len(key) != 32:
        raise ValueError("Invalid settings user key")
    path = root / f"data/system/users/{user}/bifrost-ssaid-key"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(key)
    path.chmod(0o600)


def host_default_interface(proc=Path("/proc/net")):
    """Snapshot the default-route interface, without contacting an Internet host."""
    for name, ipv6 in (("route", False), ("ipv6_route", True)):
        try:
            lines = (proc / name).read_text().splitlines()
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            try:
                if ipv6:
                    default = len(fields) == 10 and fields[0] == "0" * 32 and fields[1] == "00"
                    flags, interface = int(fields[8], 16), fields[9]
                else:
                    default = len(fields) >= 8 and fields[1] == "00000000" and fields[7] == "00000000"
                    flags, interface = int(fields[3], 16), fields[0]
                if default and interface != "lo" and flags & 1 and not flags & 0x200:
                    return interface
            except (IndexError, ValueError):
                continue
    return None


def host_network_validated(host="www.google.com", timeout=3):
    """Android's HTTPS validation probe: only HTTP 204 validates.

    NetworkMonitor treats a redirect as a captive portal and anything else as
    failure, so redirects are not followed. Plain-HTTP partial connectivity and
    captive-portal login are not modelled; both leave the network unvalidated.
    """
    connection = http.client.HTTPSConnection(host, timeout=timeout)
    try:
        connection.request("GET", "/generate_204", headers={"Connection": "close"})
        return connection.getresponse().status == 204
    except (OSError, http.client.HTTPException):
        return False
    finally:
        connection.close()


def host_network_environment():
    """Guest variables describing the launch-time host network snapshot."""
    def override(name):
        value = os.environ.get(name)
        if value is not None and value not in ("0", "1"):
            raise ValueError(name + " must be 0 or 1")
        return value
    route, validation = override("BIFROST_ANDROID_HOST_NETWORK"), override("BIFROST_ANDROID_NETWORK_VALIDATED")
    interface = host_default_interface()
    online = route == "1" if route is not None else interface is not None
    validated = online and (validation == "1" if validation is not None else host_network_validated())
    # An explicitly online profile without a host route keeps Android's Wi-Fi name.
    return {"BIFROST_ANDROID_HOST_NETWORK": str(int(online)),
            "BIFROST_ANDROID_NETWORK_VALIDATED": str(int(validated)),
            "BIFROST_ANDROID_NETWORK_INTERFACE": interface or "wlan0"}


def set_soname(path, soname):
    """Rewrite DT_SONAME in place; the new name must not be longer."""
    data = bytearray(path.read_bytes())
    if data[:5] != b"\x7fELF\x02":
        raise ValueError(f"{path}: not ELF64")
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads, dynamic = [], None
    for index in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + index * phentsize)
        if kind == 1:
            loads.append((vaddr, offset, filesz))
        elif kind == 2:
            dynamic = (offset, filesz)
    tags = {}
    for entry in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from("<qQ", data, entry)
        if tag == 0:
            break
        tags[tag] = value
    strtab = next(offset + tags[5] - vaddr for vaddr, offset, size in loads
                  if vaddr <= tags[5] < vaddr + size)
    start = strtab + tags[14]
    end = data.index(0, start)
    if len(soname) > end - start:
        raise ValueError(f"{path}: soname {soname} longer than {data[start:end].decode()}")
    data[start:end] = soname.encode().ljust(end - start, b"\0")
    path.write_bytes(data)


def install_managed_graphics(root, adapters):
    """Put Bifrost's thunk adapters in front of the image's real libraries.

    Unity resolves libandroid/libEGL/libGLES through the default namespace,
    where the framework already loaded them, so they are replaced there. The
    real library moves to re_<name> (same soname length) and each adapter
    depends on it for every export it does not thunk. libOpenSLES is a whole
    implementation over the libaaudio adapter and needs nothing from its original.
    """
    lib64 = root / "system/lib64"
    names = ("libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libaaudio.so",
             "libOpenSLES.so")
    if {p.name for p in adapters.glob("*.so")} != set(names) | {"mapper.bifrost.so"}:
        raise ValueError("Unexpected managed graphics adapters")
    for adapter in (adapters / name for name in names):
        real = lib64 / adapter.name
        moved = lib64 / ("re_" + adapter.name[3:])
        shutil.copyfile(real, moved)  # new inode; provisioned files are hardlinked
        set_soname(moved, moved.name)
        real.unlink()
        shutil.copyfile(adapter, real)
    vendor = root / "vendor/lib64/hw"
    vendor.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(adapters / "mapper.bifrost.so", vendor / "mapper.bifrost.so")
    # android_load_sphal_library falls back to the visible default namespace
    # in this framework profile. Keep the normal vendor HAL location.
    config = root / "linkerconfig/ld.config.txt"
    text = config.read_text()
    text = re.sub(r"^(namespace.default.search.paths = .*)$", r"\1:/vendor/lib64/hw", text, flags=re.M)
    text = re.sub(r"^(namespace.default.permitted.paths = .*)$", r"\1:/vendor", text, flags=re.M)
    config.write_text(text)


class LogdReceiver:
    """Minimal logd: decode liblog datagrams from <root>/dev/socket/logdw.

    Packet: packed {u8 log_id, u16 tid, u32 sec, u32 nsec} then, for text
    buffers, {u8 priority, tag NUL, message NUL} (system/logging liblog).
    Binary event buffers (log_id 2) are skipped.
    """

    def __init__(self, root, sink):
        path = root / "dev/socket/logdw"
        path.parent.mkdir(parents=True, exist_ok=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(path))
        path.chmod(0o666)
        self.sock.settimeout(0.2)
        self.sink, self.running = sink, True
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def loop(self):
        while self.running:
            try:
                packet = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(packet) < 12:
                continue
            log_id, tid = struct.unpack_from("<BH", packet, 0)
            if log_id == 2:
                continue
            body = packet[11:]
            priority = PRIORITIES[body[0]] if body[0] < len(PRIORITIES) else "?"
            tag, _, rest = body[1:].partition(b"\0")
            message = rest.split(b"\0", 1)[0]
            self.sink.write(f"[logd {tid}] {priority}/{tag.decode(errors='replace')}: "
                            f"{message.decode(errors='replace')}\n".encode())
            self.sink.flush()

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        self.sock.close()


class DnsProxy:
    """Bionic getaddrinfo's netd wire protocol, resolved by the host.

    Android flags/error numbers differ from glibc. Integer framing is big
    endian, but sockaddr and error payloads use the guest's little endian ABI.
    This supplies the default host network, not Android per-network routing.
    """

    FLAGS = {0x1: socket.AI_PASSIVE, 0x2: socket.AI_CANONNAME,
             0x4: socket.AI_NUMERICHOST, 0x8: socket.AI_NUMERICSERV,
             0x400: socket.AI_ADDRCONFIG}
    ERRORS = {getattr(socket, "EAI_" + name): number for name, number in (
        ("ADDRFAMILY", 1), ("AGAIN", 2), ("BADFLAGS", 3), ("FAIL", 4),
        ("FAMILY", 5), ("MEMORY", 6), ("NODATA", 7), ("NONAME", 8),
        ("SERVICE", 9), ("SOCKTYPE", 10), ("SYSTEM", 11), ("OVERFLOW", 14))
        if hasattr(socket, "EAI_" + name)}

    def __init__(self, root):
        self.path = root / "dev/socket/dnsproxyd"
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.bind(str(self.path))
            self.path.chmod(0o666)
            self.sock.listen(16)
            self.sock.settimeout(0.2)
        except BaseException:
            self.sock.close()
            raise
        self.stopped = threading.Event()
        self.slots = threading.BoundedSemaphore(16)
        self.lock = threading.Lock()
        self.clients = set()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    @staticmethod
    def failure(code):
        return b"401\0" + struct.pack("<i", code)

    @classmethod
    def resolve(cls, request):
        args = request.decode("ascii").split()
        if len(args) != 8 or args[0] != "getaddrinfo":
            return cls.failure(4)
        _, host, service, flags, family, kind, protocol, netid = args
        flags, family, kind, protocol, netid = map(int, (flags, family, kind, protocol, netid))
        if not 0 <= netid <= 0xffffffff:
            return cls.failure(4)
        if (flags, family, kind, protocol) == (-1, -1, -1, -1):
            flags = family = kind = protocol = 0
        if flags < 0 or flags & ~sum(cls.FLAGS):
            return cls.failure(3)
        if family not in (socket.AF_UNSPEC, socket.AF_INET, socket.AF_INET6):
            return cls.failure(5)
        host_flags = 0
        for android, native in cls.FLAGS.items():
            if flags & android:
                host_flags |= native
        try:
            results = socket.getaddrinfo(None if host == "^" else host,
                                         None if service == "^" else service,
                                         family, kind, protocol, host_flags)
        except socket.gaierror as error:
            return cls.failure(cls.ERRORS.get(error.errno, 4))
        output = bytearray(b"222\0")
        for family, kind, protocol, canon, address in results:
            if family == socket.AF_INET:
                raw = (struct.pack("<H", family) + struct.pack("!H", address[1]) +
                       socket.inet_pton(family, address[0]) + bytes(8))
            elif family == socket.AF_INET6:
                raw = (struct.pack("<H", family) + struct.pack("!HI", address[1], address[2]) +
                       socket.inet_pton(family, address[0]) + struct.pack("<I", address[3]))
            else:
                continue
            name = canon.encode("utf-8") + b"\0" if canon else b""
            output += struct.pack("!6i", 1, flags, family, kind, protocol, len(raw))
            output += raw + struct.pack("!i", len(name)) + name
        if len(output) == 4:
            return cls.failure(7)
        return bytes(output) + struct.pack("!i", 0)

    def serve(self, client):
        try:
            client.settimeout(5)
            request = bytearray()
            while b"\0" not in request:
                data = client.recv(4096 - len(request))
                if not data:
                    return
                request += data
                if len(request) == 4096 and b"\0" not in request:
                    client.sendall(self.failure(4))
                    return
            try:
                response = self.resolve(bytes(request).split(b"\0", 1)[0])
            except (ValueError, OverflowError, OSError):
                response = self.failure(4)
            client.sendall(response)
        except OSError:
            pass  # client closed, timed out, or launcher is shutting down
        finally:
            with self.lock:
                self.clients.discard(client)
            client.close()
            self.slots.release()

    def loop(self):
        while not self.stopped.is_set():
            try:
                client, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if not self.slots.acquire(blocking=False):
                client.close()
                continue
            with self.lock:
                self.clients.add(client)
            threading.Thread(target=self.serve, args=(client,), daemon=True).start()

    def close(self):
        self.stopped.set()
        self.sock.close()
        self.thread.join(timeout=1)
        with self.lock:
            for client in self.clients:
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                client.close()
        self.path.unlink(missing_ok=True)


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build_launcher(output=ROOT / "build/android-managed-launcher", extra_sources=()):
    """Explicit build step; ordinary app launches never invoke a compiler."""
    output.mkdir(parents=True, exist_ok=True)
    jar = ROOT / "Tools/android-tools/android.jar"
    sources = [LAUNCHER_DIR / name for name in LAUNCHER_SOURCES] + list(extra_sources)
    with tempfile.TemporaryDirectory(prefix=".launcher-", dir=output) as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        subprocess.run([os.environ.get("JAVAC", "javac"), "--release", "8", "-cp", str(jar), "-d", str(classes),
                        *map(str, sources)], check=True)
        subprocess.run(["java", "-cp", str(ROOT / "Tools/android-tools/d8.jar"),
                        "com.android.tools.r8.D8", "--min-api", "26", "--lib", str(jar),
                        "--output", str(work), *map(str, sorted(classes.glob("*.class")))], check=True)
        (work / "classes.dex").replace(output / "classes.dex")
        (output / "sources.json").write_text(json.dumps(
            {"sources": {p.name: file_hash(p) for p in sources}, "dex": file_hash(output / "classes.dex")}, sort_keys=True) + "\n")


def launcher_dex(directory):
    dex = directory / "classes.dex"
    sources = {Path(name).name: file_hash(LAUNCHER_DIR / name)
               for name in LAUNCHER_SOURCES}
    manifest = directory / "sources.json"
    saved = json.loads(manifest.read_text()) if manifest.is_file() else {}
    if not dex.is_file() or saved.get("sources") != sources or saved.get("dex") != file_hash(dex):
        raise ValueError("Managed launcher is missing or stale; run make android-managed-launcher")
    return dex


def installed_arguments(metadata, code):
    # Installation assigns filenames after sorting ALL manifests by split name.
    # The inventory retains archive order. Assign indices before filtering ABIs,
    # or asset packs and the arm64 code split can point at unrelated APKs.
    packages = sorted(metadata["packages"], key=lambda p: p["split"] or "")
    splits = [(p["split"], f"split-{i}.apk") for i, p in enumerate(packages)
              if p["split"] and (not p["abis"] or "arm64-v8a" in p["abis"])]
    return ["--app", metadata["package"], code, metadata["launcher"]["class"],
            *(name + "=" + path for name, path in splits)]


def remove_runtime(root):
    # Installed code directories are deliberately read-only. Only relax the
    # obsolete owned copy after its successor and data have been committed.
    if root.is_symlink(): raise ValueError("Refusing redirected obsolete runtime")
    for directory, names, _ in os.walk(root, followlinks=False):
        path = Path(directory)
        if not path.is_symlink(): path.chmod(path.stat().st_mode | 0o700)
    shutil.rmtree(root)


def ensure_app_data(root, package):
    # installd creates CE/DE app data at every start; ContextImpl only creates
    # subdirectories and fails with ENOENT when the package directory is gone.
    for relative in ("data/user/0", "data/user_de/0"):
        (root / relative / package).mkdir(parents=True, exist_ok=True)


def prepare_root(app, metadata, code, installed_root, framework, art, adapters, dex):
    """Rebuild only the runtime; retain app data across starts and upgrades.

    Caller holds the package lock through preparation AND the live session.
    Never modify the installer's verified code/runtime tree.
    """
    environment = android_framework.install(framework, art)["environment"]
    adapter_manifest = json.loads((adapters / "adapters.json").read_text())
    adapter_names = ["libbifrost-activity.so"] + [
        "managed/" + name for name in ("libandroid.so", "libEGL.so", "libGLESv2.so",
                                      "libGLESv3.so", "libaaudio.so", "libOpenSLES.so",
                                      "mapper.bifrost.so")]
    for name in adapter_names:
        if file_hash(adapters / name) != adapter_manifest["files"].get(name):
            raise ValueError("Modified managed runtime adapter: " + name)
    settings_key = settings_user_key(app.parent)
    identity = {"schema": 3, "code": code, "files": metadata["files"],
                "framework": file_hash(framework / "bifrost-framework.json"),
                "adapters": {n: adapter_manifest["files"][n] for n in adapter_names},
                "launcher": file_hash(dex), "settings_key": hashlib.sha256(settings_key).hexdigest()}
    managed = app / "managed"
    root = managed / "rootfs"
    if managed.is_symlink() or root.is_symlink():
        raise ValueError("Refusing redirected managed runtime")
    managed.mkdir(exist_ok=True)
    stamp = managed / "runtime.json"
    if root.is_dir() and stamp.is_file():
        saved = json.loads(stamp.read_text())
        if saved.get("identity") == identity:
            for relative, value in saved["immutable"].items():
                target = root / relative
                if "link" in value:
                    valid = target.is_symlink() and str(target.readlink()) == value["link"]
                else:
                    valid = (not target.is_symlink() and target.is_file() and
                             target.resolve().is_relative_to(root.resolve()) and
                             file_hash(target) == value["sha256"])
                if not valid: raise ValueError("Modified managed runtime file: " + relative)
            backup = managed / "previous-rootfs"
            if backup.exists(): remove_runtime(backup)
            ensure_app_data(root, metadata["package"])
            return root, environment
    backup = managed / "previous-rootfs"
    if backup.exists() and not root.exists(): backup.rename(root)
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=managed) as temporary:
        fresh = Path(temporary) / "rootfs"
        shutil.copytree(framework, fresh, symlinks=True)
        local = fresh / "data/local/tmp"
        local.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dex, local / "launcher.dex")
        (local / "launcher.dex").chmod(0o444)  # ART rejects writable DEX for ordinary app UIDs.
        shutil.copy2(adapters / "libbifrost-activity.so", local / "libbifrost-activity.so")
        install_managed_graphics(fresh, adapters / "managed")
        install_settings_key(fresh, settings_key)
        destination = fresh / code.lstrip("/")
        shutil.copytree(installed_root / code.lstrip("/"), destination)
        # Guest writes are live in this root, including if the host crashes.
        # Copy them into a replacement root before exchanging runtimes.
        data_source = root if root.is_dir() else installed_root
        for relative in ("data/user", "data/user_de", "storage"):
            source = data_source / relative
            target = fresh / relative
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True, symlinks=True)
        ensure_app_data(fresh, metadata["package"])
        (fresh / "data/data").symlink_to("user/0")
        (fresh / "data/dalvik-cache/arm64").mkdir(parents=True, exist_ok=True)
        (fresh / "storage/emulated/0").mkdir(parents=True, exist_ok=True)
        import android_art
        immutable = {name: value for name, value in android_art.inventory(fresh).items()
                     if name.startswith(("system/", "system_ext/", "product/", "vendor/", "apex/",
                                         "data/app/", "data/local/tmp/"))
                     or name == "data/system/users/0/bifrost-ssaid-key"}
        backup = managed / "previous-rootfs"
        if backup.exists():
            raise ValueError("Interrupted managed runtime replacement; preserve previous-rootfs before retrying")
        if root.exists():
            root.rename(backup)
        try:
            fresh.rename(root)
        except OSError:
            if backup.exists(): backup.rename(root)
            raise
        pending = managed / ".runtime.json"
        pending.write_text(json.dumps({"identity": identity, "immutable": immutable}, sort_keys=True) + "\n")
        pending.replace(stamp)
        if backup.exists(): remove_runtime(backup)
    return root, environment


def launch_managed(app, metadata, code, installed_root, emu, no_jit=False,
                   window_plugin=None, emu_args=(), timeout=0, trace_apk=None, trace_after=0, trace_duration=15,
                   process_session=None):
    if process_session is None: raise ValueError("Managed launch requires an app process session")
    dex = launcher_dex(ROOT / "build/android-managed-launcher")
    # The validation probe can wait for its timeout; overlap it with root setup.
    with ThreadPoolExecutor(1) as pool:
        network = pool.submit(host_network_environment)
        root, guest_env = prepare_root(app, metadata, code, installed_root,
                                      ROOT / "build/android-framework/rootfs",
                                      ROOT / "build/android-art/rootfs",
                                      ROOT / "build/android-adapters", dex)
        guest_env = dict(guest_env, **network.result())
    trace_args, trace_directory = trace_options(root, trace_apk, trace_after, trace_duration)
    # Linux pathname Unix sockets are limited to 108 bytes. Keep the guest
    # root prefix short even when the installed package lives in a deep path;
    # only this alias is temporary, every app write targets persistent rootfs.
    with tempfile.TemporaryDirectory(prefix="bifrost-app-", dir="/tmp") as session:
        guest_root = Path(session) / "rootfs"
        guest_root.symlink_to(root.resolve(), target_is_directory=True)
        command = [str(emu), *emu_args]
        if no_jit: command.append("--no-jit")
        if window_plugin: command += ["--android-window-plugin", str(window_plugin)]
        command += ["--android-runtime", "--android-services",
                    "--android-app-id", str(process_session.data["uid"]), "--rootfs", str(guest_root),
                    "/system/bin/app_process64", "-Xuse-stderr-logger", *trace_args,
                    "-Djava.class.path=/data/local/tmp/launcher.dex", "/system/bin",
                    "ManagedLauncher", *installed_arguments(metadata, code)]
        environment = dict(os.environ)
        environment.pop("BIFROST_ROOT", None)
        if trace_apk:
            for flag in ("BIFROST_BINDER_TRACE", "BIFROST_FAULT_TRACE", "BIFROST_FRAME_TRACE"):
                environment[flag] = "1"
        environment["BIFROST_GUEST_ENV"] = ";".join(f"{k}={v}" for k, v in guest_env.items())
        log_path, logd_path = process_session.logs()
        print(f"Launching {metadata['package']} (log: {log_path})", flush=True)
        # A socket pathname can remain after a killed host process. No other
        # session for this package is live while the caller holds its lock.
        for name in ("logdw", "dnsproxyd"):
            (root / "dev/socket" / name).unlink(missing_ok=True)
        with log_path.open("wb") as log, logd_path.open("wb") as sink, ExitStack() as services:
            process_host = services.enter_context(closing(ProcessHost(guest_root, emu,
                    list(emu_args) + (["--no-jit"] if no_jit else [])
                    + (["--android-window-plugin", str(window_plugin)] if window_plugin else []),
                environment, "/data/local/tmp/launcher.dex", log_path.parent / "processes", trace_args)))
            environment = process_host.environment
            command[1:1] = ["--android-binder-broker", process_host.socket]
            services.enter_context(closing(LogdReceiver(guest_root, sink)))
            services.enter_context(closing(DnsProxy(guest_root)))
            status = 1
            try:
                status = process_session.run(command, stdout=log, env=environment, timeout=timeout)
            finally:
                (root / "dev/socket/logdw").unlink(missing_ok=True)
                collect_trace(trace_directory, app / "managed/traces")
                if trace_directory is not None: shutil.rmtree(trace_directory)
        if status:
            print(f"Managed app exited with status {status}; see {log_path}", file=sys.stderr)
        return status if status >= 0 else 128 - status


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build",))
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-managed-launcher")
    args = parser.parse_args()
    build_launcher(args.output)
