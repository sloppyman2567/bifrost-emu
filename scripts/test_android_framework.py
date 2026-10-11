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
from contextlib import ExitStack, closing
from android_processes import ProcessHost

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from android_framework import install


from android_managed import (LogdReceiver, install_managed_graphics, installed_arguments,
                             host_network_environment, install_settings_key, add_trace_arguments,
                             trace_options, collect_trace)


def run(emu, runtime, art_root, probe, mode, output, timeout, art_options=(), properties=(), services=False,
        libraries=(), main_class="FrameworkProbe", app=None, probe_args=(), managed_graphics=None,
        trace_apk=None, trace_after=0, trace_duration=15):
    metadata = install(runtime, art_root)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bifrost-framework-run-") as temporary:
        root = Path(temporary) / "rootfs"
        shutil.copytree(runtime, root, symlinks=True)
        install_settings_key(root, os.urandom(32))
        (root / "data/local/tmp").mkdir(parents=True)
        (root / "data/dalvik-cache/arm64").mkdir(parents=True)
        (root / "storage/emulated/0").mkdir(parents=True, exist_ok=True)
        shutil.copy2(probe, root / "data/local/tmp/probe.dex")
        (root / "data/local/tmp/probe.dex").chmod(0o444)
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
        trace_args, trace_directory = trace_options(root, trace_apk, trace_after, trace_duration)
        program = ["-Xuse-stderr-logger", *art_options, *trace_args,
                   "-Djava.class.path=/data/local/tmp/probe.dex", "/system/bin", main_class, *probe_args]
        environment = dict(os.environ)
        environment.pop("BIFROST_ROOT", None)
        if trace_apk:
            for flag in ("BIFROST_BINDER_TRACE", "BIFROST_FAULT_TRACE", "BIFROST_FRAME_TRACE"):
                environment[flag] = "1"
        guest_env = dict(metadata["environment"], **host_network_environment())
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
        with log_path.open("wb") as log, (output / (mode + ".logd")).open("wb") as logd_sink, ExitStack() as hosts:
            if services and mode != "qemu":
                process_host = hosts.enter_context(closing(ProcessHost(root, emu,
                    ["--no-jit"] if mode == "interp" else [], environment,
                    "/data/local/tmp/probe.dex", output / "processes", trace_args)))
                environment = process_host.environment
                arguments[1:1] = ["--android-binder-broker", process_host.socket]
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
        diagnostic_text = text + (output / (mode + ".logd")).read_bytes().decode(errors="replace")
        app_mode = "--app" in probe_args
        play = "--play" in probe_args
        markers = ("APP_CLASSLOADER_OK", "APP_STORAGE_READY", "APP_PREFERENCES_READY",
                   "APP_APPLICATION_STARTED", "APP_ACTIVITY_CREATED", "FRAMEWORK_OK") \
            if app_mode else ("FRAMEWORK_OK",)
        if services and main_class == "ActivityProbe$InputMethodProbe":
            markers += ("INPUT_METHOD_PROTOCOL_OK",)
        if main_class == "ActivityProbe$NativeLooperProbe":
            markers += ("ANDROID_JAVA_NATIVE_LOOPER_OK",)
        if main_class == "ActivityProbe$NativeChoreographerProbe":
            markers += ("ANDROID_NATIVE_CHOREOGRAPHER_OK",)
        if services and main_class == "ActivityProbe":
            markers += ("ACTIVITY_SERVICE_REGISTRATION_OK", "ACTIVITY_SENSOR_SERVICE_OK",
                        "ACTIVITY_TELEPHONY_SERVICE_OK", "ACTIVITY_SHARED_MEMORY_OK", "ACTIVITY_AUDIO_SERVICE_OK",
                        "ACTIVITY_EXIT_HISTORY_OK", "ACTIVITY_START_HISTORY_OK")
            if not play:
                markers += ("ACTIVITY_FRAMEWORK_VSYNC_OK",)
            if "--webview" in probe_args or "--webview-renderer" in probe_args:
                markers += ("ACTIVITY_WEBVIEW_PACKAGE_OK", "ACTIVITY_APPLICATION_RESTRICTIONS_OK",
                            "ACTIVITY_WINDOW_CONTEXT_OK", "ACTIVITY_INPUT_METHOD_OK",
                            "ACTIVITY_WEBVIEW_CONSTRUCTION_OK", "ACTIVITY_WEBVIEW_OK")
            if "--webview-renderer" in probe_args:
                markers += ("ASGARD_PROCESS_READY", "ACTIVITY_WEBVIEW_RENDERER_DOM_OK",
                            "ACTIVITY_WEBVIEW_RENDERER_PIXELS_OK")
        if play:
            markers += ("APP_WINDOW_OPEN", "APP_AUDIO_ROUTING_READY", "APP_RESUMED", "APP_SURFACE_READY")
        # A play session ends when its window closes (exit 0) or its time runs out.
        # A worker can die while the UI thread keeps pumping until the timeout.
        # Also catch Unity's fatal signal forwarding when an outer debugger
        # exits successfully after the guest crashes.
        guest_failure = bool(re.search(r"\[bifrost-emu\] thread \d+: exception:|"
                                       r"E CRASH\s*: Forwarding signal \d+|"
                                       r"E AndroidRuntime: .*FATAL EXCEPTION|"
                                       r"E cr_JniAndroid\s*: (?:Handling|Crashing due to) uncaught Java exception|"
                                       r"(?:F libc\s*:|F/libc:) Fatal signal \d+|"
                                       r"F app_process64: .*?(?:Runtime aborting|Failed to abort)", diagnostic_text))
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
        saved_trace = collect_trace(trace_directory, output / "traces")
        if saved_trace is not None: result["apk_trace"] = str(saved_trace)
        for tombstone in root.glob("tombstone_*"):
            if tombstone.is_file():
                shutil.copyfile(tombstone, output / (mode + "-" + tombstone.name))
        (output / (mode + ".json")).write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        return passed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    add_trace_arguments(parser)
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
                args.probe_arg = (["--play"] if args.play else []) + installed_arguments(package, code)
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))
    if args.app and not (args.app / "base.apk").is_file():
        parser.error("--app must be an installed code directory containing base.apk")
    if not all(part.isidentifier() for part in args.main_class.split("$")):
        parser.error("--main-class must be a Java class name (optionally nested) in the default package")
    if args.play:
        if not args.installed_app:
            parser.error("--play requires --installed-app")
        adapters = ROOT / "build/android-adapters"
        args.managed_graphics = args.managed_graphics or adapters / "managed"
        args.library.append(adapters / "libbifrost-activity.so")
    elif args.services:
        # ManagedLauncher services answer host-backed queries through this bridge.
        args.library.append(ROOT / "build/android-adapters/libbifrost-activity.so")
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
                      args.managed_graphics.resolve() if args.managed_graphics else None,
                      args.trace_apk, args.trace_after, args.trace_duration) else 1)
