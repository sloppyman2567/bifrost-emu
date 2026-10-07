#!/usr/bin/env python3
"""Bounded app_process64 framework probe; success requires FRAMEWORK_OK."""
import argparse
import json
import os
from pathlib import Path
import re
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
    depends on it for every export it does not thunk.
    """
    lib64 = root / "system/lib64"
    for adapter in sorted(adapters.glob("*.so")):
        real = lib64 / adapter.name
        moved = lib64 / ("re_" + adapter.name[3:])
        shutil.copyfile(real, moved)  # new inode; provisioned files are hardlinked
        set_soname(moved, moved.name)
        real.unlink()
        shutil.copyfile(adapter, real)


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
        libraries=(), main_class="FrameworkProbe", app=None, probe_args=(), managed_graphics=None):
    metadata = install(runtime, art_root)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bifrost-framework-run-") as temporary:
        root = Path(temporary) / "rootfs"
        shutil.copytree(runtime, root, symlinks=True)
        (root / "data/local/tmp").mkdir(parents=True)
        (root / "data/dalvik-cache/arm64").mkdir(parents=True)
        (root / "storage/emulated/0").mkdir(parents=True, exist_ok=True)
        shutil.copy2(probe, root / "data/local/tmp/probe.dex")
        for library in libraries:
            if not library.name.endswith(".so") or not library.is_file():
                raise ValueError("Probe libraries must be regular .so files")
            shutil.copy2(library, root / "data/local/tmp" / library.name)
        if app:
            # An installed code directory (scripts/android_package.py install):
            # /data/app/<package>-<id>, plus the per-user data directories
            # Android creates at install time.
            package = app.name.rsplit("-", 1)[0]
            shutil.copytree(app, root / "data/app" / app.name)
            for directory in (root / "data/app" / app.name).rglob("*"):
                if directory.is_dir():
                    directory.chmod(0o755)  # installed trees are read-only
            (root / "data/app" / app.name).chmod(0o755)
            for user in ("data/user/0", "data/user_de/0"):
                (root / user / package).mkdir(parents=True)
            (root / "data/data").symlink_to("user/0")
        if managed_graphics:
            install_managed_graphics(root, managed_graphics)
        if properties:
            # Loaded last, as Android's init does with this debug file.
            (root / "debug_ramdisk").mkdir(exist_ok=True)
            (root / "debug_ramdisk/adb_debug.prop").write_text("".join(p + "\n" for p in properties))
        # ART logs to stderr: there is no logd socket for liblog here.
        program = ["-Xuse-stderr-logger", *art_options,
                   "-Djava.class.path=/data/local/tmp/probe.dex", "/system/bin", main_class, *probe_args]
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
            if "--play" in probe_args:
                environment["BIFROST_FRAME_TRACE"] = "1"
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
                child.wait(timeout=timeout or None)
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
        app_mode = "--app" in probe_args
        play = "--play" in probe_args
        markers = ("APP_CLASSLOADER_OK", "APP_STORAGE_READY", "APP_PREFERENCES_READY",
                   "APP_APPLICATION_STARTED", "APP_ACTIVITY_CREATED", "FRAMEWORK_OK") \
            if app_mode else ("FRAMEWORK_OK",)
        if services and main_class == "ActivityProbe":
            markers += ("ACTIVITY_SERVICE_REGISTRATION_OK", "ACTIVITY_SENSOR_SERVICE_OK")
        if play:
            markers += ("APP_WINDOW_OPEN", "APP_AUDIO_ROUTING_READY", "APP_RESUMED", "APP_SURFACE_READY")
        # A play session ends when its window closes (exit 0) or its time runs out.
        # A worker can die while the UI thread keeps pumping until the timeout.
        # Also catch Unity's fatal signal forwarding when an outer debugger
        # exits successfully after the guest crashes.
        guest_failure = bool(re.search(r"\[bifrost-emu\] thread \d+: exception:|"
                                       r"E CRASH\s*: Forwarding signal \d+|"
                                       r"E AndroidRuntime: .*FATAL EXCEPTION", text))
        frames = max((int(n) for n in re.findall(r"\[frame\] eglSwapBuffers: (\d+) frames", text)),
                     default=0)
        finished = child.returncode == 0 or (play and timed_out)
        passed = finished and not guest_failure and (play or not timed_out) \
            and all(marker in text for marker in markers) and (not play or frames >= 5)
        result = {"mode": mode, "passed": passed, "returncode": child.returncode,
                  "guest_failure": guest_failure,
                  "frames_presented": frames,
                  "timed_out": timed_out, "elapsed_seconds": round(time.monotonic() - started, 3),
                  "checkpoint": "app_play" if play else "app_on_create" if app_mode else "framework",
                  "reached_checkpoints": [marker for marker in markers if marker in text]}
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
    parser.add_argument("--main-class", default="FrameworkProbe", help="Probe class to run (e.g. ActivityProbe)")
    parser.add_argument("--app", type=Path, help="Installed /data/app/<package>-<id> directory to copy into the root")
    parser.add_argument("--installed-app", type=Path,
                        help="Verified android_package.py package state for ActivityProbe app mode")
    parser.add_argument("--probe-arg", action="append", default=[], help="Argument passed to the probe's main (repeatable)")
    parser.add_argument("--play", action="store_true",
                        help="Keep the installed app running in a host window (implies --managed-graphics); "
                             "--timeout 0 runs until the window closes")
    parser.add_argument("--managed-graphics", type=Path, nargs="?", const=ROOT / "build/android-adapters/managed",
                        help="Install managed libandroid/EGL/GLES adapters (tools/build-android-adapters.py)")
    args = parser.parse_args()
    if args.installed_app:
        if args.app or args.probe_arg or args.main_class != "ActivityProbe":
            parser.error("--installed-app requires --main-class ActivityProbe and cannot combine with --app/--probe-arg")
        from android_package import PACKAGE, app_lock, verify_installed
        state = args.installed_app.resolve()
        try:
            with app_lock(state):
                package = json.loads((state / "package.json").read_text())
                if not PACKAGE.fullmatch(package["package"]):
                    raise ValueError("Invalid installed package name")
                code, root = verify_installed(package["package"], state, package)
                if package["launcher"]["kind"] != "managed_activity":
                    raise ValueError("ActivityProbe app mode requires a managed Activity")
                args.app = root / code.lstrip("/")
                # The installer retains all splits as split-<index>.apk.
                # LoadedApk expects split names sorted, paired with their paths.
                splits = sorted((p["split"], f"split-{i}.apk")
                                for i, p in enumerate(package["packages"])
                                if p["split"] and (not p["abis"] or "arm64-v8a" in p["abis"]))
                args.probe_arg = (["--play"] if args.play else []) + ["--app", package["package"], code,
                                  package["launcher"]["class"],
                                  *(name + "=" + path for name, path in splits)]
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))
    if args.app and not (args.app / "base.apk").is_file():
        parser.error("--app must be an installed code directory containing base.apk")
    if not args.main_class.isidentifier():
        parser.error("--main-class must be a Java class name in the default package")
    if args.play:
        if not args.installed_app:
            parser.error("--play requires --installed-app")
        adapters = ROOT / "build/android-adapters"
        args.managed_graphics = args.managed_graphics or adapters / "managed"
        args.library.append(adapters / "libbifrost-activity.so")
    if not (1 <= args.timeout <= 300 or (args.play and args.timeout == 0)):
        parser.error("timeout must be between 1 and 300 seconds (0 = unlimited with --play)")
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
                      args.art_option, args.prop, args.services, args.library, args.main_class,
                      args.app.resolve() if args.app else None, args.probe_arg,
                      args.managed_graphics.resolve() if args.managed_graphics else None) else 1)
