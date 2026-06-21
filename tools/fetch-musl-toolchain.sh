#!/bin/bash
# fetch-musl-toolchain.sh — download the prebuilt musl aarch64 cross-toolchain.
#
# We don't bundle the 104 MB tarball in the repo (it bloats git history
# and release tarballs). Instead, this script downloads it on demand from
# musl.cc and extracts it under tools/.
#
# Usage:
#   ./tools/fetch-musl-toolchain.sh
#
# Then cross-compile with:
#   tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc -static -O2 -o foo.elf foo.c
set -euo pipefail
cd "$(dirname "$0")"

URL="https://musl.cc/aarch64-linux-musl-cross.tgz"
TARBALL="aarch64-linux-musl-cross.tgz"
DIR="aarch64-linux-musl-cross"

if [ -x "$DIR/bin/aarch64-linux-musl-gcc" ]; then
    echo "musl toolchain already present at tools/$DIR/"
    exit 0
fi

echo "Downloading $URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -fL -o "$TARBALL" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TARBALL" "$URL"
else
    echo "Error: need curl or wget to download." >&2
    exit 1
fi

echo "Extracting..."
tar -xzf "$TARBALL"
rm "$TARBALL"

echo "Done. Toolchain at tools/$DIR/bin/aarch64-linux-musl-gcc"
"$DIR/bin/aarch64-linux-musl-gcc" --version | head -1
