#!/usr/bin/env python3
"""Bounded app_process64 framework probe; success requires FRAMEWORK_OK."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from android_framework import install


PRIORITIES = "??VDIWEFS"


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


def run(emu, runtime, art_root, probe, mode, output, timeout, art_options=(), properties=(), services=False,
        libraries=()):
    metadata = install(runtime, art_root)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bifrost-framework-run-") as temporary:
        root = Path(temporary) / "rootfs"
        shutil.copytree(runtime, root, symlinks=True)
        (root / "data/local/tmp").mkdir(parents=True)
        (root / "data/dalvik-cache/arm64").mkdir(parents=True)
        shutil.copy2(probe, root / "data/local/tmp/probe.dex")
        for library in libraries:
            if not library.name.endswith(".so") or not library.is_file():
                raise ValueError("Probe libraries must be regular .so files")
            shutil.copy2(library, root / "data/local/tmp" / library.name)
        if properties:
            # The serializer keeps the first definition of a key, so
            # prepended overrides beat the image's build.prop.
            build_prop = root / "system/build.prop"
            text = build_prop.read_text() if build_prop.exists() else ""
            build_prop.unlink(missing_ok=True)  # may be a hard link into the source root
            build_prop.write_text("".join(p + "\n" for p in properties) + text)
        # ART logs to stderr: there is no logd socket for liblog here.
        program = ["-Xuse-stderr-logger", *art_options,
                   "-Djava.class.path=/data/local/tmp/probe.dex", "/system/bin", "FrameworkProbe"]
        environment = dict(os.environ)
        environment.pop("BIFROST_ROOT", None)
        guest_env = metadata["environment"]
        if mode == "qemu":
            # QEMU's AT_EXECFN carries the host-prefixed path; map it to the
            # same linker section in this temporary copy only.
            config = root / "linkerconfig/ld.config.txt"
            config.write_text(f"dir.art = {root}/system/bin\n" + config.read_text())
            # Bifrost serves /dev/__properties__ itself; give QEMU the same
            # serialized area. Bionic requires it to be root-owned, so run
            # QEMU in a user namespace where our files appear as uid 0.
            dumper = ROOT / "build/dump_android_properties"
            (root / "dev").mkdir(exist_ok=True)
            subprocess.run([str(dumper), str(root), str(root / "dev/__properties__")],
                           check=True, capture_output=True, timeout=30)
            (root / "dev/__properties__").chmod(0o444)
            arguments = ["unshare", "-r", str(emu), "-L", str(root)]
            for key, value in guest_env.items():
                arguments += ["-E", f"{key}={value}"]
            arguments += [str(root / "system/bin/app_process64")] + program
        else:
            environment["BIFROST_GUEST_ENV"] = ";".join(f"{k}={v}" for k, v in guest_env.items())
            environment["BIFROST_TIER2"] = "0"
            arguments = [str(emu)] + (["--no-jit"] if mode == "interp" else [])
            if services:
                arguments += ["--android-services"]
            arguments += ["--android-runtime", "--rootfs", str(root), "/system/bin/app_process64"] + program
        log_path = output / (mode + ".log")
        started = time.monotonic()
        timed_out = False
        with log_path.open("wb") as log, (output / (mode + ".logd")).open("wb") as logd_sink:
            logd = LogdReceiver(root, logd_sink)
            child = subprocess.Popen(arguments, stdout=log, stderr=subprocess.STDOUT,
                                     env=environment, start_new_session=True)
            try:
                child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
            finally:
                if child.poll() is None:
                    try:
                        os.killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    child.wait()
                logd.close()
        text = log_path.read_bytes().decode(errors="replace")
        passed = child.returncode == 0 and not timed_out and "FRAMEWORK_OK" in text
        result = {"mode": mode, "passed": passed, "returncode": child.returncode,
                  "timed_out": timed_out, "elapsed_seconds": round(time.monotonic() - started, 3)}
        (output / (mode + ".json")).write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        return passed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emu", type=Path, help="Emulator; defaults to Bifrost or qemu-aarch64 for qemu mode")
    parser.add_argument("--runtime-root", type=Path, default=ROOT / "build/android-framework/rootfs")
    parser.add_argument("--art-root", type=Path, default=ROOT / "build/android-art/rootfs")
    parser.add_argument("--probe", type=Path, default=ROOT / "build/framework-probe/classes.dex")
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-framework-results")
    parser.add_argument("--mode", choices=("jit", "interp", "qemu"), default="jit")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--art-option", action="append", default=[], metavar="OPTION",
                        help="Extra ART runtime option (repeatable), e.g. -Xint")
    parser.add_argument("--prop", action="append", default=[], metavar="KEY=VALUE",
                        help="System property override (repeatable)")
    parser.add_argument("--services", action="store_true", help="Enable Bifrost's Binder service registry")
    parser.add_argument("--library", type=Path, action="append", default=[], help="Copy a probe JNI library into /data/local/tmp")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 300:
        parser.error("timeout must be between 1 and 300 seconds")
    if args.emu is None:
        if args.mode == "qemu":
            qemu = shutil.which("qemu-aarch64")
            if not qemu:
                parser.error("qemu-aarch64 is required for qemu mode; pass --emu")
            args.emu = Path(qemu)
        else:
            args.emu = ROOT / "bifrost-emu"
    sys.exit(0 if run(args.emu.resolve(), args.runtime_root.resolve(), args.art_root.resolve(),
                      args.probe.resolve(), args.mode, args.output.resolve(), args.timeout,
                      args.art_option, args.prop, args.services, args.library) else 1)
