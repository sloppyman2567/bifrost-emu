#!/bin/bash
# fetch-realworld-binaries.sh — fetch/build real-world AArch64 binaries
# for integration testing.
#
# This script fetches or builds static AArch64 binaries that we use for
# real-world testing. Currently supports:
#   - BusyBox 1.36.1 (built from source with musl-cross)
#
# Future: iperf2, curl, coreutils, etc.
#
# The binaries are placed in ctest_real/realworld/ and are gitignored
# (fetched on demand to keep the repo small).
#
# Usage:
#   ./scripts/fetch-realworld-binaries.sh
#
# Requirements:
#   - musl cross-toolchain at tools/aarch64-linux-musl-cross/
#     (fetch via ./tools/fetch-musl-toolchain.sh)
#   - make, gcc (for building busybox from source)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

DEST="ctest_real/realworld"
mkdir -p "$DEST"

TOOLCHAIN="tools/aarch64-linux-musl-cross"
if [ ! -x "$TOOLCHAIN/bin/aarch64-linux-musl-gcc" ]; then
    echo "Error: musl toolchain not found at $TOOLCHAIN/"
    echo "  Run ./tools/fetch-musl-toolchain.sh first."
    exit 1
fi

export PATH="$PROJECT_ROOT/$TOOLCHAIN/bin:$PATH"

# ── BusyBox ────────────────────────────────────────────────────────────
BB_VERSION="1.36.1"
BB_DEST="$DEST/busybox-aarch64"

if [ -x "$BB_DEST" ]; then
    echo "BusyBox already present at $BB_DEST"
else
    echo "=== Building BusyBox $BB_VERSION (static aarch64 musl) ==="
    TMPDIR=$(mktemp -d)
    trap 'rm -rf "$TMPDIR"' EXIT

    curl -fL -o "$TMPDIR/busybox.tar.bz2" \
        "https://busybox.net/downloads/busybox-$BB_VERSION.tar.bz2"
    tar xjf "$TMPDIR/busybox.tar.bz2" -C "$TMPDIR"
    cd "$TMPDIR/busybox-$BB_VERSION"

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-musl- defconfig
    # Static linking
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    # Disable tc (broken in 1.36.1 with musl)
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-musl- -j"$(nproc)" 2>&1 | tail -5
    cp busybox "$PROJECT_ROOT/$BB_DEST"
    chmod +x "$PROJECT_ROOT/$BB_DEST"
    echo "BusyBox built: $BB_DEST"
    file "$PROJECT_ROOT/$BB_DEST"
fi

echo ""
echo "=== Real-world binaries ready ==="
ls -lh "$DEST"/
echo ""
echo "To run real-world tests:"
echo "  ./scripts/run_tests.sh --realworld"
