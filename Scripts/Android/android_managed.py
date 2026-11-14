#!/usr/bin/env python3
"""Managed Activity launcher build, persistent runtime and host supervision."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack, closing
import hashlib
import ipaddress
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
import file_digests
import android_framework
import android_google
import android_microg
import android_paths
from android_processes import ProcessHost, media_folder, pictures_directory

PRIORITIES = "??VDIWEFS"
LAUNCHER_DIR = ROOT / "Guest/Android/Managed"
# Compile-time declarations of hidden framework classes the launcher subclasses or names (InputEventSender): read as a
# source path and never compiled into the dex, so the device's own classes run.
LAUNCHER_STUBS = LAUNCHER_DIR / "Stubs"
LAUNCHER_SOURCES = ("ManagedLauncher.java", "ManagedZygote.java", "ActivityProbe.java", "Framework/FrameworkServices.java",
                    "Framework/FrameworkCompatibility.java",
                    "Framework/FrameworkPackages.java", "Framework/FrameworkConnectivity.java",
                    "Framework/FrameworkServiceRuntime.java", "Framework/FrameworkTrace.java",
                    "Framework/FrameworkDisplay.java", "Framework/FrameworkInputMethod.java",
                    "Framework/FrameworkKeyboard.java", "Framework/FrameworkKeyLayout.java", "Framework/FrameworkGamepads.java",
                    "Framework/FrameworkProcessRuntime.java", "Framework/FrameworkJobs.java",
                    "Framework/FrameworkContent.java", "Framework/FrameworkNativeServices.java",
                    "Framework/FrameworkWindows.java", "Framework/FrameworkActivities.java", "Framework/FrameworkRemoteActivities.java", "Framework/FrameworkLinks.java",
                    "Framework/FrameworkParseCache.java", "Framework/FrameworkStallWatch.java",
                    "Framework/FrameworkAppOps.java", "Framework/FrameworkSurfaces.java",
                    "Framework/FrameworkAudio.java", "Framework/FrameworkNotifications.java",
                    "Framework/FrameworkPower.java", "Framework/FrameworkWifi.java", "Framework/FrameworkNsd.java", "Framework/FrameworkAudioTracks.java", "Framework/FrameworkToasts.java",
                    "Framework/FrameworkMediaStore.java",
                    "Framework/FrameworkPermissions.java", "Framework/FrameworkUiMode.java",
                    "Framework/FrameworkBattery.java", "Framework/FrameworkVibrator.java",
                    "Framework/FrameworkKeyguard.java", "Framework/FrameworkClipboard.java",
                    "Framework/FrameworkAlarms.java", "Framework/FrameworkAccounts.java",
                    "Framework/FrameworkAccountStore.java", "Framework/FrameworkMediaSessions.java",
                    "Framework/FrameworkLocation.java", "Framework/FrameworkSettingsDefaults.java",
                    "Framework/FrameworkSettings.java", "Framework/FrameworkBroadcasts.java",
                    "Framework/FrameworkPendingIntents.java", "Framework/FrameworkUsers.java",
                    "Framework/FrameworkDeviceIdle.java", "Framework/FrameworkMediaRouter.java",
                    "Framework/FrameworkTelephony.java", "Framework/FrameworkPackageChecksums.java",
                    "Framework/FrameworkAppWidgets.java", "Framework/FrameworkRoles.java", "Framework/FrameworkShortcuts.java", "Framework/FrameworkCursors.java",
                    "Framework/FrameworkUsageStats.java", "Framework/FrameworkNetworkStats.java", "Framework/FrameworkMediaMetrics.java",
                    "Framework/FrameworkLocales.java", "Framework/FrameworkLegacyPermissions.java", "Framework/FrameworkAuth.java", "Framework/FrameworkSharedLibraries.java",
                    "Framework/FrameworkKeystore.java")


def add_trace_arguments(parser):
    parser.add_argument("--trace-apk", nargs="?", const="calls", choices=("calls", "activity", "methods", "sample"),
                        help="APK framework calls, pending calls, thread stacks and library mappings; "
                             "activity limits capture to Activity Binder APIs and their catalog; "
                             "methods/sample also records a bounded ART Java/JNI trace")
    parser.add_argument("--trace-after", type=int, default=0, help="seconds before ART method capture, 0..3600")
    parser.add_argument("--trace-duration", type=int, default=15, help="ART capture seconds, 1..300 (16 MiB buffer)")


def trace_options(root, mode=None, after=0, duration=15):
    if mode not in (None, "calls", "activity", "methods", "sample") or not 0 <= after <= 3600 or not 1 <= duration <= 300:
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


def activity_api_report(path):
    """Separate the runtime's available Activity signatures from observed behavior."""
    apis, pending, interfaces = {}, {}, {}
    invalid_records = orphan_completions = 0
    truncated = stopped = False

    def entry(service, method, signature):
        key = (service, signature or method)
        if key not in apis:
            apis[key] = {"service": service, "method": method, "signature": signature,
                         "declared_coverage": "unclassified", "calls": 0, "returns": 0,
                         "throws": 0, "unfinished": 0, "exceptions": {}}
        return apis[key]

    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            try: record = json.loads(line)
            except (ValueError, UnicodeError):
                invalid_records += 1
                continue
            kind = record.get("event")
            if kind == "api.catalog":
                service = record["service"]
                interfaces[service] = record["interface"]
                for method in record["methods"]:
                    entry(service, method["method"], method["signature"])["declared_coverage"] = method["declared_coverage"]
            elif kind in ("call", "return", "throw"):
                service, _, method = record.get("operation", "").rpartition(".")
                if service not in ("activity", "activity_task", "bifrost.activity_client"): continue
                item = entry(service, method, record.get("signature"))
                key = (record.get("pid"), record["id"])
                if kind == "call":
                    item["calls"] += 1
                    pending[key] = item
                    if "first_caller" not in item:
                        item["first_caller"] = record.get("caller", [])
                        item["first_args"] = record.get("args", [])
                else:
                    if pending.pop(key, None) is None: orphan_completions += 1
                    item["returns" if kind == "return" else "throws"] += 1
                    if kind == "throw":
                        exception = record["exception"]
                        item["exceptions"][exception] = item["exceptions"].get(exception, 0) + 1
                        item["last_failure"] = {name: record.get(name) for name in ("exception", "message", "stack")}
            elif kind == "trace.truncated": truncated = True
            elif kind == "trace.stop": stopped = True
    for item in pending.values(): item["unfinished"] += 1
    if not interfaces: return None
    methods = sorted(apis.values(), key=lambda item: (
        -item["throws"], -item["unfinished"], -item["calls"], item["service"], item["signature"] or item["method"]))
    return {"scope": "ActivityManager, ActivityTaskManager and ActivityClientController Binder interfaces; "
                     "available signatures are not proof that the app calls them or that their full contract is implemented",
            "interfaces": interfaces, "truncated": truncated, "invalid_records": invalid_records,
            "trace_stopped": stopped, "orphan_completions": orphan_completions,
            "available_methods": len(methods), "observed_methods": sum(item["calls"] > 0 for item in methods),
            "methods": methods}


def collect_trace(directory, destination):
    """Preserve guest trace files before a temporary root disappears, even on timeout."""
    if directory is None: return None
    target = destination / directory.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory, target)
    calls, errors, pending, threads, slowest = Counter(), Counter(), {}, {}, []
    scope = "local framework boundaries; Java stacks; mapped libraries"
    truncated = False
    method_started = method_stopped = False
    diagnostics = []
    log = target / "calls.jsonl"
    if log.exists():
        with log.open(encoding="utf-8", errors="replace") as source:
            for line in source:
                try: event = json.loads(line)
                except (ValueError, UnicodeError):
                    truncated = True
                    continue
                kind = event.get("event")
                if kind == "trace.start": scope = event.get("scope", scope)
                elif kind == "call":
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
    summary = {"scope": scope,
               "calls": dict(calls.most_common()), "errors": dict(errors.most_common()),
               "unfinished_calls": list(pending.values()), "slowest_calls": slowest,
               "last_thread_stacks": list(threads.values()), "truncated": truncated,
               "diagnostic_errors": diagnostics, "method_capture_started": method_started,
               "method_capture_completed": method_stopped,
               "methods_trace_bytes": (target / "methods.trace").stat().st_size
               if (target / "methods.trace").exists() else None}
    (target / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if log.exists():
        report = activity_api_report(log)
        if report is not None:
            (target / "activity-api-report.json").write_text(json.dumps(report, indent=2) + "\n")
            lines = [report["scope"], "",
                     f"Available signatures: {report['available_methods']}; observed: {report['observed_methods']}",
                     f"Stopped: {report['trace_stopped']}; truncated: {report['truncated']}; "
                     f"invalid records: {report['invalid_records']}; orphan completions: {report['orphan_completions']}", ""]
            for item in report["methods"]:
                lines.append(f"{item['service']}: {item['signature'] or item['method']}")
                lines.append(f"  declared={item['declared_coverage']} calls={item['calls']} "
                             f"returns={item['returns']} throws={item['throws']} unfinished={item['unfinished']}")
                if "last_failure" in item:
                    lines.append(f"  last failure: {item['last_failure']['exception']}: {item['last_failure']['message']}")
                if item.get("first_caller"):
                    lines.append("  first caller: " + " <- ".join(item["first_caller"]))
            (target / "activity-api-report.txt").write_text("\n".join(lines) + "\n")
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
    if os.environ.get("BIFROST_ANDROID_OFFLINE") == "1":
        route = "0"  # the emulator also refuses the sockets; Android should say so
    interface = host_default_interface()
    online = route == "1" if route is not None else interface is not None
    validated = online and (validation == "1" if validation is not None else host_network_validated())
    # An explicitly online profile without a host route keeps Android's Wi-Fi name.
    return {"BIFROST_ANDROID_HOST_NETWORK": str(int(online)),
            "BIFROST_ANDROID_NETWORK_VALIDATED": str(int(validated)),
            "BIFROST_ANDROID_NETWORK_INTERFACE": interface or "wlan0"}


def window_mode_environment(environ):
    """The guest variable for the app's Window setting; unset leaves the default, which keeps the app's shape."""
    mode = environ.get("BIFROST_ANDROID_WINDOW_MODE")
    if mode not in (None, "keep", "resizable"):
        raise ValueError("BIFROST_ANDROID_WINDOW_MODE must be keep or resizable")
    return {"BIFROST_ANDROID_WINDOW_MODE": mode} if mode else {}


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
    names = ("libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libaaudio.so",
             "libOpenSLES.so")
    if {p.name for p in adapters.glob("*.so")} != set(names) | {"mapper.bifrost.so", "vulkan.bifrost.so", BINDER_ATTACH}:
        raise ValueError("Unexpected managed graphics adapters")
    shutil.copyfile(adapters / BINDER_ATTACH, lib64 / BINDER_ATTACH)
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
    # Android's libvulkan loader finds its driver through this HAL module, the
    # same way it finds the gralloc mapper above.
    shutil.copyfile(adapters / "vulkan.bifrost.so", vendor / "vulkan.bifrost.so")
    # android_load_sphal_library falls back to the visible default namespace
    # in this framework profile. Keep the normal vendor HAL location.
    config = root / "linkerconfig/ld.config.txt"
    text = config.read_text()
    text = re.sub(r"^(namespace.default.search.paths = .*)$", r"\1:/vendor/lib64/hw", text, flags=re.M)
    text = re.sub(r"^(namespace.default.permitted.paths = .*)$", r"\1:/vendor", text, flags=re.M)
    config.write_text(text)


# Preloaded into app processes (not the image's tools): attaches native threads that reach an in-process Java service.
BINDER_ATTACH = "libbifrost-binder-attach.so"


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

    def __init__(self, root, offline=False):
        self.offline = offline
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

    @staticmethod
    def local_name(host):
        """Names an offline device still resolves: no name, localhost and loopback address literals."""
        if host in ("^", "localhost", "localhost.localdomain"):
            return True
        try:
            return ipaddress.ip_address(host.split("%")[0]).is_loopback
        except ValueError:
            return False

    @classmethod
    def resolve(cls, request, offline=False):
        args = request.decode("ascii").split()
        if len(args) != 8 or args[0] != "getaddrinfo":
            return cls.failure(4)
        if offline and not cls.local_name(args[1]):
            return cls.failure(7)  # EAI_NODATA, Android's "No address associated with hostname"
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
                response = self.resolve(bytes(request).split(b"\0", 1)[0], self.offline)
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
    return file_digests.digest(path)


def build_launcher(output=ROOT / "build/android-managed-launcher", extra_sources=()):
    """Explicit build step; ordinary app launches never invoke a compiler."""
    compiler = android_paths.javac()
    if not compiler:
        raise SystemExit("Building the managed launcher needs a JDK with javac; install one or set JAVAC")
    output.mkdir(parents=True, exist_ok=True)
    jar = ROOT / "Tools/android-tools/android.jar"
    sources = [LAUNCHER_DIR / name for name in LAUNCHER_SOURCES] + list(extra_sources)
    with tempfile.TemporaryDirectory(prefix=".launcher-", dir=output) as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        subprocess.run([compiler, "--release", "8", "-cp", str(jar), "-sourcepath", str(LAUNCHER_STUBS),
                        "-implicit:none", "-d", str(classes), *map(str, sources)], check=True)
        subprocess.run(["java", "-cp", str(ROOT / "Tools/android-tools/d8.jar"),
                        "com.android.tools.r8.D8", "--min-api", "26", "--lib", str(jar),
                        "--output", str(work), *map(str, sorted(classes.glob("*.class")))], check=True)
        (work / "classes.dex").replace(output / "classes.dex")
        (output / "sources.json").write_text(json.dumps(
            {"sources": {p.name: file_hash(p) for p in sources}, "stubs": launcher_stub_hashes(),
             "dex": file_hash(output / "classes.dex")}, sort_keys=True) + "\n")


def launcher_stub_hashes():
    return {path.relative_to(LAUNCHER_STUBS).as_posix(): file_hash(path) for path in sorted(LAUNCHER_STUBS.rglob("*.java"))}


def launcher_current(directory):
    """Whether `directory` holds a launcher built from the sources and stubs in this tree: the dex is there, is the
    file the build wrote, and every source and stub hashes as it did then."""
    dex = directory / "classes.dex"
    manifest = directory / "sources.json"
    try:
        saved = json.loads(manifest.read_text()) if manifest.is_file() else {}
        sources = {Path(name).name: file_hash(LAUNCHER_DIR / name) for name in LAUNCHER_SOURCES}
        return (dex.is_file() and saved.get("sources") == sources and saved.get("stubs") == launcher_stub_hashes()
                and saved.get("dex") == file_hash(dex))
    except (OSError, ValueError, AttributeError):
        return False


def launcher_dex(directory):
    if not launcher_current(directory):
        # An installed copy has no compiler inputs to rebuild from; a checkout rebuilds what changed.
        fix = ("install Asgard again from its checkout: make install-asgard" if (ROOT / ".asgard-install").is_file()
               else "run make android-provision (it rebuilds only what changed)")
        raise ValueError(f"The managed launcher is missing or does not match its sources; {fix}")
    return directory / "classes.dex"


def installed_arguments(metadata, code):
    # Installation assigns filenames after sorting ALL manifests by split name.
    # The inventory retains archive order. Assign indices before filtering ABIs,
    # or asset packs and the arm64 code split can point at unrelated APKs.
    if metadata.get("system"):
        # A provisioned system package (microG): parsed from its priv-app directory.
        return ["--app", metadata["package"], code, metadata["launcher"]["class"]]
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


# Android's read-only partitions. Every app's runtime links their files from one shared image per set of inputs
# instead of copying them (about 750 MB per app), and the emulator refuses every change below them with EROFS
# (BIFROST_ROOT_READONLY), as Android's read-only mounts do, so no guest can change a file other roots share.
READONLY_PARTITIONS = ("system", "system_ext", "product", "vendor", "apex")
READONLY_ENVIRONMENT = {"BIFROST_ROOT_READONLY": ":".join("/" + name for name in READONLY_PARTITIONS)}
IMAGE_SCHEMA = 1


def make_writable_tree(path):
    for directory, _, _ in os.walk(path, followlinks=False):
        directory = Path(directory)
        if not directory.is_symlink(): directory.chmod(stat.S_IMODE(directory.lstat().st_mode) | 0o700)


def runtime_image(state, framework, adapters, media_mask, inputs):
    """The shared runtime image for `inputs` under <state>/.runtime-images, built once: the framework with microG,
    the media configuration and Bifrost's adapters applied, then made read-only (files and directories without write
    permission; image.json keeps the directories' original modes for the roots). Returns (rootfs, key)."""
    key = hashlib.sha256(json.dumps({"schema": IMAGE_SCHEMA, **inputs}, sort_keys=True).encode()).hexdigest()[:32]
    images = Path(state) / ".runtime-images"
    image = images / key
    if (image / "image.json").is_file(): return image / "rootfs", key
    images.mkdir(parents=True, exist_ok=True)
    import fcntl
    with (images / ".lock").open("a+b") as lock:  # two apps' first launches after an update
        fcntl.flock(lock, fcntl.LOCK_EX)
        if (image / "image.json").is_file(): return image / "rootfs", key
        staging = Path(tempfile.mkdtemp(prefix=".building-", dir=images))
        try:
            root = staging / "rootfs"
            shutil.copytree(framework, root, symlinks=True)
            android_microg.install(root)
            from asgard import configure_media
            configure_media(root, media_mask)
            shutil.copy2(adapters / "managed/am", root / "system/bin/am")
            shutil.copy2(adapters / "managed/zygote", root / "system/bin/bifrost-zygote")
            install_managed_graphics(root, adapters / "managed")
            directories = {}
            for directory, _, files in os.walk(root, followlinks=False):
                directory = Path(directory)
                directories[str(directory.relative_to(root))] = stat.S_IMODE(directory.lstat().st_mode)
                for name in files:
                    path = directory / name
                    if not path.is_symlink(): path.chmod(stat.S_IMODE(path.lstat().st_mode) & ~0o222)
            (staging / "image.json").write_text(json.dumps(
                {"schema": IMAGE_SCHEMA, "inputs": inputs, "directories": directories}, sort_keys=True) + "\n")
            for directory, _, _ in os.walk(root, topdown=False, followlinks=False):
                Path(directory).chmod(directories[str(Path(directory).relative_to(root))] & ~0o222)
            staging.rename(image)
        finally:
            if staging.exists():
                make_writable_tree(staging)
                shutil.rmtree(staging, ignore_errors=True)
    return image / "rootfs", key


def link_runtime_image(image, fresh):
    """Populate a new root from a runtime image: the read-only partitions' files hardlinked to the image's (copied
    where the filesystem refuses a link), everything else copied and writable by its owner, directories with the
    image's original modes."""
    directories = json.loads((image.parent / "image.json").read_text())["directories"]
    for directory, subdirectories, files in os.walk(image, followlinks=False):
        source = Path(directory)
        relative = source.relative_to(image)
        target = fresh / relative
        target.mkdir(exist_ok=True)
        shared = len(relative.parts) > 0 and relative.parts[0] in READONLY_PARTITIONS
        for name in [*files, *(d for d in subdirectories if (source / d).is_symlink())]:
            path, copy = source / name, target / name
            if path.is_symlink():
                copy.symlink_to(os.readlink(path))
                continue
            if shared:
                try:
                    os.link(path, copy)
                    continue
                except OSError:
                    pass
            shutil.copy2(path, copy)
            if not shared: copy.chmod(stat.S_IMODE(copy.stat().st_mode) | 0o200)
    for directory, _, _ in os.walk(fresh, topdown=False, followlinks=False):
        relative = str(Path(directory).relative_to(fresh))
        if relative in directories: Path(directory).chmod(directories[relative])


def prune_runtime_images(state, keep):
    """Remove shared images no app's runtime uses. Roots keep their linked files whatever happens here."""
    images = Path(state) / ".runtime-images"
    if not images.is_dir(): return
    used = {keep}
    for stamp in Path(state).glob("*/managed/runtime.json"):
        try: used.add(json.loads(stamp.read_text()).get("image"))
        except (OSError, ValueError): pass
    for image in images.iterdir():
        if image.name in used or image.name.startswith(".") or image.is_symlink() or not image.is_dir(): continue
        make_writable_tree(image)
        shutil.rmtree(image, ignore_errors=True)


def ensure_app_data(root, package):
    # installd creates CE/DE app data at every start; ContextImpl only creates
    # subdirectories and fails with ENOENT when the package directory is gone.
    # It also creates the profile directories. ART's ProfileSaver and androidx
    # ProfileInstaller create primary.prof there, never the directory itself.
    for relative in ("data/user/0", "data/user_de/0", "data/misc/profiles/cur/0",
                     "data/misc/profiles/ref"):
        (root / relative / package).mkdir(parents=True, exist_ok=True)


def image_properties(root, names, prefixes=()):
    """Read build properties with init's partition precedence (later file wins)."""
    values = {}
    for relative in ("system/build.prop", "system_ext/etc/build.prop", "vendor/default.prop",
                     "vendor/build.prop", "product/etc/build.prop", "debug_ramdisk/adb_debug.prop"):
        path = root / relative
        if not path.is_file(): continue
        for line in path.read_text(errors="replace").splitlines():
            key, separator, value = line.strip().partition("=")
            if separator and (key in names or key.startswith(prefixes)): values[key] = value
    return values


def run_guest_tool(root, emu, guest_env, no_jit, binary, *args, timeout):
    """Run one image command-line tool (profman, dex2oat64) in the app root."""
    environment = dict(os.environ, **READONLY_ENVIRONMENT,
                       BIFROST_GUEST_ENV=";".join(f"{k}={v}" for k, v in guest_env.items()))
    environment.pop("BIFROST_ROOT", None)
    command = [str(emu), *(["--no-jit"] if no_jit else []), "--android-runtime",
               "--rootfs", str(root), binary, *args]
    return subprocess.run(command, env=environment, capture_output=True, timeout=timeout)


DEXOPT_PROPERTIES = {"dalvik.vm.isa.arm64.variant", "dalvik.vm.isa.arm64.features", "dalvik.vm.dex2oat-Xms",
                     "dalvik.vm.dex2oat-Xmx", "dalvik.vm.appimageformat", "dalvik.vm.dex2oat-max-image-block-size",
                     "dalvik.vm.dex2oat-resolve-startup-strings"}
DEXOPT_CONTEXT = "PCL[]"


def dexopt_key(identity, root, metadata, code, guest_env):
    """Compilation dependencies, independently of app-runtime provisioning choices."""
    profile = root / f"data/misc/profiles/ref/{metadata['package']}/primary.prof"
    profiled = profile.is_file() and profile.stat().st_size > 0
    return {"schema": 2, "framework": identity["framework"], "code": code, "context": DEXOPT_CONTEXT,
            "apk": metadata["files"]["base.apk"], "filter": "speed-profile" if profiled else "verify",
            "profile": file_hash(profile) if profiled else None,
            "properties": image_properties(root, DEXOPT_PROPERTIES,
                ("dalvik.vm.", "persist.sys.dalvik.vm.", "ro.dalvik.vm.", "ro.build.version.")),
            "environment": {k: guest_env[k] for k in ("BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "ANDROID_ROOT",
                            "ANDROID_DATA", "ANDROID_ART_ROOT", "ANDROID_I18N_ROOT", "ANDROID_TZDATA_ROOT")
                            if k in guest_env}}


def read_dexopt_stamp(managed):
    try:
        value = json.loads((managed / "dexopt.json").read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def dexopt_artifacts(root, code):
    directory = root / code.lstrip("/") / "oat/arm64"
    result = {}
    for name in ("base.odex", "base.vdex", "base.art"):
        path = directory / name
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()): return None
        if path.is_file(): result[name] = file_hash(path)
    return result


def valid_dexopt(saved, key, root, code, runtime_hash):
    if saved.get("schema") == 2:
        matches = {k: v for k, v in saved.items() if k not in ("failed", "artifacts")} == key
    else:
        # Migrate only when the old, stricter runtime key still proves its inputs.
        matches = (saved.get("runtime") == runtime_hash and
                   all(saved.get(k) == key[k] for k in ("apk", "filter", "profile")))
    if not matches: return False
    if saved.get("failed"): return True
    artifacts = dexopt_artifacts(root, code)
    return (artifacts is not None and {"base.odex", "base.vdex"} <= artifacts.keys()
            and (saved.get("schema") != 2 or artifacts == saved.get("artifacts")))


def save_dexopt_stamp(managed, key, root, code, failed):
    pending = managed / ".dexopt.json"
    pending.write_text(json.dumps(dict(key, failed=failed,
        artifacts={} if failed else dexopt_artifacts(root, code)), sort_keys=True) + "\n")
    pending.replace(managed / "dexopt.json")


def retain_dexopt(managed, old, fresh, identity, metadata, code, guest_env):
    """Carry verified artifacts through a rebuild that did not change compiler inputs."""
    runtime = managed / "runtime.json"
    if not old.is_dir() or not runtime.is_file() or "base.apk" not in metadata["files"]: return None
    previous = json.loads(runtime.read_text())["identity"]
    old_key = dexopt_key(previous, old, metadata, code, guest_env)
    key = dexopt_key(identity, fresh, metadata, code, guest_env)
    saved = read_dexopt_stamp(managed)
    if old_key != key or not valid_dexopt(saved, old_key, old, code, file_hash(runtime)): return None
    if not saved.get("failed"):
        directory = fresh / code.lstrip("/")
        mode = directory.stat().st_mode
        try:
            directory.chmod(mode | stat.S_IWUSR)
            target = directory / "oat/arm64"
            target.mkdir(parents=True, exist_ok=True)
            for name in dexopt_artifacts(old, code):
                shutil.copy2(old / code.lstrip("/") / "oat/arm64" / name, target / name)
        finally:
            directory.chmod(mode)
    return key, bool(saved.get("failed"))


class OptimizationCancelled(Exception):
    """A launch asked the after-session optimizer to stop (SIGUSR1)."""


DEXOPT_OUTPUTS = ("base.odex", "base.vdex", "base.art")


def dex2oat_arguments(stamp, apk, oat, reference, profiled, odex="base.odex"):
    properties = stamp["properties"]
    arguments = ["--dex-file=" + apk, "--dex-location=" + apk, "--oat-file=" + oat + "/" + odex,
                 "--instruction-set=arm64",
                 "--instruction-set-variant=" + properties.get("dalvik.vm.isa.arm64.variant", "generic"),
                 "--instruction-set-features=" + properties.get("dalvik.vm.isa.arm64.features", "default"),
                 "--compiler-filter=" + ("speed-profile" if profiled else "verify"),
                 "--class-loader-context=" + stamp["context"],
                 "--compilation-reason=" + ("bg-dexopt" if profiled else "install")]
    for option in ("Xms", "Xmx"):
        if properties.get("dalvik.vm.dex2oat-" + option):
            arguments += ["--runtime-arg", f"-{option}{properties['dalvik.vm.dex2oat-' + option]}"]
    if profiled:
        arguments += ["--profile-file=" + reference, "--app-image-file=" + oat + "/base.art"]
        if properties.get("dalvik.vm.appimageformat"):
            arguments.append("--image-format=" + properties["dalvik.vm.appimageformat"])
        if properties.get("dalvik.vm.dex2oat-max-image-block-size"):
            arguments.append("--max-image-block-size=" + properties["dalvik.vm.dex2oat-max-image-block-size"])
        if properties.get("dalvik.vm.dex2oat-resolve-startup-strings"):
            arguments.append("--resolve-startup-const-strings=" +
                             properties["dalvik.vm.dex2oat-resolve-startup-strings"])
    return arguments


def microg_dexopt_key(identity, root, guest_env):
    """Inputs of microG's compile: the same APKs, framework and properties in every app's root."""
    return {"schema": 1, "framework": identity["framework"], "microg": identity["microg"], "filter": "verify",
            "context": DEXOPT_CONTEXT,
            "properties": image_properties(root, DEXOPT_PROPERTIES,
                ("dalvik.vm.", "persist.sys.dalvik.vm.", "ro.dalvik.vm.", "ro.build.version.")),
            "environment": {k: guest_env[k] for k in ("BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "ANDROID_ROOT",
                            "ANDROID_DATA", "ANDROID_ART_ROOT", "ANDROID_I18N_ROOT", "ANDROID_TZDATA_ROOT")
                            if k in guest_env}}


def microg_dexopt(state, root, emu, guest_env, no_jit=False):
    """The install dexopt of microG's system packages (verify), shared by every app's root.

    GmsCore keeps 48 MB of dex compressed in its APK: without a vdex every process that loads its code (microG's own,
    and an app's dynamite modules) inflates and verifies all of it, about 0.7 s each. A verify compile copies the dex
    into the vdex, which processes map instead. It is compiled once per framework, microG version and property set
    into <state>/.microg-dexopt and copied into each root as system/priv-app/<name>/oat/arm64/<name>.{odex,vdex}. A
    failure is recorded and not retried until an input changes; ART then reads the APK as before.
    """
    identity = json.loads((root.parent / "runtime.json").read_text())["identity"]
    key = microg_dexopt_key(identity, root, guest_env)
    cache = Path(state) / ".microg-dexopt" / hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()[:32]
    if not (cache / "dexopt.json").is_file():
        cache.parent.mkdir(parents=True, exist_ok=True)
        print("Compiling microG (verify)", flush=True)
        scratch = Path(tempfile.mkdtemp(prefix="microg-", dir=root / "data/local/tmp"))
        staging = Path(tempfile.mkdtemp(prefix=".compiling-", dir=cache.parent))
        failed = []
        try:
            guest = "/" + str(scratch.relative_to(root))
            for package, (directory, *_rest) in sorted(android_microg.PACKAGES.items()):
                apk = f"/system/priv-app/{directory}/{directory}.apk"
                if not (root / apk.lstrip("/")).is_file(): continue
                try:
                    result = run_guest_tool(root, emu, guest_env, no_jit, "/apex/com.android.art/bin/dex2oat64",
                                            *dex2oat_arguments(key, apk, guest, None, False, directory + ".odex"),
                                            timeout=600)
                    ok = result.returncode == 0
                except subprocess.TimeoutExpired:
                    result, ok = None, False
                outputs = [scratch / (directory + suffix) for suffix in (".odex", ".vdex")]
                if ok and all(path.is_file() for path in outputs):
                    for path in outputs: shutil.move(path, staging / path.name)
                else:
                    failed.append(package)
                    detail = result.stderr.decode(errors="replace")[-2000:] if result else "timed out"
                    print(f"dex2oat failed for {package}; it loads from its APK:\n{detail}", file=sys.stderr)
            (staging / "dexopt.json").write_text(json.dumps(dict(key, failed=failed), sort_keys=True) + "\n")
            try:
                staging.rename(cache)
            except OSError:  # another launch finished first; its result is the same compile
                pass
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
            shutil.rmtree(staging, ignore_errors=True)
    for package, (directory, *_rest) in android_microg.PACKAGES.items():
        names = [directory + suffix for suffix in (".odex", ".vdex")]
        if not all((cache / name).is_file() for name in names): continue
        target = root / "system/priv-app" / directory / "oat/arm64"
        if all((target / name).is_file() and file_hash(target / name) == file_hash(cache / name) for name in names):
            continue
        target.mkdir(parents=True, exist_ok=True)
        for name in names:
            (target / name).unlink(missing_ok=True)
            (cache / name).chmod(0o444)  # read-only like the partition it is linked into
            try:
                os.link(cache / name, target / name)  # one copy for every root
            except OSError:
                shutil.copyfile(cache / name, target / name)
                (target / name).chmod(0o444)


def dexopt(managed, root, metadata, code, emu, guest_env, no_jit=False):
    """ART Service's install dexopt of the base APK (artd/artd.cc), on the launch path.

    It compiles only when no valid artifacts exist for the committed reference profile (first launch, a new
    framework, APK or property set): profile merges and the recompiles they call for run after the session
    (optimize_after_session), as ART Service's background dexopt does on a device, so a launch never waits for
    them. Without a reference profile the image's speed-profile install filter degrades to verify. Split APK
    code stays with ART's JIT. Caller holds the package lock.
    """
    package = metadata["package"]
    apk = code + "/base.apk"
    reference = f"/data/misc/profiles/ref/{package}/primary.prof"
    oat = code + "/oat/arm64"
    profile = root / reference.lstrip("/")
    profiled = profile.is_file() and profile.stat().st_size > 0
    runtime = managed / "runtime.json"
    stamp = dexopt_key(json.loads(runtime.read_text())["identity"], root, metadata, code, guest_env)
    outputs = [root / oat.lstrip("/") / name for name in DEXOPT_OUTPUTS]
    saved = read_dexopt_stamp(managed)
    # A failure is not retried until an input changes.
    if valid_dexopt(saved, stamp, root, code, file_hash(runtime)):
        if saved.get("schema") != 2:
            save_dexopt_stamp(managed, stamp, root, code, bool(saved.get("failed")))
        return
    directory = root / code.lstrip("/")
    mode = directory.stat().st_mode
    print(f"Compiling {package} ({'speed-profile' if profiled else 'verify'})", flush=True)
    try:
        directory.chmod(mode | stat.S_IWUSR)
        (directory / "oat/arm64").mkdir(parents=True, exist_ok=True)
        for path in outputs: path.unlink(missing_ok=True)
        try:
            result = run_guest_tool(root, emu, guest_env, no_jit, "/apex/com.android.art/bin/dex2oat64",
                                    *dex2oat_arguments(stamp, apk, oat, reference, profiled), timeout=3600)
            failed = result.returncode != 0
        except subprocess.TimeoutExpired:
            result, failed = None, True
        if failed:
            for path in outputs: path.unlink(missing_ok=True)
            detail = result.stderr.decode(errors="replace")[-2000:] if result else "timed out"
            print(f"dex2oat failed; {package} runs from ART's JIT:\n{detail}", file=sys.stderr)
        for path in outputs:
            if path.exists(): path.chmod(0o444)
    finally:
        directory.chmod(mode)
    save_dexopt_stamp(managed, stamp, root, code, failed)


def optimizer_pid_file(managed):
    return managed / "optimizing.pid"


def cancel_optimizer(managed):
    """Ask a finished session's optimizer to stop, so a new launch can take the package lock. True if one runs."""
    try:
        pid = int(optimizer_pid_file(managed).read_text().strip())
        os.kill(pid, signal.SIGUSR1)
        return True
    except (OSError, ValueError):
        return False


def optimize_after_session(managed, root, metadata, code, emu, guest_env, no_jit=False):
    """ART Service's background dexopt (artd MergeProfiles + Dexopt), run once the app has exited.

    profman merges the session's current profile into a staged copy of the reference profile; only when it asks
    for a compile (kCompile) is the app recompiled, with the previous artifacts set aside. The staged profile and
    the new record are committed together after a successful compile, so a failure or a cancellation (a launch
    sends SIGUSR1) leaves the previous profile, artifacts and record exactly as the next launch expects them.
    Caller holds the package lock.
    """
    package = metadata["package"]
    if metadata.get("system") or "base.apk" not in metadata["files"]: return
    apk = code + "/base.apk"
    current = f"/data/misc/profiles/cur/0/{package}/primary.prof"
    reference = f"/data/misc/profiles/ref/{package}/primary.prof"
    staged = reference + ".next"
    oat = code + "/oat/arm64"
    current_path, reference_path, staged_path = (root / name.lstrip("/") for name in (current, reference, staged))
    if not current_path.is_file() or not current_path.stat().st_size: return
    pid_file = optimizer_pid_file(managed)
    directory = root / code.lstrip("/")
    output_dir = root / oat.lstrip("/")
    aside = output_dir / ".previous"
    mode = directory.stat().st_mode

    def cancelled(signum, frame): raise OptimizationCancelled()
    previous_handler = signal.signal(signal.SIGUSR1, cancelled)
    pending = pid_file.with_name("." + pid_file.name)
    pending.write_text(f"{os.getpid()}\n")
    pending.replace(pid_file)
    moved = False
    try:
        staged_path.parent.mkdir(parents=True, exist_ok=True)
        if reference_path.is_file(): shutil.copy2(reference_path, staged_path)
        else: staged_path.touch()  # profman locks an existing reference by path; artd creates it first
        merge = run_guest_tool(root, emu, guest_env, no_jit, "/apex/com.android.art/bin/profman",
                               "--profile-file=" + current, "--reference-profile-file=" + staged,
                               "--apk=" + apk, "--dex-location=" + apk, timeout=300)
        # profman_result.h: 1 kCompile; 0, 2 (small delta) and 7 (empty) leave the compiled code as it is.
        if merge.returncode != 1:
            if merge.returncode not in (0, 2, 7):
                print(f"profman failed ({merge.returncode}); keeping the previous reference profile", file=sys.stderr)
            return
        runtime = managed / "runtime.json"
        stamp = dexopt_key(json.loads(runtime.read_text())["identity"], root, metadata, code, guest_env)
        print(f"Compiling {package} (speed-profile) for its next launch", flush=True)
        directory.chmod(mode | stat.S_IWUSR)
        output_dir.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(aside, ignore_errors=True)
        aside.mkdir()
        for name in DEXOPT_OUTPUTS:
            if (output_dir / name).exists(): (output_dir / name).rename(aside / name)
        moved = True
        result = run_guest_tool(root, emu, guest_env, no_jit, "/apex/com.android.art/bin/dex2oat64",
                                *dex2oat_arguments(stamp, apk, oat, staged, True), timeout=3600)
        if result.returncode != 0:
            print(f"dex2oat failed; {package} keeps its previous code:\n"
                  f"{result.stderr.decode(errors='replace')[-2000:]}", file=sys.stderr)
            return
        for name in DEXOPT_OUTPUTS:
            if (output_dir / name).exists(): (output_dir / name).chmod(0o444)
        staged_path.replace(reference_path)
        save_dexopt_stamp(managed, dexopt_key(json.loads(runtime.read_text())["identity"], root, metadata, code,
                                              guest_env), root, code, False)
        shutil.rmtree(aside)
        moved = False
    except OptimizationCancelled:
        print(f"Optimizing {package} was cancelled by a new launch", flush=True)
    except subprocess.TimeoutExpired:
        print(f"Optimizing {package} timed out; it keeps its previous code", file=sys.stderr)
    finally:
        signal.signal(signal.SIGUSR1, signal.SIG_IGN)
        if moved:
            for name in DEXOPT_OUTPUTS:
                (output_dir / name).unlink(missing_ok=True)
                if (aside / name).exists(): (aside / name).rename(output_dir / name)
            shutil.rmtree(aside, ignore_errors=True)
        staged_path.unlink(missing_ok=True)
        if directory.exists(): directory.chmod(mode)
        pid_file.unlink(missing_ok=True)
        signal.signal(signal.SIGUSR1, previous_handler)


# FrameworkParseCache (/data/local/tmp/package-cache) holds what the launcher derives from parsing immutable archives,
# keyed in the guest by each archive's path, size and time, the platform resources and the build fingerprint. A rebuilt
# runtime keeps it while the sources that derive the entries are unchanged: WebView's and the app's parses cost about
# 2 s each on the main thread.
PARSE_CACHE = "data/local/tmp/package-cache"
PARSE_CACHE_SOURCES = ("FrameworkPackages.java", "FrameworkParseCache.java")


def parse_cache_code(dex):
    """The digests of the launcher sources that derive parse cache entries, from the dex's build manifest, or None."""
    try:
        sources = json.loads((Path(dex).parent / "sources.json").read_text())["sources"]
        return {name: sources[name] for name in PARSE_CACHE_SOURCES}
    except (OSError, ValueError, KeyError, TypeError):
        return None


def prepare_root(app, metadata, code, installed_root, framework, art, adapters, dex, media_mask=15):
    """Rebuild only the runtime; retain app data across starts and upgrades.

    Caller holds the package lock through preparation AND the live session.
    Never modify the installer's verified code/runtime tree.
    """
    environment = android_framework.install(framework, art)["environment"]
    adapter_manifest = json.loads((adapters / "adapters.json").read_text())
    adapter_names = ["libbifrost-activity.so", "managed/am", "managed/zygote"] + [
        "managed/" + name for name in ("libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
                                      "libGLESv3.so", "libaaudio.so", "libOpenSLES.so",
                                      "mapper.bifrost.so", "vulkan.bifrost.so", BINDER_ATTACH)]
    for name in adapter_names:
        if file_hash(adapters / name) != adapter_manifest["files"].get(name):
            raise ValueError("Modified managed runtime adapter: " + name)
    settings_key = settings_user_key(app.parent)
    import android_settings
    property_overrides = android_settings.properties(android_settings.load(app))
    # Both the app-facing detection (Unity logs and gates on this) and the
    # loader's driver name come from this property; the module above is
    # installed as /vendor/lib64/hw/vulkan.bifrost.so.
    property_overrides.setdefault("ro.hardware.vulkan", "bifrost")
    identity = {"schema": 3, "code": code, "files": metadata["files"], "media_mask": media_mask,
                "framework": file_hash(framework / "bifrost-framework.json"),
                "microg": {package: value[3] for package, value in android_microg.PACKAGES.items()},
                "adapters": {n: adapter_manifest["files"][n] for n in adapter_names},
                "launcher": file_hash(dex), "settings_key": hashlib.sha256(settings_key).hexdigest(),
                "properties": property_overrides}
    managed = app / "managed"
    root = managed / "rootfs"
    if managed.is_symlink() or root.is_symlink():
        raise ValueError("Refusing redirected managed runtime")
    managed.mkdir(exist_ok=True)
    stamp = managed / "runtime.json"
    if root.is_dir() and stamp.is_file():
        saved = json.loads(stamp.read_text())
        if saved.get("identity") == identity:
            # A regular file lies inside the root when its directory does: resolve each directory once (resolving
            # every one of the ~5,000 files took half a second per launch).
            root_real, directories = root.resolve(), {}
            def inside(target):
                real = directories.get(target.parent)
                if real is None: real = directories[target.parent] = target.parent.resolve()
                return real == root_real or root_real in real.parents
            for relative, value in saved["immutable"].items():
                target = root / relative
                if "link" in value:
                    valid = target.is_symlink() and str(target.readlink()) == value["link"]
                else:
                    valid = (not target.is_symlink() and target.is_file() and inside(target) and
                             file_hash(target) == value["sha256"])
                if not valid: raise ValueError("Modified managed runtime file: " + relative)
            backup = managed / "previous-rootfs"
            if backup.exists(): remove_runtime(backup)
            android_framework.protect_shared_libraries(root)
            ensure_app_data(root, metadata["package"])
            return root, environment
    backup = managed / "previous-rootfs"
    if backup.exists() and not root.exists(): backup.rename(root)
    parse_code = parse_cache_code(dex)
    try:
        saved_parse_code = json.loads(stamp.read_text()).get("parse_cache_code") if stamp.is_file() else None
    except ValueError:
        saved_parse_code = None
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=managed) as temporary:
        fresh = Path(temporary) / "rootfs"
        image, image_key = runtime_image(app.parent, framework, adapters, media_mask, {
            "framework": identity["framework"], "microg": identity["microg"],
            "adapters": identity["adapters"], "media_mask": media_mask})
        link_runtime_image(image, fresh)
        android_settings.install_properties(fresh, property_overrides)
        local = fresh / "data/local/tmp"
        local.mkdir(parents=True, exist_ok=True)
        old_cache = root / PARSE_CACHE
        if (parse_code is not None and parse_code == saved_parse_code and old_cache.is_dir()
                and not old_cache.is_symlink() and not root.is_symlink()):
            shutil.copytree(old_cache, fresh / PARSE_CACHE, symlinks=True)
        shutil.copy2(dex, local / "launcher.dex")
        (local / "launcher.dex").chmod(0o444)  # ART rejects writable DEX for ordinary app UIDs.
        shutil.copy2(adapters / "libbifrost-activity.so", local / "libbifrost-activity.so")
        install_settings_key(fresh, settings_key)
        # A system app's code is already in the provisioned image.
        if installed_root is not None:
            shutil.copytree(installed_root / code.lstrip("/"), fresh / code.lstrip("/"))
        # Guest writes are live in this root, including if the host crashes.
        # Copy them into a replacement root before exchanging runtimes.
        data_source = root if root.is_dir() else installed_root
        for relative in ("data/user", "data/user_de", "data/misc/profiles", "data/misc/keystore", "storage"):
            if data_source is None: break
            source = data_source / relative
            target = fresh / relative
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True, symlinks=True)
        for retained in ("data/system/users/0/bifrost-system-settings.xml",
                         "data/system/users/0/bifrost-accounts.json"):
            if data_source is not None and (data_source / retained).is_file():
                shutil.copy2(data_source / retained, fresh / retained)
        ensure_app_data(fresh, metadata["package"])
        (fresh / "data/data").symlink_to("user/0")
        (fresh / "data/dalvik-cache/arm64").mkdir(parents=True, exist_ok=True)
        (fresh / "storage/emulated/0").mkdir(parents=True, exist_ok=True)
        retained_dexopt = retain_dexopt(managed, root, fresh, identity, metadata, code, environment)
        import android_art
        immutable = {name: value for name, value in android_art.inventory(fresh).items()
                     if (name.startswith(("system/", "system_ext/", "product/", "vendor/", "apex/",
                                         "data/app/", "data/local/tmp/"))
                     or name in ("data/system/users/0/bifrost-ssaid-key", "debug_ramdisk/adb_debug.prop"))
                     and not name.startswith((code.lstrip("/") + "/oat/", PARSE_CACHE + "/"))}
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
        pending.write_text(json.dumps({"identity": identity, "immutable": immutable, "parse_cache_code": parse_code,
                                       "image": image_key}, sort_keys=True) + "\n")
        pending.replace(stamp)
        if retained_dexopt is not None:
            save_dexopt_stamp(managed, retained_dexopt[0], root, code, retained_dexopt[1])
        if backup.exists(): remove_runtime(backup)
        prune_runtime_images(app.parent, image_key)
    return root, environment


def launch_managed(app, metadata, code, installed_root, emu, no_jit=False,
                   window_plugin=None, emu_args=(), timeout=0, trace_apk=None, trace_after=0, trace_duration=15,
                   process_session=None):
    if process_session is None: raise ValueError("Managed launch requires an app process session")
    dex = launcher_dex(ROOT / "build/android-managed-launcher")
    process_session.update(phase="Checking media services")
    from asgard import decoder_mask
    broker = emu_args[emu_args.index('--android-binder-broker') + 1] if '--android-binder-broker' in emu_args else None
    media_mask = decoder_mask(emu, broker)
    # The validation probe can wait for its timeout; overlap it with root setup.
    with ThreadPoolExecutor(1) as pool:
        network = pool.submit(host_network_environment)
        process_session.update(phase="Preparing Android runtime")
        root, guest_env = prepare_root(app, metadata, code, installed_root,
                                      ROOT / "build/android-framework/rootfs",
                                      ROOT / "build/android-art/rootfs",
                                      ROOT / "build/android-adapters", dex, media_mask)
        guest_env = dict(guest_env, **network.result(), **window_mode_environment(os.environ))
        guest_env["PATH"] = "/system/bin:/apex/com.android.runtime/bin"
        session_preload = {"LD_PRELOAD": "/system/lib64/" + BINDER_ATTACH}
    process_session.update(phase="Optimizing Android app code")
    microg_dexopt(app.parent, root, emu, guest_env, no_jit)
    # A system app (microG) has no installed base.apk to compile; ART's JIT runs it.
    if not metadata.get("system"):
        dexopt(app / "managed", root, metadata, code, emu, guest_env, no_jit)
    process_session.update(phase="Starting Android app process")
    # The Google accounts signed in on this device, with microG's registration for them.
    android_google.import_shared(app.parent, root)
    # The user's choice about letting microG create a Play Games profile (off until chosen).
    android_google.apply_games_profile(app.parent, root)
    # An app's request for one of microG's Activities (this session is the system app's) or none.
    request = metadata.get("request") if metadata.get("system") else None
    if metadata.get("system"): android_google.stage_request(root, request)
    completion = android_google.ActivityCompletion(app.parent, root, request) if request else None
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
        environment = dict(os.environ, **READONLY_ENVIRONMENT)
        environment.pop("BIFROST_ROOT", None)
        # The persistent JIT cache (BIFROST_JIT_DISK_CACHE) keeps one directory and
        # budget per app, shared by the session's helper processes.
        environment.setdefault("BIFROST_JIT_CACHE_APP", metadata["package"])
        if not metadata.get("system"):  # the window's application id: the entry of the same name gives its taskbar button the icon
            import android_desktop
            for name, value in android_desktop.window_environment(metadata["package"]).items(): environment.setdefault(name, value)
        if trace_apk and trace_apk != "activity":
            for flag in ("BIFROST_BINDER_TRACE", "BIFROST_FAULT_TRACE", "BIFROST_FRAME_TRACE"):
                environment[flag] = "1"
        environment["BIFROST_GUEST_ENV"] = ";".join(f"{k}={v}" for k, v in {**guest_env, **session_preload}.items())
        log_path, logd_path = process_session.logs()
        print(f"Launching {metadata['package']} (log: {log_path})", flush=True)
        # A socket pathname can remain after a killed host process. No other
        # session for this package is live while the caller holds its lock.
        for name in ("logdw", "dnsproxyd"):
            (root / "dev/socket" / name).unlink(missing_ok=True)
        with log_path.open("wb") as log, logd_path.open("wb") as sink, ExitStack() as services:
            relay = None if metadata.get("system") else android_google.ActivityRelay(
                app.parent, root, android_google.session_options(emu, emu_args, no_jit, window_plugin))
            process_host = services.enter_context(closing(ProcessHost(guest_root, emu,
                    list(emu_args) + (["--no-jit"] if no_jit else [])
                    + (["--android-window-plugin", str(window_plugin)] if window_plugin else []),
                environment, "/data/local/tmp/launcher.dex", log_path.parent / "processes", trace_args,
                media_mask=media_mask, activities=relay, task_session=process_session, state=app.parent,
                activity_finished=completion, zygote=True,
                media_destination=pictures_directory() / "Bifrost"
                    / media_folder(metadata.get("label"), metadata["package"]))))
            process_session.task_control = process_host.task
            environment = process_host.environment
            command[1:1] = ["--android-binder-broker", process_host.socket]
            services.enter_context(closing(LogdReceiver(guest_root, sink)))
            services.enter_context(closing(DnsProxy(guest_root, environment.get("BIFROST_ANDROID_OFFLINE") == "1")))
            status = 1
            try:
                status = process_session.run(command, stdout=log, env=environment, timeout=timeout)
            finally:
                (root / "dev/socket/logdw").unlink(missing_ok=True)
                if completion: completion()
                else: android_google.publish_shared(app.parent, root)
                collect_trace(trace_directory, app / "managed/traces")
                if trace_directory is not None: shutil.rmtree(trace_directory)
        if status:
            print(f"Managed app exited with status {status}; see {log_path}", file=sys.stderr)
        else:
            from android_launcher import quit_reason  # an exit with status 0 is silent unless the log says why it quit
            if reason := quit_reason(log_path):
                print(f"Managed app: {reason} See {log_path}", file=sys.stderr)
        # What this session taught ART's profile is compiled now, for the next launch, not before it.
        try:
            optimize_after_session(app / "managed", root, metadata, code, emu, guest_env, no_jit)
        except Exception as error:  # the session's own result stands; the next launch checks the code again
            print(f"Optimizing {metadata['package']} after the session failed: {error}", file=sys.stderr)
        return status if status >= 0 else 128 - status


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build",))
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-managed-launcher")
    parser.add_argument("--force", action="store_true", help="build even when the launcher matches its sources")
    args = parser.parse_args()
    if not args.force and launcher_current(args.output):
        print(f"Managed launcher is up to date: {args.output}")
    else:
        build_launcher(args.output)
