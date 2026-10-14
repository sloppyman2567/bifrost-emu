#!/usr/bin/env python3
"""Asgard: Bifrost's Android compatibility layer and shared process realm."""
import argparse
import fcntl
import hashlib
import json
import os
import shutil
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import time
import tempfile
import xml.etree.ElementTree as ET

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
                    status['media'] = {'state': 'unavailable', 'reason': 'Broker lacks media status support'}
            return status
        except (OSError, ValueError):
            pass
    return {"name": "Asgard", "state": "stopped", "socket": str(path)}


def ensure_broker(state, emu):
    directory = state / ".asgard"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (directory / "broker.lock").open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        status = broker_status(state)
        if status["state"] == "running":
            return status
        if status["state"] == "incompatible":
            raise ValueError("Existing Asgard broker uses an older protocol; stop its app sessions, then run realm stop and realm start")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-root", type=Path, default=ROOT / "android-state")
    parser.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    parser.add_argument("command", choices=("realm", "run", "start", "list", "apps", "status",
                                           "stop", "logs", "install", "launcher", "probe"))
    args, remainder = parser.parse_known_args()
    state = args.state_root.resolve()
    try:
        if args.command == "realm":
            operation = remainder[0] if remainder else "status"
            if len(remainder) > 1 or operation not in ("start", "status", "stop"):
                parser.error("realm accepts start, status or stop")
            if operation == "start":
                value = ensure_broker(state, args.emu.resolve())
            else:
                value = broker_status(state)
                if operation == "stop" and value["state"] in ("running", "incompatible"):
                    control(Path(value["socket"]), stop=True, version=value.get("protocol", VERSION))
                    value["state"] = "stopped"
            print(json.dumps(value))
            return 0
        command = [sys.executable, str(ROOT / "Scripts/Android/android_package.py"),
                   "--state-root", str(state), args.command, *remainder]
        if args.command in ("run", "start"):
            broker = ensure_broker(state, args.emu.resolve())
            command += ["--binder-broker", broker["socket"]]
        if args.command in ("run", "start", "probe"):
            command += ["--emu", str(args.emu.resolve())]
        return subprocess.call(command)
    except (OSError, ValueError) as error:
        print(f"Asgard: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
