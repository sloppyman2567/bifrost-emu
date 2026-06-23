#!/bin/bash
# fetch-glibc-toolchain.sh — download a prebuilt glibc aarch64 cross-toolchain.
#
# This is OPTIONAL — bifrost-emu primarily targets static musl binaries.
# However, some programs are built against glibc (e.g., distro packages).
# This script fetches a prebuilt aarch64-linux-gnu toolchain from musl.cc's
# mirror (which also hosts glibc toolchains) or from the Arm toolchain site.
#
# Usage:
#   ./tools/fetch-glibc-toolchain.sh
#
# Then cross-compile with:
#   tools/aarch64-linux-gnu-cross/bin/aarch64-linux-gnu-gcc -static -O2 -o foo.elf foo.c
set -euo pipefail
cd "$(dirname "$0")"

URL="https://musl.cc/aarch64-linux-gnu-cross.tgz"
TARBALL="aarch64-linux-gnu-cross.tgz"
DIR="aarch64-linux-gnu-cross"

if [ -x "$DIR/bin/aarch64-linux-gnu-gcc" ]; then
    echo "glibc toolchain already present at tools/$DIR/"
    exit 0
fi

echo "Downloading $URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -fL -o "$TARBALL" "$URL" || {
        echo "musl.cc glibc toolchain not available, trying Arm developer site..."
        URL="https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
        curl -fL -o "$TARBALL.xz" "$URL"
        tar -xJf "$TARBALL.xz"
        rm "$TARBALL.xz"
        mv arm-gnu-toolchain-* "$DIR"
        echo "Done. Toolchain at tools/$DIR/bin/aarch64-none-linux-gnu-gcc"
        exit 0
    }
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TARBALL" "$URL" || {
        echo "musl.cc glibc toolchain not available. Please install manually."
        exit 1
    }
else
    echo "Error: need curl or wget to download." >&2
    exit 1
fi

echo "Extracting..."
tar -xzf "$TARBALL"
rm "$TARBALL"

echo "Done. Toolchain at tools/$DIR/bin/aarch64-linux-gnu-gcc"
"$DIR/bin/aarch64-linux-gnu-gcc" --version | head -1
