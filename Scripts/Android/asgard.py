#!/usr/bin/env python3
"""Asgard: Bifrost's Android compatibility layer and shared process realm."""
import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import signal
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import time
import tempfile
import xml.etree.ElementTree as ET

import android_paths

ROOT = Path(__file__).resolve().parents[2]
HEADER = struct.Struct("<IIIIQQ")
MAGIC, VERSION = 0x41534744, 2


def realm_socket(state):
    directory = Path("/tmp") / f"asgard-{os.getuid()}"
    directory.mkdir(mode=0o700, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("Asgard runtime directory must be private and owned by this user")
    identity = hashlib.sha256(str(state.resolve()).encode()).hexdigest()[:24]
    return directory / (identity + ".sock")


def receive(stream, length):
    data = bytearray()
    while len(data) < length:
        part = stream.recv(length - len(data))
        if not part:
            raise ConnectionError("Asgard broker disconnected")
        data.extend(part)
    return data


def control(path, stop=False, version=VERSION, operation=None, body=b""):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
        stream.settimeout(45 if operation == 8 else 5)
        stream.connect(str(path))
        operation = operation if operation is not None else 6 if stop else 5
        stream.sendall(HEADER.pack(MAGIC, version, operation, len(body), 0, 0) + body)
        magic, returned_version, reply, size, _, _ = HEADER.unpack(receive(stream, HEADER.size))
        if (magic, returned_version, reply) != (MAGIC, version, operation) or not 16 <= size <= 1024 * 1024:
            raise ValueError("Invalid Asgard broker reply")
        data = receive(stream, size)
        result, chunks, files = struct.unpack("<qII", data[:16])
        if result or chunks or files:
            raise ValueError("Asgard broker rejected control request")
        return bytes(data[16:])


def media_status(path):
    return json.loads(control(path, operation=7))


def decoder_mask(emu, broker=None):
    if broker is not None:
        status = media_status(broker)
        if status['state'] != 'disabled':
            return status['routing_mask']
    result = subprocess.run([str(emu), '--media-capabilities'], check=True, capture_output=True, text=True, timeout=30)
    report = json.loads(result.stdout)
    key = 'gpu_probe_passed' if os.environ.get('BIFROST_MEDIA_BACKEND') == 'gpu' else 'cpu_probe_passed'
    return sum(1 << index for index, codec in enumerate(report['codecs']) if codec[key])


def configure_media(root, mask):
    """Select verified components before the managed runtime is inventoried."""
    names = ('vp8', 'vp9', 'h264', 'hevc')
    config = root / 'vendor/etc/media_codecs.xml'
    tree = ET.parse(config)
    for codec in tree.iter('MediaCodec'):
        name = codec.get('name', '')
        matches = [index for index, family in enumerate(names) if name == f'c2.goldfish.{family}.decoder']
        if len(matches) != 1:
            raise ValueError('Unexpected host codec in verified runtime')
        codec.set('enabled', 'true' if mask & (1 << matches[0]) else 'false')
    ET.indent(tree)
    tree.write(config, encoding='utf-8', xml_declaration=True)
    properties = root / 'debug_ramdisk/adb_debug.prop'
    properties.parent.mkdir(parents=True, exist_ok=True)
    # This file is generated runtime configuration, never app-persistent data.
    generated = ('media.c2.hal.selection=aidl\nro.vendor.api_level=202404\n'
                          'debug.stagefright.c2-poolmask=786432\n'
                          f'ro.boot.qemu.hwcodec.avcdec={2 if mask & 4 else 0}\n'
                          f'ro.boot.qemu.hwcodec.hevcdec={2 if mask & 8 else 0}\n'
                          f'ro.boot.qemu.hwcodec.vpxdec={2 if mask & 3 else 0}\n')
    keys = {line.split('=', 1)[0] for line in generated.splitlines()}
    retained = [line for line in properties.read_text().splitlines()
                if line.split('=', 1)[0] not in keys] if properties.exists() else []
    properties.write_text('\n'.join(retained) + '\n' + generated)


def ensure_media(path, emu, framework, art, adapters, mask):
    """Attach one independent verified service root to a broker, never to an app."""
    path, framework, art, adapters = map(Path, (path, framework, art, adapters))
    with Path(str(path) + '.media.lock').open('a+b') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        status = media_status(path)
        if status['state'] == 'disabled':
            import android_framework
            android_framework.install(framework, art)
            manifest = json.loads((adapters / 'adapters.json').read_text())
            mapper = adapters / 'managed/mapper.bifrost.so'
            if hashlib.sha256(mapper.read_bytes()).hexdigest() != manifest['files']['managed/mapper.bifrost.so']:
                raise ValueError('Modified media mapper adapter')
            root = Path(str(path) + '.media-root')
            if root.exists() or root.is_symlink():
                # Only replace our own previous service tree, under the realm lock.
                marker = root / '.asgard-media-root'
                if root.is_symlink() or not marker.is_file() or marker.read_text() != str(path):
                    raise ValueError('Refusing to replace foreign media service root')
                shutil.rmtree(root)
            with tempfile.TemporaryDirectory(prefix='.media-', dir=path.parent) as temporary:
                fresh = Path(temporary) / 'rootfs'
                shutil.copytree(framework, fresh, symlinks=True)
                target = fresh / 'vendor/lib64/hw/mapper.bifrost.so'
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(mapper, target)
                configure_media(fresh, mask)
                (fresh / '.asgard-media-root').write_text(str(path))
                fresh.rename(root)
            control(path, operation=8, body=struct.pack('<I', mask) + str(root).encode())
        deadline = time.monotonic() + 45
        while True:
            status = media_status(path)
            if status.get('routing_mask') != mask:
                raise ValueError('Media routing changed while preparing the app; retry launch')
            if status['state'] == 'ready':
                return status
            if status['state'] in ('failed', 'stopped') or time.monotonic() >= deadline:
                raise ValueError(f'Media services did not become ready: {status}; logs: {path}.media-*.log')
            time.sleep(.1)


def broker_status(state):
    path = realm_socket(state)
    for version in (VERSION, 1):
        try:
            control(path, version=version)
            status = {"name": "Asgard", "state": "running" if version == VERSION else "incompatible",
                      "protocol": version, "socket": str(path)}
            if version == VERSION:
                try:
                    status['media'] = media_status(path)
                except (OSError, ValueError):
                    # Brokers built before STATUS/ENABLE_MEDIA/PROCESS_STATE
                    # answer the same protocol version but reject these ops.
                    status['state'] = 'incompatible'
                    status['media'] = {'state': 'unavailable', 'reason': 'Broker predates media and process-state support'}
            return status
        except (OSError, ValueError):
            pass
    return {"name": "Asgard", "state": "stopped", "socket": str(path)}


def broker_pid(path):
    """The process that owns the broker's control socket (its peer credentials), or None."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
            stream.settimeout(2)
            stream.connect(str(path))
            pid, _, _ = struct.unpack("3i", stream.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                                              struct.calcsize("3i")))
    except OSError:
        return None
    return pid or None


def same_program(pid, emu):
    """Whether process `pid` runs the same bytes as the emulator file `emu`.

    /proc/PID/exe still opens the file a process started from after it was replaced (an upgrade installs a new
    one under the same name). Unknown, such as another user's process, counts as the same: do not disturb it."""
    try:
        running, current = os.stat(f"/proc/{pid}/exe"), os.stat(emu)
        if (running.st_dev, running.st_ino) == (current.st_dev, current.st_ino):
            return True
        if running.st_size != current.st_size:
            return False
        digests = []
        for name in (f"/proc/{pid}/exe", str(emu)):
            digest = hashlib.sha256()
            with open(name, "rb") as stream:
                for chunk in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(chunk)
            digests.append(digest.digest())
        return digests[0] == digests[1]
    except OSError:
        return True


def broker_clients(path):
    """Process ids of the emulator processes (apps, helpers) that joined the broker at `path`."""
    clients = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            words = Path(f"/proc/{entry}/cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        # Helpers bound to a parent (media codecs) are not clients; supervised Android service processes are.
        if b"--android-binder-broker" in words and str(path).encode() in words \
                and (b"--asgard-service-parent" not in words or b"--service-child" in words) \
                and b"--asgard-broker" not in words:
            clients.append(int(entry))
    return clients


def process_alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()[0] != "Z"
    except (OSError, IndexError):
        return False


def stop_clients(path, grace=3.0):
    """End the emulator processes still joined to the broker at `path`: SIGTERM, then SIGKILL after `grace` seconds."""
    pids = broker_clients(path)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline and any(process_alive(pid) for pid in pids):
        time.sleep(0.05)
    for pid in pids:
        if process_alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    return pids


def stop_realm(state, force=False):
    """Stop the realm's broker. `force` first ends the app processes still joined to it, which otherwise run on
    with nothing to talk to."""
    value = broker_status(state)
    if value["state"] in ("running", "incompatible"):
        if force:
            value["ended"] = stop_clients(value["socket"])
        elif broker_clients(value["socket"]):
            print("Apps are still joined to this realm and lose their services; "
                  "use 'realm stop --force' to end them too", file=sys.stderr)
        try:
            control(Path(value["socket"]), stop=True, version=value.get("protocol", VERSION))
        except (OSError, ValueError):
            pass
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and broker_status(state)["state"] != "stopped":
            time.sleep(0.05)
        value["state"] = broker_status(state)["state"]
    return value


def ensure_broker(state, emu):
    directory = state / ".asgard"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (directory / "broker.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        status = broker_status(state)
        if status["state"] == "running":
            pid = broker_pid(status["socket"])
            if pid is None or same_program(pid, emu):
                return status
            # An upgrade replaced the emulator while its broker kept running: the protocol number can match
            # while the Binder behavior no longer does, and a new app then dies on its first service call.
            if broker_clients(status["socket"]):
                raise ValueError("The Asgard broker was started by an older build of bifrost-emu and apps are "
                                 "still using it; close them, or run: asgard realm stop --force")
            print("Restarting the Asgard broker: it was started by an older build of bifrost-emu", flush=True)
            status = stop_realm(state)
            if status["state"] != "stopped":
                raise ValueError("The old Asgard broker did not stop; run: asgard realm stop --force")
        if status["state"] == "running":
            return status
        if status["state"] == "incompatible":
            raise ValueError("Existing Asgard broker is older than this emulator; stop its app sessions, then run realm stop and realm start")
        path = Path(status["socket"])
        if path.exists():
            info = path.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise ValueError("Refusing to replace a foreign Asgard socket")
            path.unlink()
        with (directory / "broker.log").open("ab", buffering=0) as log:
            child = subprocess.Popen([str(emu), "--asgard-broker", str(path)],
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise ValueError(f"Asgard broker exited with status {child.returncode}; see {directory / 'broker.log'}")
            status = broker_status(state)
            if status["state"] == "running":
                return status
            time.sleep(0.025)
        child.terminate()
        child.wait(timeout=5)
        raise ValueError("Asgard broker did not become ready")


PACKAGE_FILES = (".apk", ".apkm", ".apks", ".xapk")
MAX_PACKAGE_BYTES = 2 * 1024 * 1024 * 1024  # android_package.MAX_TOTAL: larger files fail to install
# Debug channels, like WINEDEBUG: each turns on trace switches that Source/System/Core/DebugFlags.hpp
# reads when it is present. Only logging switches belong here, never ones that change execution.
DEBUG_CHANNELS = {
    "audio": ("BIFROST_AUDIO_TRACE",),
    "binder": ("BIFROST_BINDER_TRACE",),
    "dynlink": ("BIFROST_DYNLINK_TRACE",),
    "fault": ("BIFROST_FAULT_TRACE", "BIFROST_TRACE_CRASH"),
    "frame": ("BIFROST_FRAME_TRACE",),
    "futex": ("BIFROST_FUTEX_TRACE",),
    "input": ("BIFROST_INPUT_TRACE",),
    "media": ("BIFROST_MEDIA_TRACE",),
    "mmap": ("BIFROST_TRACE_MMAP", "BIFROST_TRACE_MADVISE"),
    "poll": ("BIFROST_PPOLL_TRACE",),
    "process": ("BIFROST_PROCESS_TRACE", "BIFROST_EXEC_TRACE"),
    "thunk": ("BIFROST_THUNK_TRACE",),
    "x11": ("BIFROST_XTRACE",),
    "all": ("BIFROST_TRACE",),
}


def debug_environment(channels):
    """Environment switches for a comma-separated channel list."""
    names = [name.strip() for name in channels.split(",") if name.strip()]
    unknown = [name for name in names if name not in DEBUG_CHANNELS]
    if unknown:
        raise ValueError(f"Unknown debug channel {unknown[0]!r}; channels: {', '.join(DEBUG_CHANNELS)}")
    return {variable: "1" for name in names for variable in DEBUG_CHANNELS[name]}


def file_sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def installed_from(state, source):
    """The package already installed from exactly this file, or None.

    `install` records the SHA-256 of the file it installed. A match whose
    root still exists means installing would only re-verify and re-extract
    the same bytes; the run path re-checks every installed file anyway."""
    if not source.is_file() or source.stat().st_size > MAX_PACKAGE_BYTES:
        return None
    wanted = None
    for metadata_path in sorted(state.glob("*/package.json")):
        try:
            metadata = json.loads(metadata_path.read_text())
        except (OSError, ValueError):
            continue
        recorded = metadata.get("source_sha256") if isinstance(metadata, dict) else None
        if not recorded or metadata.get("package") != metadata_path.parent.name \
                or not (metadata_path.parent / "rootfs").is_dir():
            continue
        wanted = wanted or file_sha256(source)
        if recorded == wanted:
            return metadata["package"]
    return None


def install_for_run(script, state, arguments):
    """`run app.apk` installs the file (a no-op for an identical, installed one) and runs its package."""
    downgrade = [arg for arg in arguments if arg in ("-d", "--allow-downgrade")]
    arguments = [arg for arg in arguments if arg not in ("-d", "--allow-downgrade")]
    index = next((i for i, value in enumerate(arguments)
                  if Path(value).suffix.lower() in PACKAGE_FILES and Path(value).is_file()), None)
    if index is None:
        if downgrade:
            raise ValueError("--allow-downgrade requires an APK/APKM file to install")
        return arguments
    package = installed_from(state, Path(arguments[index]))
    if package:
        print(f"Already installed {package}; skipping installation", flush=True)
        return arguments[:index] + [package] + arguments[index + 1:]
    installed = subprocess.run([sys.executable, script, "--state-root", str(state), "install",
                                *(["--allow-downgrade"] if downgrade else []), arguments[index]],
                               stdout=subprocess.PIPE, text=True)
    print(installed.stdout, end="", flush=True)
    if installed.returncode:
        raise ValueError(f"Could not install {arguments[index]}")
    match = re.search(r"^Installed (\S+) ", installed.stdout, re.M)
    if not match:
        raise ValueError("The installer did not report a package name")
    return arguments[:index] + [match[1]] + arguments[index + 1:]


# One line per command for `asgard --help`. android_package.py implements all but `realm`; a test checks the two agree.
COMMANDS = {
    "install": "install an .apk, .apkm, .apks or .xapk",
    "run": "run an app in the foreground (an APK file is installed first)",
    "start": "start apps in the background",
    "list": "installed apps and what each is doing (also: apps)",
    "apps": "same as list",
    "status": "whether an app is running",
    "stop": "ask a running app to stop",
    "resume": "bring a backgrounded app back",
    "logs": "the end of an app's session log",
    "config": "show or change an app's saved settings",
    "uninstall": "remove an app with its code and all its data",
    "clear-data": "reset an app's saves and settings; it stays installed",
    "launcher": "open the desktop app library",
    "desktop": "put installed apps in the desktop's application menu, or take them out",
    "refresh-icons": "read icons of apps installed by an older version",
    "compat": "what is known to work, from the compatibility records",
    "google": "sign in to Google (microG) and manage accounts",
    "realm": "the shared service process apps join: start, stop, restart, status",
    "probe": "load an app's native libraries without starting it (diagnostics)",
}
EXAMPLES = """\
examples:
  asgard install game.apk        install an app (also .apkm, .apks, .xapk)
  asgard run game.apk            install it if needed, then run it
  asgard list                    installed apps and what each is doing
  asgard run arknights           part of a package name or an app's title is enough
  asgard config arknights        its saved settings
  asgard launcher                the desktop library
  asgard COMMAND --help          the options of one command
"""


def help_text(parser):
    width = max(map(len, COMMANDS))
    return parser.format_help() + "\ncommands:\n" + "".join(
        f"  {name:<{width}}  {text}\n" for name, text in COMMANDS.items() if name != "apps") + "\n" + EXAMPLES


def main():
    # -h belongs to the command when one is given: `asgard run -h` lists the options of `run`.
    parser = argparse.ArgumentParser(prog="asgard", description=__doc__, add_help=False, usage="asgard [options] COMMAND [...]")
    parser.add_argument("--state-root", type=Path, default=android_paths.state_root())
    parser.add_argument("--emu", type=Path, default=android_paths.emulator())
    parser.add_argument("--debug", metavar="CHANNELS", default="",
                        help="trace channels, comma-separated: " + ", ".join(DEBUG_CHANNELS) +
                             " (a running realm broker keeps the channels it started with)")
    parser.add_argument("command", nargs="?", metavar="COMMAND")
    args, remainder = parser.parse_known_args()
    if args.command is None or args.command == "help":
        if args.command is None and any(value not in ("-h", "--help") for value in remainder):
            parser.error(f"unrecognized arguments: {' '.join(remainder)}")
        print(help_text(parser), end="")
        return 0
    if args.command not in COMMANDS:
        import difflib
        close = difflib.get_close_matches(args.command, COMMANDS, n=1)
        parser.error(f"unknown command {args.command!r}" + (f" (did you mean {close[0]!r}?)" if close else "")
                     + "; asgard --help lists the commands")
    state = args.state_root.resolve()
    try:
        os.environ.update(debug_environment(args.debug))
        if args.command == "realm":
            operation = remainder[0] if remainder else "status"
            force = remainder[1:] == ["--force"] and operation in ("stop", "restart")
            if operation not in ("start", "status", "stop", "restart") or (len(remainder) > 1 and not force):
                parser.error("realm accepts start, status, stop [--force] or restart [--force]")
            if operation == "start":
                value = ensure_broker(state, args.emu.resolve())
            elif operation == "restart":
                stopped = stop_realm(state, force)
                value = ensure_broker(state, args.emu.resolve())
                if "ended" in stopped:
                    value["ended"] = stopped["ended"]
            elif operation == "stop":
                value = stop_realm(state, force)
            else:
                value = broker_status(state)
            print(json.dumps(value))
            return 0
        script = str(ROOT / "Scripts/Android/android_package.py")
        if args.command == "run":
            remainder = install_for_run(script, state, remainder)
        command = [sys.executable, script, "--state-root", str(state), args.command, *remainder]
        # Opening microG's window (sign-in, settings) is a session like any app's.
        session = args.command in ("run", "start") or (args.command == "google"
                                                       and remainder[:1] in (["sign-in"], ["settings"]))
        if session:
            broker = ensure_broker(state, args.emu.resolve())
            command += ["--binder-broker", broker["socket"]]
        if session or args.command == "probe":
            command += ["--emu", str(args.emu.resolve())]
        # Replace this process: a wrapper that stayed as the parent would leave the real command running when it
        # alone was signalled or killed (a library window closing, `kill`, a test's terminate()).
        sys.stdout.flush(); sys.stderr.flush()
        os.execv(sys.executable, command)
    except (OSError, ValueError) as error:
        print(f"Asgard: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
