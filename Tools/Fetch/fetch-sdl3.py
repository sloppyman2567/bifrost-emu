#!/usr/bin/env python3
"""Build the pinned SDL3 release into Tools/sdl3 when the system has no SDL 3.2 or newer.

Android apps draw into an SDL3 window: the emulator compiles against SDL3's headers and loads
libSDL3.so.0 at run time. Distributions such as Ubuntu 24.04 and Debian 12 and 13 package no SDL3,
so this downloads SDL3-3.4.18.tar.gz (the GitHub release asset, pinned SHA-256), builds the shared
library with CMake and installs it into Tools/sdl3 (headers, libSDL3.so.0, sdl3.pc). The Makefile
compiles against it when pkg-config finds no system SDL3 and makes the emulator load it from there,
or from <prefix>/lib/bifrost once installed. The build needs CMake and SDL's X11, Wayland, EGL and
input development packages (Scripts/Setup/install-deps.sh installs them on apt systems).

A copy built from another release (the pin moved since) is built again, over the old one, which comes back if the
build fails. A system SDL3 makes the built copy unused, so it is then left alone.

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


def built_version():
    """The release recorded in the built tree's sdl3.pc ("" when it names none), or None when nothing is built there."""
    pc = DESTINATION / "lib/pkgconfig/sdl3.pc"
    if not pc.is_file():
        return None
    for line in pc.read_text(errors="replace").splitlines():
        if line.startswith("Version:"):
            return line.split(":", 1)[1].strip()
    return ""


def decide(built, destination_exists, system, force):
    """What to do: "current" (the built copy is the pinned release), "system" (a system SDL3 makes building needless),
    "replace" (a built copy of another release), "incomplete" (a directory with no build in it) or "build"."""
    if built == VERSION:
        return "current"
    if system and not force:
        return "system"
    if built is not None:
        return "replace"
    return "incomplete" if destination_exists else "build"


def replace_destination(install, replacing):
    """Run install(), which writes DESTINATION. With `replacing`, the old tree is moved aside first and comes back if
    install() fails; it is deleted once install() has succeeded."""
    old = DESTINATION.with_name(DESTINATION.name + ".old")
    if replacing:
        shutil.rmtree(old, ignore_errors=True)
        DESTINATION.rename(old)
    try:
        install()
    except BaseException:
        shutil.rmtree(DESTINATION, ignore_errors=True)
        if replacing:
            old.rename(DESTINATION)
        raise
    if replacing:
        shutil.rmtree(old, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="build even when the system has SDL 3.2 or newer")
    args = parser.parse_args()
    built = built_version()
    action = decide(built, DESTINATION.exists(), system_sdl3() if built != VERSION else False, args.force)
    if action == "current":
        print(f"SDL3 {VERSION} is already built in Tools/sdl3")
        return
    if action == "system":
        print("The system has SDL 3.2 or newer; nothing to build")
        return
    if action == "incomplete":
        raise SystemExit("An incomplete Tools/sdl3 exists; remove it and run this again")
    if action == "replace":
        print(f"Tools/sdl3 holds SDL3 {built or 'of an unknown release'}, not the pinned {VERSION}: building it again")
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
        replace_destination(lambda: subprocess.run(["cmake", "--install", str(build)], check=True),
                            replacing=action == "replace")
    archive.unlink(missing_ok=True)
    print(f"SDL3 {VERSION} is built in Tools/sdl3; rebuild bifrost-emu to use it (make)")


if __name__ == "__main__":
    main()
