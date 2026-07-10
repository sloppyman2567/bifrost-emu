#!/bin/bash
# fetch-glibc-toolchain.sh — download a prebuilt glibc aarch64 cross-toolchain.
#
# This is OPTIONAL — bifrost-emu primarily targets static musl binaries.
# However, some programs are built against glibc (e.g., distro packages).
# This script fetches a prebuilt aarch64-none-linux-gnu toolchain from the
# Arm developer site.
#
# Turn 81: switched to Arm GNU 15.2.Rel1 (glibc 2.42, GCC 15.2) as the
# primary download. glibc 2.42 is the latest stable and produces DT_RELR
# by default (supported since Turn 78). Falls back to 14.2.Rel1 (glibc
# 2.40) then 13.2.Rel1 (glibc 2.38) if the newer downloads fail.
# An existing tools/aarch64-linux-gnu-cross/ is left in place (not
# upgraded automatically) — delete it to re-fetch a newer version.
#
# The download is bounded by a hard 180-second total timeout (the 15.2
# toolchain is ~151 MB).
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
DOWNLOAD_TIMEOUT=180
CURL_OPTS=(--connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 -fL)

if [ -x "$DIR/bin/aarch64-none-linux-gnu-gcc" ]; then
    echo "glibc toolchain already present at tools/$DIR/"
    "$DIR/bin/aarch64-none-linux-gnu-gcc" --version | head -1
    exit 0
fi

# Try versions from newest to oldest. 15.2.Rel1 (glibc 2.42) is the
# latest as of 2026-07. 14.2.Rel1 (glibc 2.40) and 13.2.Rel1 (glibc
# 2.38) are fallbacks for environments where the Arm CDN hasn't
# propagated the latest release.
VERSIONS=("15.2.rel1" "14.2.rel1" "13.2.rel1")
TARBALL_XZ=""
URL=""

for ver in "${VERSIONS[@]}"; do
    TARBALL_XZ="arm-gnu-toolchain-${ver}-x86_64-aarch64-none-linux-gnu.tar.xz"
    URL="https://developer.arm.com/-/media/Files/downloads/gnu/${ver}/binrel/${TARBALL_XZ}"
    echo "Trying Arm GNU $ver ..."
    if command -v curl >/dev/null 2>&1; then
        if curl "${CURL_OPTS[@]}" -o "$TARBALL_XZ" "$URL"; then
            echo "Downloaded $ver successfully."
            break
        fi
    elif command -v wget >/dev/null 2>&1; then
        if wget --timeout="$DOWNLOAD_TIMEOUT" --tries=1 -O "$TARBALL_XZ" "$URL"; then
            echo "Downloaded $ver successfully."
            break
        fi
    else
        echo "Error: need curl or wget to download." >&2
        exit 1
    fi
    echo "  $ver not available, trying next..."
    rm -f "$TARBALL_XZ"
    TARBALL_XZ=""
    URL=""
done

if [ -z "$TARBALL_XZ" ] || [ ! -f "$TARBALL_XZ" ]; then
    echo "Error: all Arm toolchain downloads failed." >&2
    exit 1
fi

echo "Extracting..."
tar -xJf "$TARBALL_XZ"
rm "$TARBALL_XZ"
mv arm-gnu-toolchain-* "$DIR"

echo "Done. Toolchain at tools/$DIR/bin/aarch64-none-linux-gnu-gcc"
"$DIR/bin/aarch64-none-linux-gnu-gcc" --version | head -1
