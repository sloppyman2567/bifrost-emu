#!/bin/bash
# fetch-sdl2-headers.sh — download SDL2 dev headers via apt-get download.
#
# Downloads the libsdl2-dev .deb package without installing it system-wide,
# extracts the headers and sdl2-config into tools/sdl2-sdk/, and prints
# instructions for building bifrost-emu with SDL2 support.
#
# Usage:
#   ./tools/fetch-sdl2-headers.sh
#
# Then build with:
#   make USE_SDL2=1 SDL2_CFLAGS="-Itools/sdl2-sdk/include" \
#        SDL2_LIBS="-Ltools/sdl2-sdk/lib -lSDL2"
#
# Or, if sdl2-config is available system-wide, just:
#   make USE_SDL2=1
set -euo pipefail
cd "$(dirname "$0")"

SDK_DIR="sdl2-sdk"
DEB_DIR="sdl2-debs"
DEB_PATTERN="libsdl2-dev_*.deb"

# ── Check if already set up ──────────────────────────────────────────
if [ -d "$SDK_DIR/include/SDL2" ]; then
    echo "SDL2 SDK already present at tools/$SDK_DIR/"
    echo "  Headers:  tools/$SDK_DIR/include/SDL2/"
    echo "  To build: make USE_SDL2=1 SDL2_CFLAGS=\"-I$SDK_DIR/include\" SDL2_LIBS=\"-L$SDK_DIR/lib -lSDL2\""
    exit 0
fi

echo "=== Downloading SDL2 dev package via apt-get download ==="

# Create temp directory for .deb files
mkdir -p "$DEB_DIR"
cd "$DEB_DIR"

# Download the .deb without installing it
# apt-get download fetches the package to the current directory
apt-get download libsdl2-dev 2>&1 || {
    echo "Error: apt-get download failed. You may need to run 'apt-get update' first."
    echo "Alternatively, install SDL2 system-wide: sudo apt-get install libsdl2-dev"
    exit 1
}

# Also download the runtime library (for the .so file)
apt-get download libsdl2-2.0-0 2>&1 || true

cd ..

# ── Extract the .deb ────────────────────────────────────────────────
echo ""
echo "=== Extracting SDL2 SDK ==="

DEB_FILE=$(ls "$DEB_DIR"/$DEB_PATTERN 2>/dev/null | head -1)
if [ -z "$DEB_FILE" ]; then
    echo "Error: libsdl2-dev .deb not found in $DEB_DIR/"
    exit 1
fi

# .deb files are ar archives containing data.tar.xz or data.tar.gz
# Extract in a temp directory
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Extract the .deb
ar x "$DEB_FILE" --output "$TMP_DIR"

# Find and extract the data archive
DATA_FILE=""
for ext in xz gz bz2 lzma; do
    if [ -f "$TMP_DIR/data.tar.$ext" ]; then
        DATA_FILE="$TMP_DIR/data.tar.$ext"
        break
    fi
done

if [ -z "$DATA_FILE" ]; then
    echo "Error: could not find data.tar.* in $DEB_FILE"
    exit 1
fi

# Extract data.tar to SDK directory
mkdir -p "$SDK_DIR"
case "$DATA_FILE" in
    *.xz)  tar xJf "$DATA_FILE" -C "$SDK_DIR" ;;
    *.gz)  tar xzf "$DATA_FILE" -C "$SDK_DIR" ;;
    *.bz2) tar xjf "$DATA_FILE" -C "$SDK_DIR" ;;
esac

# The .deb extracts to usr/include/SDL2/, usr/lib/x86_64-linux-gnu/, etc.
# Move them to a flat structure under sdk/
if [ -d "$SDK_DIR/usr" ]; then
    # Move include
    if [ -d "$SDK_DIR/usr/include" ]; then
        mv "$SDK_DIR/usr/include" "$SDK_DIR/include_tmp"
        rm -rf "$SDK_DIR/usr"
        mv "$SDK_DIR/include_tmp" "$SDK_DIR/include"
    fi
    # Move lib (if present)
    if [ -d "$SDK_DIR/usr/lib" ]; then
        mv "$SDK_DIR/usr/lib" "$SDK_DIR/lib_tmp"
        rm -rf "$SDK_DIR/usr"
        mv "$SDK_DIR/lib_tmp" "$SDK_DIR/lib"
    fi
fi

# ── Copy _real_SDL_config.h from the multiarch include path ──────────
# Debian's libsdl2-dev ships SDL_config.h as a thin wrapper that
# #includes <SDL2/_real_SDL_config.h> from the multiarch include dir
# (e.g. usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h). The
# wrapper lives in usr/include/SDL2/ (which we moved to sdk/include/SDL2/),
# but the real config header is in the multiarch path. The data.tar.xz
# of the .deb contains both — find the multiarch copy and move it
# alongside the wrapper so `#include <SDL2/_real_SDL_config.h>` works
# with just `-Itools/sdl2-sdk/include`.
REAL_CONFIG=$(find "$SDK_DIR" -name "_real_SDL_config.h" -type f 2>/dev/null | head -1)
if [ -n "$REAL_CONFIG" ] && [ ! -f "$SDK_DIR/include/SDL2/_real_SDL_config.h" ]; then
    cp "$REAL_CONFIG" "$SDK_DIR/include/SDL2/_real_SDL_config.h"
    echo "  Copied _real_SDL_config.h (Debian multiarch config header)"
fi

# Also extract the runtime .so from libsdl2-2.0-0
RT_DEB=$(ls "$DEB_DIR"/libsdl2-2.0-0_*.deb 2>/dev/null | head -1)
if [ -n "$RT_DEB" ]; then
    RT_TMP=$(mktemp -d)
    ar x "$RT_DEB" --output "$RT_TMP"
    RT_DATA=""
    for ext in xz gz bz2 lzma; do
        if [ -f "$RT_TMP/data.tar.$ext" ]; then
            RT_DATA="$RT_TMP/data.tar.$ext"
            break
        fi
    done
    if [ -n "$RT_DATA" ]; then
        mkdir -p "$RT_TMP/extract"
        case "$RT_DATA" in
            *.xz)  tar xJf "$RT_DATA" -C "$RT_TMP/extract" ;;
            *.gz)  tar xzf "$RT_DATA" -C "$RT_TMP/extract" ;;
            *.bz2) tar xjf "$RT_DATA" -C "$RT_TMP/extract" ;;
        esac
        # Find the .so file and copy it
        SO_FILE=$(find "$RT_TMP/extract" -name "libSDL2-2.0.so.*" -type f | head -1)
        if [ -n "$SO_FILE" ]; then
            mkdir -p "$SDK_DIR/lib"
            cp "$SO_FILE" "$SDK_DIR/lib/"
            # Create a symlink libSDL2.so -> libSDL2-2.0.so.0
            SO_BASENAME=$(basename "$SO_FILE")
            (cd "$SDK_DIR/lib" && ln -sf "$SO_BASENAME" libSDL2.so)
            echo "  Copied $SO_BASENAME to $SDK_DIR/lib/"
        fi
    fi
    rm -rf "$RT_TMP"
fi

# Clean up .deb files (keep them for reference)
echo ""
echo "=== SDL2 SDK ready ==="
echo "  Headers:  tools/$SDK_DIR/include/SDL2/"
if [ -d "$SDK_DIR/lib" ]; then
    echo "  Library:  tools/$SDK_DIR/lib/"
fi
echo ""
echo "To build bifrost-emu with SDL2:"
echo "  make USE_SDL2=1 \\"
echo "       SDL2_CFLAGS=\"-Itools/$SDK_DIR/include\" \\"
echo "       SDL2_LIBS=\"-Ltools/$SDK_DIR/lib -lSDL2\""
echo ""
echo "Or, if SDL2 is installed system-wide, just:"
echo "  make USE_SDL2=1"
