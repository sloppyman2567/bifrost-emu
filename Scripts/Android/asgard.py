#!/usr/bin/env python3
"""Asgard: Bifrost's Android compatibility layer and shared process realm."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import time

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


def control(path, stop=False, version=VERSION):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stream:
        stream.settimeout(2)
        stream.connect(str(path))
        operation = 6 if stop else 5
        stream.sendall(HEADER.pack(MAGIC, version, operation, 0, 0, 0))
        magic, returned_version, reply, size, _, _ = HEADER.unpack(receive(stream, HEADER.size))
        if (magic, returned_version, reply, size) != (MAGIC, version, operation, 16):
            raise ValueError("Invalid Asgard broker reply")
        result, chunks, files = struct.unpack("<qII", receive(stream, size))
        if result or chunks or files:
            raise ValueError("Asgard broker rejected control request")


def broker_status(state):
    path = realm_socket(state)
    for version in (VERSION, 1):
        try:
            control(path, version=version)
            return {"name": "Asgard", "state": "running" if version == VERSION else "incompatible",
                    "protocol": version, "socket": str(path)}
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
