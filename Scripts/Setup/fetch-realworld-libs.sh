#!/bin/bash
# fetch-realworld-libs.sh — download common AArch64 shared libraries for
# the rootfs. These are needed by real-world dynamically-linked glibc
# programs (coreutils, iperf3, etc.) that use optional features like
# SELinux, PCRE2 regex, OpenSSL, etc.
#
# Usage: ./Scripts/Setup/fetch-realworld-libs.sh [rootfs-dir]
#
# Downloads the current versions from the Debian package index
# (debian-arm64-index.sh, which also sets the download limits). Exits non-zero,
# with the reason, when there is no usable index; a package that fails is
# reported and skipped.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
# shellcheck source=Scripts/Setup/debian-arm64-index.sh
source "$PROJECT_ROOT/Scripts/Setup/debian-arm64-index.sh"

PACKAGES=(
    libselinux1     # needed by coreutils
    libpcre2-8-0    # needed by libselinux for regex
    libssl3         # libssl and libcrypto: needed by iperf3, curl, etc.
    libiperf0       # needed by iperf3
    zlib1g          # needed by many programs for compression
    libblkid1       # needed by many coreutils-like programs
    libmount1       # needed by some util-linux programs
    libuuid1        # needed by some programs
)

LIBDIR="$(realpath -m "$ROOTFS/lib")"
if [ ! -d "$LIBDIR" ]; then
    echo "Error: $LIBDIR does not exist; create the rootfs first" >&2
    exit 1
fi

DEBIAN_LOOKUP=""
trap 'rm -f "$DEBIAN_LOOKUP"' EXIT
debian_index_load

echo "Fetching real-world AArch64 shared libraries into $LIBDIR/ ..."
SUCCESS=0
FAILED=0
for pkg in "${PACKAGES[@]}"; do
    if fetch_deb "$pkg" "$LIBDIR"; then
        SUCCESS=$((SUCCESS + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

# Fix symlinks (some .deb packages install versioned .so files but
# the symlinks point to relative paths that don't exist in rootfs/lib/)
cd "$ROOTFS/lib"
for so_file in lib*.so.*; do
    [ -f "$so_file" ] || continue
    [ -L "$so_file" ] && continue  # skip existing symlinks
    # libfoo.so.1.2.3 → base=libfoo.so, major=libfoo.so.1
    base=$(echo "$so_file" | sed 's/\.so\..*//').so
    major=$(echo "$so_file" | sed 's/\(\.so\.[0-9]*\)\..*/\1/')
    [ -e "$base" ] || ln -sf "$so_file" "$base"
    [ -e "$major" ] || ln -sf "$so_file" "$major"
done
# Also handle files like libfoo.so.1 (only major version, no minor)
for so_file in lib*.so.[0-9]*; do
    [ -f "$so_file" ] || continue
    [ -L "$so_file" ] && continue
    base=$(echo "$so_file" | sed 's/\.so\..*//').so
    [ -e "$base" ] || ln -sf "$so_file" "$base"
done

echo ""
echo "Done. $SUCCESS packages installed, $FAILED failed."
echo "Total .so files in $ROOTFS/lib/:"
find "$LIBDIR" -maxdepth 1 -name 'lib*.so*' | wc -l
