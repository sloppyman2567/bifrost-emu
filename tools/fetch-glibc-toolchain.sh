#!/bin/bash
# fetch-glibc-toolchain.sh — download a prebuilt glibc aarch64 cross-toolchain.
#
# This is OPTIONAL — bifrost-emu primarily targets static musl binaries.
# However, some programs are built against glibc (e.g., distro packages).
# This script fetches a prebuilt aarch64-none-linux-gnu toolchain from the
# Arm developer site (glibc 2.40 / GCC 14.2 as of the 14.2.Rel1 release).
#
# Turn 78: switched from Arm 13.2.Rel1 (glibc 2.38) to 14.2.Rel1
# (glibc 2.40) as the primary download. glibc 2.40 ships DT_RELR
# (compact relative relocations) in libc.so.6 by default, which
# bifrost-emu now supports. The older 13.2.Rel1 is the fallback.
# An existing tools/aarch64-linux-gnu-cross/ from 13.2.Rel1 is left
# in place (not upgraded automatically) — delete it to re-fetch 14.2.
#
# The download is bounded by a hard 120-second total timeout (the 14.2
# toolchain is ~145 MB, slightly larger than 13.2's ~133 MB).
#
# Usage:
#   ./tools/fetch-glibc-toolchain.sh
#
# Then cross-compile with:
#   tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc -static -O2 -o foo.elf foo.c
set -euo pipefail
cd "$(dirname "$0")"

DIR="aarch64-linux-gnu-cross"

# Hard total timeout (seconds) for the download phase.
DOWNLOAD_TIMEOUT=120
CURL_OPTS=(--connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 -fL)

if [ -x "$DIR/bin/aarch64-none-linux-gnu-gcc" ]; then
    echo "glibc toolchain already present at tools/$DIR/"
    "$DIR/bin/aarch64-none-linux-gnu-gcc" --version | head -1
    exit 0
fi

# Primary: Arm GNU Toolchain 14.2.Rel1 (glibc 2.40, GCC 14.2).
# glibc 2.40 produces DT_RELR by default, which bifrost-emu now
# supports (Turn 78). Falls back to 13.2.Rel1 (glibc 2.38) if the
# 14.2 download fails.
TARBALL_XZ="arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
URL="https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"

echo "Downloading $URL (timeout: ${DOWNLOAD_TIMEOUT}s) ..."
if command -v curl >/dev/null 2>&1; then
    if ! curl "${CURL_OPTS[@]}" -o "$TARBALL_XZ" "$URL"; then
        echo "Arm 14.2.Rel1 not available, trying 13.2.Rel1 (glibc 2.38)..."
        URL="https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
        TARBALL_XZ="arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz"
        if ! curl "${CURL_OPTS[@]}" -o "$TARBALL_XZ" "$URL"; then
            echo "Error: both Arm toolchain downloads failed (curl exit $?)." >&2
            [ -f "$TARBALL_XZ" ] && rm -f "$TARBALL_XZ"
            exit 1
        fi
    fi
elif command -v wget >/dev/null 2>&1; then
    if ! wget --timeout="$DOWNLOAD_TIMEOUT" --tries=1 -O "$TARBALL_XZ" "$URL"; then
        echo "Error: wget download failed." >&2
        [ -f "$TARBALL_XZ" ] && rm -f "$TARBALL_XZ"
        exit 1
    fi
else
    echo "Error: need curl or wget to download." >&2
    exit 1
fi

echo "Extracting..."
tar -xJf "$TARBALL_XZ"
rm "$TARBALL_XZ"
mv arm-gnu-toolchain-* "$DIR"

echo "Done. Toolchain at tools/$DIR/bin/aarch64-none-linux-gnu-gcc"
"$DIR/bin/aarch64-none-linux-gnu-gcc" --version | head -1
