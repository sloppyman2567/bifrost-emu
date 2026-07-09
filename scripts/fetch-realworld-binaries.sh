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

# Per-download hard timeout (seconds). Each source tarball is a few MB, but
# sourceforge / busybox.net mirrors can stall — bound them so the script
# fails fast instead of hanging indefinitely. Matches the toolchain fetch
# scripts' 90 s cap for consistency.
DOWNLOAD_TIMEOUT=90
CURL_OPTS=(--connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 -fL)

# ── BusyBox ────────────────────────────────────────────────────────────
BB_VERSION="1.36.1"
BB_DEST="$DEST/busybox-aarch64"

if [ -x "$BB_DEST" ]; then
    echo "BusyBox already present at $BB_DEST"
else
    echo "=== Building BusyBox $BB_VERSION (static aarch64 musl) ==="
    WORKDIR1=$(mktemp -d) || { echo "Failed to create temp dir" >&2; exit 1; }
    trap 'rm -rf "$WORKDIR1"' EXIT

    curl "${CURL_OPTS[@]}" -o "$WORKDIR1/busybox.tar.bz2" \
        "https://busybox.net/downloads/busybox-$BB_VERSION.tar.bz2"
    tar xjf "$WORKDIR1/busybox.tar.bz2" -C "$WORKDIR1"
    cd "$WORKDIR1/busybox-$BB_VERSION"

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

# ── iperf2 ─────────────────────────────────────────────────────────────
IPERF_DEST="$DEST/iperf2-aarch64"

if [ -x "$IPERF_DEST" ]; then
    echo "iperf2 already present at $IPERF_DEST"
else
    echo ""
    echo "=== Building iperf2 2.2.1 (static aarch64 musl) ==="
    WORKDIR2=$(mktemp -d) || { echo "Failed to create temp dir" >&2; exit 1; }
    trap 'rm -rf "$WORKDIR1" "$WORKDIR2"' EXIT

    if curl "${CURL_OPTS[@]}" -o "$WORKDIR2/iperf2.tar.gz" \
        "https://sourceforge.net/projects/iperf2/files/iperf2-2.2.1.tar.gz/download" 2>/dev/null; then
        tar xzf "$WORKDIR2/iperf2.tar.gz" -C "$WORKDIR2"
        cd "$WORKDIR2"/iperf2-*
        ./configure --host=aarch64-linux-musl --disable-shared --prefix=/usr \
            CC=aarch64-linux-musl-gcc CFLAGS="-static -O2" LDFLAGS="-static" 2>&1 | tail -3
        make -j"$(nproc)" 2>&1 | tail -3
        cp src/iperf "$PROJECT_ROOT/$IPERF_DEST" 2>/dev/null || echo "  (iperf2 build failed — skipping)"
        chmod +x "$PROJECT_ROOT/$IPERF_DEST" 2>/dev/null
        [ -f "$PROJECT_ROOT/$IPERF_DEST" ] && echo "iperf2 built: $IPERF_DEST"
    else
        echo "  (could not download iperf2 source — skipping)"
    fi
fi

# ── toybox (if not present in ctest_real/) ─────────────────────────────
TOYBOX_DEST="ctest_real/toybox"
if [ ! -f "$TOYBOX_DEST" ]; then
    echo ""
    echo "=== Downloading toybox ==="
    if curl "${CURL_OPTS[@]}" -o "$TOYBOX_DEST" \
        "https://landley.net/toybox/downloads/binaries/0.8.10/toybox-aarch64" 2>/dev/null; then
        chmod +x "$TOYBOX_DEST"
        echo "toybox downloaded: $TOYBOX_DEST"
    else
        echo "  (could not download toybox — skipping)"
    fi
fi

echo ""
echo "=== Real-world binaries ready ==="
ls -lh "$DEST"/
echo ""
echo "To run real-world tests:"
echo "  ./scripts/run_tests.sh --realworld"
