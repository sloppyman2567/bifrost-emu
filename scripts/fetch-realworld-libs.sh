#!/bin/bash
# fetch-realworld-libs.sh — download common AArch64 shared libraries for
# the rootfs. These are needed by real-world dynamically-linked glibc
# programs (coreutils, iperf3, etc.) that use optional features like
# SELinux, PCRE2 regex, OpenSSL, etc.
#
# Usage: ./scripts/fetch-realworld-libs.sh [rootfs-dir]
#
# Downloads from Debian package mirrors (stable, arm64). Idempotent —
# re-running only downloads missing libraries.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
MIRROR="http://deb.debian.org/debian"

echo "Fetching real-world AArch64 shared libraries into $ROOTFS/lib/ ..."

fetch_deb() {
    local pkg_path="$1"  # e.g. main/s/selinux/libselinux1_3.1-3_arm64.deb
    local pkg_name=$(basename "$pkg_path")
    local tmp_dir=$(mktemp -d)
    
    echo -n "  $pkg_name ... "
    if curl -fsL -o "$tmp_dir/pkg.deb" "$MIRROR/$pkg_path" 2>/dev/null; then
        cd "$tmp_dir"
        ar x pkg.deb data.tar.xz 2>/dev/null
        tar -xJf data.tar.xz 2>/dev/null
        # Copy all .so* files to rootfs/lib/
        find . -name "lib*.so*" -exec cp -f {} "$ROOTFS/lib/" \; 2>/dev/null
        cd "$PROJECT_ROOT"
        rm -rf "$tmp_dir"
        echo "OK"
    else
        echo "FAILED (download error)"
        rm -rf "$tmp_dir"
        return 1
    fi
}

# libselinux (needed by coreutils)
fetch_deb "pool/main/libs/libselinux/libselinux1_3.1-3_arm64.deb" || true

# libpcre2-8 (needed by libselinux for regex)
fetch_deb "pool/main/p/pcre2/libpcre2-8-0_10.42-1_arm64.deb" || true

# libssl3 / libcrypto3 (needed by iperf3, curl, etc.)
fetch_deb "pool/main/o/openssl/libssl3_3.0.17-1~deb12u2_arm64.deb" || true

# libiperf (needed by iperf3)
fetch_deb "pool/main/i/iperf3/libiperf0_3.12-1+deb12u2_arm64.deb" || true

# Fix symlinks (some .deb packages install versioned .so files but
# the symlinks point to relative paths that don't exist in rootfs/lib/)
cd "$ROOTFS/lib"
for so_file in lib*.so.*.*.*; do
    [ -f "$so_file" ] || continue
    # Extract the base name (e.g., libpcre2-8.so from libpcre2-8.so.0.11.2)
    base=$(echo "$so_file" | sed 's/\.so\..*//').so
    # Extract the major version (e.g., libpcre2-8.so.0 from libpcre2-8.so.0.11.2)
    major=$(echo "$so_file" | sed 's/\(\.so\.[0-9]*\)\..*/\1/')
    [ -L "$base" ] || ln -sf "$so_file" "$base"
    [ -L "$major" ] || ln -sf "$so_file" "$major"
done

echo ""
echo "Done. Libraries in $ROOTFS/lib/:"
ls -1 "$ROOTFS/lib"/lib*.so* 2>/dev/null | head -20
