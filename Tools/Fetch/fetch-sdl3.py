#!/usr/bin/env python3
"""Build the pinned SDL3 release into Tools/sdl3 when the system has no SDL 3.2 or newer.

Android apps draw into an SDL3 window: the emulator compiles against SDL3's headers and loads
libSDL3.so.0 at run time. Distributions such as Ubuntu 24.04 and Debian 12 and 13 package no SDL3,
so this downloads SDL3-3.4.18.tar.gz (the GitHub release asset, pinned SHA-256), builds the shared
library with CMake and installs it into Tools/sdl3 (headers, libSDL3.so.0, sdl3.pc). The Makefile
compiles against it when pkg-config finds no system SDL3 and makes the emulator load it from there,
or from <prefix>/lib/bifrost once installed. The build needs CMake and SDL's X11, Wayland, EGL and
input development packages (Scripts/Setup/install-deps.sh installs them on apt systems).

Usage: fetch-sdl3.py [--force]   (--force builds even when the system has SDL3, for tests)
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/Android"))
import download_progress  # noqa: E402

VERSION = "3.4.18"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL3-{VERSION}.tar.gz"
SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
LIMIT = 64 * 1024 * 1024
DESTINATION = ROOT / "Tools/sdl3"


def system_sdl3():
    try:
        return subprocess.run(["pkg-config", "--exists", "sdl3 >= 3.2.0"], check=False).returncode == 0
    except FileNotFoundError:
        return False


def extract(archive, target):
    with tarfile.open(archive) as source:
        if hasattr(tarfile, "data_filter"):
            source.extractall(target, filter="data")
            return
        for member in source.getmembers():  # Python before 3.12: refuse anything leaving the target
            path = (target / member.name).resolve()
            if not str(path).startswith(str(target.resolve()) + os.sep) or member.issym() or member.islnk() \
                    or member.isdev():
                raise SystemExit(f"Refusing archive member {member.name}")
        source.extractall(target)


def video_drivers(build):
    """The window systems this SDL3 build has: SDL's CMake drops X11 and Wayland without their development packages."""
    drivers = set()
    for header in build.rglob("SDL_build_config.h"):
        text = header.read_text(errors="replace")
        drivers |= {name for name in ("X11", "WAYLAND") if f"#define SDL_VIDEO_DRIVER_{name} 1" in text}
    return drivers


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="build even when the system has SDL 3.2 or newer")
    args = parser.parse_args()
    if (DESTINATION / "lib/pkgconfig/sdl3.pc").is_file():
        print(f"SDL3 {VERSION} is already built in Tools/sdl3")
        return
    if system_sdl3() and not args.force:
        print("The system has SDL 3.2 or newer; nothing to build")
        return
    if DESTINATION.exists():
        raise SystemExit("An incomplete Tools/sdl3 exists; remove it and run this again")
    if not shutil.which("cmake"):
        raise SystemExit("Building SDL3 needs CMake (and SDL's development packages; see Scripts/Setup/install-deps.sh)")
    with tempfile.TemporaryDirectory(prefix=".sdl3-build-", dir=ROOT / "Tools") as directory:
        work = Path(directory)
        # Kept in build/ when interrupted, so the next run resumes it.
        try:
            archive = download_progress.fetch(URL, ROOT / f"build/sdl3-source/SDL3-{VERSION}.tar.gz", f"SDL3 {VERSION}",
                                              LIMIT, check=download_progress.sha256_check(SHA256, f"SDL3-{VERSION}.tar.gz"))
        except ValueError as error:
            raise SystemExit(str(error))
        extract(archive, work)
        source, build = work / f"SDL3-{VERSION}", work / "build"
        generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        print(f"Building SDL3 {VERSION} into Tools/sdl3 ...")
        subprocess.run(["cmake", "-S", str(source), "-B", str(build), *generator, "-DCMAKE_BUILD_TYPE=Release",
                        f"-DCMAKE_INSTALL_PREFIX={DESTINATION}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_SHARED=ON",
                        "-DSDL_STATIC=OFF", "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF"], check=True)
        if not video_drivers(build):
            raise SystemExit("This SDL3 would have no X11 or Wayland window support: its development packages are "
                             "missing (libx11-dev libxext-dev libxkbcommon-dev libwayland-dev wayland-protocols "
                             "libwayland-bin on Debian; Scripts/Setup/install-deps.sh installs them)")
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(os.cpu_count() or 2)], check=True)
        try:
            subprocess.run(["cmake", "--install", str(build)], check=True)
        except BaseException:
            shutil.rmtree(DESTINATION, ignore_errors=True)
            raise
    archive.unlink(missing_ok=True)
    print(f"SDL3 {VERSION} is built in Tools/sdl3; rebuild bifrost-emu to use it (make)")


if __name__ == "__main__":
    main()
