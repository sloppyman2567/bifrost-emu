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
# manager still asks before it changes anything. A package counts as present when its tool or library is
# found anyway: tools off PATH (debugfs in /usr/sbin, a JDK under /usr/lib/jvm, anything in ~/.local/bin;
# see find-host-tools.sh) and libraries pkg-config finds (PKG_CONFIG_PATH, or the SDL3 that
# Tools/Fetch/fetch-sdl3.py builds), so a machine without root can use what it has. Where the
# distribution has no SDL3 package (Debian 12 and 13, Ubuntu 24.04), the packages that building SDL3
# needs are installed and `make bootstrap` builds it. On a distribution using another package
# manager it lists what to look for. Run the emulator where its libraries are: on an immutable
# system such as Bazzite that means inside a container (see the README).
set -euo pipefail

HERE=${BASH_SOURCE[0]%/*}; [ "$HERE" != "${BASH_SOURCE[0]}" ] || HERE=.  # no dirname: it need not be on PATH
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=find-host-tools.sh
. "$ROOT/Scripts/Setup/find-host-tools.sh"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$ROOT/Tools/sdl3/lib/pkgconfig"

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
SDL3_BUILD=()  # what building SDL3 needs where the distribution packages none
case "$MANAGER" in
    apt)
        BUILD=(build-essential pkg-config git python3 curl xz-utils libavcodec-dev libavutil-dev libswscale-dev
               libegl-dev libffi-dev libgl-dev libsdl2-dev libwayland-dev libx11-dev)
        OPTIONAL=(libsdl3-dev)
        SDL3_BUILD=(cmake libxkbcommon-dev wayland-protocols libwayland-bin libxext-dev libxcursor-dev libxi-dev
                    libxrandr-dev libxfixes-dev libdrm-dev libgbm-dev libudev-dev)
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

# What a package provides, as "tool NAME", "pc MODULE" or "python MODULE" words; empty when unknown.
provides() {
    case "$1" in
        build-essential|base-devel|gcc-c++) echo "tool g++ tool make" ;;
        make|cmake|git|curl|clang|binutils) echo "tool ${1/binutils/readelf}" ;;
        pkg-config|pkgconf|pkgconf-pkg-config) echo "tool pkg-config" ;;
        python3|python) echo "tool python3" ;;
        xz-utils|xz) echo "tool xz" ;;
        diffutils) echo "tool diff" ;;
        lld) echo "tool ld.lld" ;;
        e2fsprogs) echo "tool debugfs" ;;
        default-jdk-headless|java-latest-openjdk-devel|java-devel|jdk-openjdk) echo "tool javac tool keytool" ;;
        python3-pyqt6|python-pyqt6|python3-PyQt6) echo "python PyQt6.QtWidgets" ;;
        libsdl2-dev|sdl2-compat|sdl2-compat-devel) echo "pc sdl2" ;;
        libsdl3-dev|SDL3-devel|sdl3) echo "pc sdl3>=3.2.0" ;;
        libavcodec-dev|libavcodec-free-devel|ffmpeg|'pkgconfig(libavcodec)') echo "pc libavcodec pc libavutil pc libswscale" ;;
        libavutil-dev|libavutil-free-devel|'pkgconfig(libavutil)') echo "pc libavutil" ;;
        libswscale-dev|libswscale-free-devel|'pkgconfig(libswscale)') echo "pc libswscale" ;;
        libegl-dev|mesa-libEGL-devel|Mesa-libEGL-devel) echo "pc egl" ;;
        libgl-dev|mesa-libGL-devel|Mesa-libGL-devel|libglvnd) echo "pc gl" ;;
        libffi-dev|libffi-devel|libffi) echo "pc libffi" ;;
        libwayland-dev|wayland-devel|wayland) echo "pc wayland-client" ;;
        libx11-dev|libX11-devel|libx11) echo "pc x11" ;;
        libxkbcommon-dev) echo "pc xkbcommon" ;;
        wayland-protocols) echo "pc wayland-protocols" ;;
        libwayland-bin) echo "tool wayland-scanner" ;;
        libxext-dev) echo "pc xext" ;;
        libxcursor-dev) echo "pc xcursor" ;;
        libxi-dev) echo "pc xi" ;;
        libxrandr-dev) echo "pc xrandr" ;;
        libxfixes-dev) echo "pc xfixes" ;;
        libdrm-dev) echo "pc libdrm" ;;
        libgbm-dev) echo "pc gbm" ;;
        libudev-dev) echo "pc libudev" ;;
    esac
}

# True when everything the package provides is found, wherever it is; FOUND_NOTES records where.
found_anyway() {
    local words kind what path
    read -r -a words <<<"$(provides "$1")"
    [ "${#words[@]}" -gt 0 ] || return 1
    for ((i = 0; i < ${#words[@]}; i += 2)); do
        kind=${words[i]} what=${words[i + 1]}
        case "$kind" in
            tool)   path=$(find_host_tool "$what") || return 1 ;;
            pc)     if [[ "$what" == *">="* ]]; then
                        pkg-config --atleast-version "${what#*>=}" "${what%%>=*}" 2>/dev/null || return 1
                    else
                        pkg-config --exists "$what" 2>/dev/null || return 1
                    fi
                    path="pkg-config $what" ;;
            python) python3 -c "import $what" >/dev/null 2>&1 || return 1; path="python3 $what" ;;
        esac
    done
    FOUND_NOTES+=("  $1: not installed by $MANAGER, but found ($path)")
}

installed() {
    case "$MANAGER" in
        apt)        [ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null)" = installed ] && return 0 ;;
        dnf|zypper) rpm -q --whatprovides "$1" >/dev/null 2>&1 && return 0 ;;
        pacman)     pacman -T "$1" >/dev/null 2>&1 && return 0 ;;  # unlike -Q, also counts what a package provides
    esac
    found_anyway "$1"
}
FOUND_NOTES=()

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
if [ "${#FOUND_NOTES[@]}" -gt 0 ]; then
    echo "Using what is already here instead of installing:"
    printf '%s\n' "${FOUND_NOTES[@]}"
fi
if [ "${#MISSING[@]}" -eq 0 ]; then
    echo "All the packages this build needs are installed ($MANAGER)."
    exit 0
fi

# Packages the distribution may lack (OPTIONAL) are dropped from MISSING, and where that leaves it without
# SDL3 the packages building it needs are added. A real run decides after the refresh, which is what makes
# the package lists current; a dry run and the no-sudo error use the lists as they are.
resolve_optional() {
    [ "${#OPTIONAL[@]}" -gt 0 ] || return 0
    local AVAILABLE=() package build_package
    for package in "${MISSING[@]}"; do
        if [[ " ${OPTIONAL[*]} " == *" $package "* ]] && ! apt-cache show "$package" >/dev/null 2>&1; then
            echo "warning: this distribution has no $package; SDL3 will be built from source by make bootstrap" \
                 "(Debian/Ubuntu 25.10 or newer packages it)." >&2
            if [ "$ASGARD" = 1 ]; then
                for build_package in "${SDL3_BUILD[@]}"; do
                    installed "$build_package" || AVAILABLE+=("$build_package")
                done
            fi
            continue
        fi
        AVAILABLE+=("$package")
    done
    MISSING=("${AVAILABLE[@]+"${AVAILABLE[@]}"}")
}

SUDO=()
if [ "$(id -u)" != 0 ]; then
    SUDO=(sudo)
    if [ "$DRY_RUN" = 0 ] && ! command -v sudo >/dev/null 2>&1; then
        resolve_optional
        echo "error: sudo is not available. As root, run: ${INSTALL[*]} ${MISSING[*]}" >&2
        echo "  Without root, install those yourself under a prefix of your own and point the build at them:" >&2
        echo "  BIFROST_TOOL_DIRS=DIR/bin for tools, PKG_CONFIG_PATH=DIR/lib/pkgconfig for libraries" >&2
        echo "  (LD_LIBRARY_PATH=DIR/lib when running), then run this again; what it finds it does not install." >&2
        exit 1
    fi
fi

if [ "$DRY_RUN" = 1 ]; then
    resolve_optional
    [ "${#MISSING[@]}" -gt 0 ] || { echo "Nothing to install."; exit 0; }
    echo "Would run: ${SUDO[*]+${SUDO[*]} }${INSTALL[*]} ${MISSING[*]}"
    exit 0
fi

[ "${#REFRESH[@]}" -eq 0 ] || "${SUDO[@]+"${SUDO[@]}"}" "${REFRESH[@]}"
resolve_optional
[ "${#MISSING[@]}" -gt 0 ] || exit 0

echo "Installing with $MANAGER: ${MISSING[*]}"
if ! "${SUDO[@]+"${SUDO[@]}"}" "${INSTALL[@]}" "${MISSING[@]}"; then
    echo "error: $MANAGER could not install the packages above." >&2
    [ "$MANAGER" != pacman ] || echo "  If a download failed with 404, update the system first: sudo pacman -Syu" >&2
    exit 1
fi
