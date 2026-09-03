#!/bin/bash
# check-rootfs-libs.sh — verify every ELF in the rootfs has its DT_NEEDED
# libraries resolvable under the same search order as the emulator's
# find_library() (src/frontend/dynamic_linker.cpp).
#
# Catches the neverball-class failure (pc=0 from NULL-resolved symbols)
# before you ever boot the game: a missing lib is one line of output
# here instead of a whole debugging hunt.
#
# Usage: ./scripts/check-rootfs-libs.sh [rootfs-dir] [--quiet]
# Exit 0 = all resolved, 1 = something missing.
set -u

ROOTFS="${1:-./rootfs}"
QUIET=0
[ "${2:-}" = "--quiet" ] && QUIET=1

if [ ! -d "$ROOTFS" ]; then
    echo "check-rootfs-libs: no such dir '$ROOTFS'" >&2
    exit 1
fi

# Mirror the BIFROST_ROOT search dirs in find_library().
search_dirs() {
    local soname="$1"
    local d
    for d in "$ROOTFS/lib" "$ROOTFS/lib64" \
             "$ROOTFS/usr/lib" "$ROOTFS/usr/lib64" \
             "$ROOTFS/lib/aarch64-linux-gnu" \
             "$ROOTFS/usr/lib/aarch64-linux-gnu"; do
        if [ -f "$d/$soname" ]; then
            echo "$d/$soname"
            return 0
        fi
    done
    return 1
}

# Libraries the emulator serves via thunks (host GL/SDL/audio/display),
# never from rootfs files. Missing on disk is by design, not a bug.
is_thunk_lib() {
    case "$1" in
        libGL.so*|libEGL.so*|libSDL2-2.0.so*|libGLESv2.so*|libglfw.so*|\
        libasound*|libpulse*|libopenal*|libaaudio*|libOpenSLES*|\
        libvulkan*|libwayland-*|libX11*|libgbm*) return 0;;
    esac
    return 1
}

total_missing=0
total_checked=0
while IFS= read -r f; do
    # ELF magic check (skip scripts, data, dangling symlinks).
    if ! head -c 4 "$f" 2>/dev/null | grep -q $'^\x7fELF'; then
        continue
    fi
    # gconv modules reference sibling plugins loaded relatively at
    # runtime by iconv, not via the loader search — always false alarms.
    case "$f" in */gconv/*) continue;; esac
    needed=$(readelf -d "$f" 2>/dev/null | grep -oP '\[lib[^]]+\.so[^]]*\]' | tr -d '[]' | sort -u)
    [ -z "$needed" ] && continue
    total_checked=$((total_checked + 1))
    missing=""
    for lib in $needed; do
        is_thunk_lib "$lib" && continue
        if ! loc=$(search_dirs "$lib"); then
            missing="$missing $lib"
        fi
    done
    if [ -n "$missing" ]; then
        echo "MISSING for $f:$missing"
        total_missing=$((total_missing + 1))
    fi
done < <(find "$ROOTFS" -type f -not -path "*/include/*" -not -name "*.sol" -not -name "*.png" -not -name "*.jpg" -not -name "*.obj" -not -name "*.mtl" -not -name "*.map" -not -name "*.txt" -not -name "*.mo" 2>/dev/null)

if [ "$QUIET" -eq 0 ]; then
    echo "checked $total_checked ELFs, $total_missing with missing libs"
fi
[ "$total_missing" -eq 0 ]
