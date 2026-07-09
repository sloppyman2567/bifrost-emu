#!/bin/bash
# fetch-musl-toolchain.sh — download the prebuilt musl aarch64 cross-toolchain.
#
# We don't bundle the 104 MB tarball in the repo (it bloats git history
# and release tarballs). Instead, this script downloads it on demand from
# musl.cc and extracts it under tools/.
#
# The download is bounded by a hard 90-second total timeout (--max-time for
# curl, --timeout for wget). Without a cap, a stalled mirror or a flaky
# network can hang the bootstrap indefinitely — particularly painful in CI
# where the job would just run until the runner's wall-clock limit. 90 s is
# generous for a 104 MB tarball on a 1.5 MB/s link, while still being short
# enough to fail fast so the caller can retry or surface the error.
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

# Hard total timeout (seconds) for the download phase. Covers connect +
# transfer + any retries the tool decides to do internally. Bump this only
# if you are intentionally fetching over a very slow link.
DOWNLOAD_TIMEOUT=90

if [ -x "$DIR/bin/aarch64-linux-musl-gcc" ]; then
    echo "musl toolchain already present at tools/$DIR/"
    exit 0
fi

echo "Downloading $URL (timeout: ${DOWNLOAD_TIMEOUT}s) ..."
if command -v curl >/dev/null 2>&1; then
    # --max-time caps the WHOLE transfer (connect + body) at 90 s.
    # --connect-timeout 15 fails fast if the server is unreachable, so we
    # don't burn the full 90 s on a dead host. --retry 1 allows a single
    # retry on a transient curl error (e.g. a TCP reset) without spiraling
    # into a long retry storm.
    if ! curl -fL --connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 \
            -o "$TARBALL" "$URL"; then
        echo "Error: download failed (curl exit $?). Check connectivity or" >&2
        echo "       retry manually with: curl -fL -o $TARBALL $URL" >&2
        [ -f "$TARBALL" ] && rm -f "$TARBALL"
        exit 1
    fi
elif command -v wget >/dev/null 2>&1; then
    # wget --timeout sets connect+read+write timeouts all at once; combined
    # with --tries=1 we get the same "fail fast, don't hang" behavior as
    # the curl branch. wget has no single "total transfer time" cap, but
    # the per-operation timeout bounds the worst-case stall to 90 s.
    if ! wget --timeout="$DOWNLOAD_TIMEOUT" --tries=1 -O "$TARBALL" "$URL"; then
        echo "Error: download failed (wget exit $?). Check connectivity or" >&2
        echo "       retry manually with: wget -O $TARBALL $URL" >&2
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

echo "Done. Toolchain at tools/$DIR/bin/aarch64-linux-musl-gcc"
"$DIR/bin/aarch64-linux-musl-gcc" --version | head -1
