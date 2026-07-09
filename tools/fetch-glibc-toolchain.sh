#!/bin/bash
# fetch-glibc-toolchain.sh — download a prebuilt glibc aarch64 cross-toolchain.
#
# This is OPTIONAL — bifrost-emu primarily targets static musl binaries.
# However, some programs are built against glibc (e.g., distro packages).
# This script fetches a prebuilt aarch64-linux-gnu toolchain from musl.cc's
# mirror (which also hosts glibc toolchains) or from the Arm toolchain site.
#
# The download is bounded by a hard 90-second total timeout. See
# fetch-musl-toolchain.sh for the rationale — TL;DR: without a cap, a stalled
# mirror can hang the bootstrap indefinitely and CI will silently run into
# its wall-clock limit instead of failing fast.
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

# Hard total timeout (seconds) for the download phase. The glibc toolchain
# is ~130 MB (larger than musl's 104 MB), but 90 s is still generous on any
# reasonable link and bounds worst-case stalls so the caller can fail fast.
DOWNLOAD_TIMEOUT=90
# curl-style argument string — reused for both URLs below.
CURL_OPTS=(--connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 -fL)

if [ -x "$DIR/bin/aarch64-linux-gnu-gcc" ]; then
    echo "glibc toolchain already present at tools/$DIR/"
    exit 0
fi

echo "Downloading $URL (timeout: ${DOWNLOAD_TIMEOUT}s) ..."
if command -v curl >/dev/null 2>&1; then
    if ! curl "${CURL_OPTS[@]}" -o "$TARBALL" "$URL"; then
        echo "musl.cc glibc toolchain not available, trying Arm developer site..."
        URL="https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
        if ! curl "${CURL_OPTS[@]}" -o "$TARBALL.xz" "$URL"; then
            echo "Error: Arm developer site download also failed (curl exit $?)." >&2
            [ -f "$TARBALL.xz" ] && rm -f "$TARBALL.xz"
            exit 1
        fi
        tar -xJf "$TARBALL.xz"
        rm "$TARBALL.xz"
        mv arm-gnu-toolchain-* "$DIR"
        echo "Done. Toolchain at tools/$DIR/bin/aarch64-none-linux-gnu-gcc"
        exit 0
    fi
elif command -v wget >/dev/null 2>&1; then
    if ! wget --timeout="$DOWNLOAD_TIMEOUT" --tries=1 -O "$TARBALL" "$URL"; then
        echo "musl.cc glibc toolchain not available. Please install manually." >&2
        [ -f "$TARBALL" ] && rm -f "$TARBALL"
        exit 1
    fi
else
    echo "Error: need curl or wget to download." >&2
    exit 1
fi

echo "Extracting..."
tar -xzf "$TARBALL"
rm "$TARBALL"

echo "Done. Toolchain at tools/$DIR/bin/aarch64-linux-gnu-gcc"
"$DIR/bin/aarch64-linux-gnu-gcc" --version | head -1
