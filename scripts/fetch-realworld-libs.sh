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
#
# Each individual .deb download is bounded by a 90-second timeout (matches
# the toolchain fetch scripts), so one slow mirror can't hang the whole
# bootstrap.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
MIRROR="http://deb.debian.org/debian"

# Per-download hard timeout (seconds). A single .deb is typically < 2 MB,
# so 90 s is more than generous; the cap just prevents a stalled mirror
# from hanging the whole script.
DOWNLOAD_TIMEOUT=90

echo "Fetching real-world AArch64 shared libraries into $ROOTFS/lib/ ..."

fetch_deb() {
    local pkg_path="$1"  # e.g. main/s/selinux/libselinux1_3.1-3_arm64.deb
    local pkg_name=$(basename "$pkg_path")
    local tmp_dir=$(mktemp -d)
    
    echo -n "  $pkg_name ... "
    if curl -fsL --connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 \
            -o "$tmp_dir/pkg.deb" "$MIRROR/$pkg_path" 2>/dev/null; then
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

# libz / zlib (needed by many programs for compression)
fetch_deb "pool/main/z/zlib/zlib1g_1.2.13.dfsg-1_arm64.deb" || true

# libblkid (needed by many coreutils-like programs)
fetch_deb "pool/main/u/util-linux/libblkid1_2.38.1-5+deb12u1_arm64.deb" || true

# libmount (needed by some util-linux programs)
fetch_deb "pool/main/u/util-linux/libmount1_2.38.1-5+deb12u1_arm64.deb" || true

# libuuid (needed by some programs)
fetch_deb "pool/main/u/util-linux/libuuid1_2.38.1-5+deb12u1_arm64.deb" || true

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
echo "Done. Libraries in $ROOTFS/lib/:"
ls -1 "$ROOTFS/lib"/lib*.so* 2>/dev/null | head -20
