#!/usr/bin/env bash
# install-deps.sh — install the host packages that building bifrost-emu and Asgard need.
#
# Usage:
#   ./Scripts/Setup/install-deps.sh                 # what the emulator and Asgard need (or: make deps)
#   ./Scripts/Setup/install-deps.sh --no-asgard     # only what the emulator needs
#   ./Scripts/Setup/install-deps.sh --dry-run       # print what it would do, install nothing
#   ./Scripts/Setup/install-deps.sh --manager NAME  # use apt, dnf, zypper or pacman, not the detected one
#
# Only packages that are missing are installed, so sudo is needed only then, and the package
# manager still asks before it changes anything. On a distribution using another package
# manager it lists what to look for. Run the emulator where its libraries are: on an immutable
# system such as Bazzite that means inside a container (see the README).
set -euo pipefail

ASGARD=1
DRY_RUN=0
MANAGER=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-asgard) ASGARD=0 ;;
        --dry-run)   DRY_RUN=1 ;;
        --manager)   [ $# -ge 2 ] || { echo "--manager needs a name" >&2; exit 2; }
                     MANAGER="$2"; shift ;;
        --help|-h)   sed -n '2,/^set /p' "$0" | sed '$d; s/^# \?//'; exit 0 ;;
        *)           echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# pacman comes last: on Debian the command of that name can be the game.
if [ -z "$MANAGER" ]; then
    if command -v apt-get >/dev/null 2>&1; then MANAGER=apt
    elif command -v dnf >/dev/null 2>&1; then MANAGER=dnf
    elif command -v zypper >/dev/null 2>&1; then MANAGER=zypper
    elif command -v pacman >/dev/null 2>&1; then MANAGER=pacman
    fi
fi

# OPTIONAL: left out with a warning where the distribution has no such package.
OPTIONAL=()
case "$MANAGER" in
    apt)
        BUILD=(build-essential pkg-config git python3 curl xz-utils libavcodec-dev libavutil-dev libswscale-dev
               libegl-dev libffi-dev libgl-dev libsdl2-dev libwayland-dev libx11-dev)
        OPTIONAL=(libsdl3-dev)
        TOOLS=(clang lld default-jdk-headless e2fsprogs binutils python3-pyqt6)
        ;;
    dnf)
        BUILD=(gcc-c++ make pkgconf-pkg-config git python3 curl xz diffutils SDL3-devel sdl2-compat-devel
               mesa-libGL-devel mesa-libEGL-devel libffi-devel wayland-devel libX11-devel
               libavcodec-free-devel libavutil-free-devel libswscale-free-devel)
        TOOLS=(clang lld java-latest-openjdk-devel e2fsprogs binutils python3-pyqt6)
        ;;
    zypper)
        BUILD=(gcc-c++ make pkgconf-pkg-config git python3 curl xz diffutils SDL3-devel sdl2-compat-devel
               Mesa-libGL-devel Mesa-libEGL-devel libffi-devel wayland-devel libX11-devel
               'pkgconfig(libavcodec)' 'pkgconfig(libavutil)' 'pkgconfig(libswscale)')
        TOOLS=(clang lld java-devel e2fsprogs binutils python3-PyQt6)
        ;;
    pacman)
        BUILD=(base-devel pkgconf git python curl xz diffutils ffmpeg sdl3 sdl2-compat mesa libglvnd libffi wayland libx11)
        TOOLS=(clang lld jdk-openjdk e2fsprogs binutils python-pyqt6)
        ;;
    "")
        echo "error: no supported package manager found (apt, dnf, zypper or pacman)." >&2
        echo "  Install the equivalents of: a C++17 compiler, make, pkg-config, git, Python 3.12+, curl, xz," >&2
        echo "  development files for SDL2, SDL3, OpenGL, EGL, libffi, Wayland, X11 and FFmpeg" >&2
        echo "  (libavcodec, libavutil, libswscale); for Asgard: Clang, LLD, a JDK, e2fsprogs, binutils, PyQt6." >&2
        exit 1
        ;;
    *)
        echo "error: unknown package manager: $MANAGER (apt, dnf, zypper or pacman)" >&2
        exit 2
        ;;
esac

installed() {
    # Any JDK will do, and a system often has one under another package name.
    case "$1" in
        default-jdk-headless|java-latest-openjdk-devel|java-devel|jdk-openjdk)
            if command -v javac >/dev/null 2>&1 && command -v keytool >/dev/null 2>&1; then return 0; fi ;;
    esac
    case "$MANAGER" in
        apt)        [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" = installed ] ;;
        dnf|zypper) rpm -q --whatprovides "$1" >/dev/null 2>&1 ;;
        pacman)     pacman -T "$1" >/dev/null 2>&1 ;;  # unlike -Q, also counts what a package provides
    esac
}

if [ "$MANAGER" = apt ]; then
    REFRESH=(apt-get update)
    INSTALL=(apt-get install)
else
    REFRESH=()
    case "$MANAGER" in
        dnf)    INSTALL=(dnf install) ;;
        zypper) INSTALL=(zypper install) ;;
        pacman) INSTALL=(pacman -S --needed) ;;  # not -Sy: Arch does not support partial upgrades
    esac
fi

WANTED=("${BUILD[@]}")
[ "$ASGARD" = 0 ] || WANTED+=("${TOOLS[@]}")
MISSING=()
for package in "${WANTED[@]}" "${OPTIONAL[@]}"; do
    installed "$package" || MISSING+=("$package")
done
if [ "${#MISSING[@]}" -eq 0 ]; then
    echo "All the packages this build needs are installed ($MANAGER)."
    exit 0
fi

SUDO=()
if [ "$(id -u)" != 0 ]; then
    SUDO=(sudo)
    if [ "$DRY_RUN" = 0 ] && ! command -v sudo >/dev/null 2>&1; then
        echo "error: sudo is not available. As root, run: ${INSTALL[*]} ${MISSING[*]}" >&2
        exit 1
    fi
fi

if [ "$DRY_RUN" = 1 ]; then
    echo "Would run: ${SUDO[*]+${SUDO[*]} }${INSTALL[*]} ${MISSING[*]}"
    exit 0
fi

[ "${#REFRESH[@]}" -eq 0 ] || "${SUDO[@]+"${SUDO[@]}"}" "${REFRESH[@]}"

# Decided after the refresh, which is what makes the package lists current.
if [ "${#OPTIONAL[@]}" -gt 0 ]; then
    AVAILABLE=()
    for package in "${MISSING[@]}"; do
        if [[ " ${OPTIONAL[*]} " == *" $package "* ]] && ! apt-cache show "$package" >/dev/null 2>&1; then
            echo "warning: this distribution has no $package; Android apps will have no host window" \
                 "(Debian/Ubuntu 25.10 or newer has it)." >&2
            continue
        fi
        AVAILABLE+=("$package")
    done
    MISSING=("${AVAILABLE[@]+"${AVAILABLE[@]}"}")
    [ "${#MISSING[@]}" -gt 0 ] || exit 0
fi

echo "Installing with $MANAGER: ${MISSING[*]}"
if ! "${SUDO[@]+"${SUDO[@]}"}" "${INSTALL[@]}" "${MISSING[@]}"; then
    echo "error: $MANAGER could not install the packages above." >&2
    [ "$MANAGER" != pacman ] || echo "  If a download failed with 404, update the system first: sudo pacman -Syu" >&2
    exit 1
fi
