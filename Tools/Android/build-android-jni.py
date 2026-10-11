#!/usr/bin/env python3
"""Build a real DEX/JNI fixture against the separate same-image ART runtime."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

from android_art import install, ROOT
from android_guest_dependencies import prepare_headers


def build(runtime, output, javac):
    install(runtime)
    spec = importlib.util.spec_from_file_location("package_tools", ROOT / "Tools/Android/fetch-android-package-tools.py")
    package_tools = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package_tools)
    package_tools.install(ROOT / "Tools/android-tools")
    output.mkdir(parents=True, exist_ok=True)
    bionic = runtime / "apex/com.android.runtime/lib64/bionic"
    with tempfile.TemporaryDirectory(prefix=".android-jni-", dir=output.parent) as temporary:
        staging = Path(temporary)
        flags = ["clang", "--target=aarch64-linux-android35", "-fuse-ld=lld", "-nostdlib",
                 "-ffreestanding", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "Guest/Android"),
                 "-Wl,-z,max-page-size=16384", "-Wl,-z,defs"]
        flags += prepare_headers(staging / "headers")
        subprocess.run(flags + ["-fPIC", "-shared", str(ROOT / "Tests/Unit/android_jni/bridge.c"), str(bionic / "libc.so"),
                                "-Wl,--allow-shlib-undefined",
                                "-o", str(staging / "libbifrost-jni.so")], check=True)
        subprocess.run(flags + ["-fPIE", "-pie", "-Wl,--dynamic-linker=/system/bin/linker64",
                                "-Wl,--allow-shlib-undefined", str(ROOT / "Tests/Unit/android_bionic_start.S"),
                                str(ROOT / "Guest/Android/art_launcher.c"), str(bionic / "libc.so"),
                                str(bionic / "libdl.so"), "-o", str(staging / "bifrost-art")], check=True)
        subprocess.run([javac, "--release", "8", "-encoding", "UTF-8", "-d", str(staging),
                        str(ROOT / "Tests/Unit/android_jni/Bootstrap.java")], check=True)
        subprocess.run(["java", "-cp", str(ROOT / "Tools/android-tools/d8.jar"), "com.android.tools.r8.D8",
                        "--min-api", "26", "--output", str(staging), str(staging / "Bootstrap.class")], check=True)
        for name in ("bifrost-art", "libbifrost-jni.so", "classes.dex"):
            (staging / name).replace(output / name)
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", type=Path, default=ROOT / "build/android-art/rootfs")
    parser.add_argument("--output", type=Path, default=ROOT / "build/android-jni")
    parser.add_argument("--javac", default=shutil.which("javac"))
    args = parser.parse_args()
    if not args.javac:
        parser.error("A JDK javac is required; pass --javac /path/to/javac")
    build(args.runtime_root.resolve(), args.output.resolve(), args.javac)
