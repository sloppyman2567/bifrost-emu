#!/usr/bin/env python3
"""Bounded real-ART launch; success requires a JNI marker and clean shutdown."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from android_art import install


def inspect_log(path):
    markers = ("android_jni_bootstrap: ALL PASS", "Fatal signal", "[crash]",
               "JNI DETECTED ERROR", "ART_LOAD_OK", "ART_VM_OK")
    seen, tail, overlap = set(), b"", b""
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(64 * 1024), b""):
            data = overlap + chunk
            seen.update(marker for marker in markers if marker.encode() in data)
            overlap = data[-64:]
            tail = (tail + chunk)[-8000:]
    return seen, tail.decode(errors="replace")


def run(emu, runtime, fixture, mode, output, timeout):
    install(runtime)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bifrost-art-run-") as temporary:
        root = Path(temporary) / "rootfs"
        shutil.copytree(runtime, root, symlinks=True)
        files = root / "data/local/tmp"
        files.mkdir(parents=True)
        (root / "data/dalvik-cache/arm64").mkdir(parents=True)
        for name in ("bifrost-art", "libbifrost-jni.so", "classes.dex"):
            shutil.copy2(fixture / name, files / name)
        arguments = [str(emu)] + (["--no-jit"] if mode == "interp" else [])
        arguments += ["--android-runtime", "--rootfs", str(root), "/data/local/tmp/bifrost-art", "Bootstrap"]
        environment = dict(os.environ)
        environment.pop("BIFROST_ROOT", None)
        environment["BIFROST_TIER2"] = "0"
        log_path = output / (mode + ".log")
        started = time.monotonic()
        timed_out = False
        with log_path.open("wb") as log:
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
            code = child.returncode
        seen, tail = inspect_log(log_path)
        passed = code == 0 and not timed_out and "android_jni_bootstrap: ALL PASS" in seen
        passed = passed and not seen.intersection(("Fatal signal", "[crash]", "JNI DETECTED ERROR"))
        passed = bool(passed)
        result = {"mode": mode, "passed": passed, "returncode": code,
                  "timed_out": timed_out, "elapsed_seconds": round(time.monotonic() - started, 3),
                  "art_loaded": "ART_LOAD_OK" in seen, "vm_created": "ART_VM_OK" in seen}
        (output / (mode + ".json")).write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        if not passed:
            print(tail, file=sys.stderr)
        return passed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    parser.add_argument("--runtime-root", type=Path, default=ROOT / "build/android-art/rootfs")
    parser.add_argument("--fixture", type=Path, default=ROOT / "build/android-jni")
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-jni-results")
    parser.add_argument("--mode", choices=("jit", "interp"), default="jit")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 300:
        parser.error("timeout must be between 1 and 300 seconds")
    sys.exit(0 if run(args.emu.resolve(), args.runtime_root.resolve(), args.fixture.resolve(),
                     args.mode, args.output.resolve(), args.timeout) else 1)
