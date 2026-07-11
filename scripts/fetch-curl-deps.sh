#!/bin/bash
# fetch-curl-deps.sh — download all shared libraries curl needs to run.
#
# curl has a deep dependency tree (libnghttp2, libidn2, librtmp, libssh2,
# libpsl, libgssapi_krb5, libldap, libzstd, libbrotli, etc.). This script
# fetches them all from the Debian arm64 package mirror and installs them
# into the rootfs.
#
# Usage: ./scripts/fetch-curl-deps.sh [rootfs-dir]
#
# Each download is bounded by a 90-second timeout (matches the other
# fetch scripts). The script looks up the correct pool paths by querying
# the Debian Packages.gz index, so it's resilient to version bumps.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
MIRROR="http://deb.debian.org/debian"
DIST="bookworm"
DOWNLOAD_TIMEOUT=90

# Packages that curl (Debian Bookworm arm64) depends on, transitively.
# libbrotlidec1/libbrotlicommon1 are bundled into libbrotli1 in Bookworm.
# liblber-2.5-0 is bundled into libldap-2.5-0.
PACKAGES=(
    libcurl4
    zlib1g
    libssl3
    libnghttp2-14
    libidn2-0
    librtmp1
    libssh2-1
    libpsl5
    libgssapi-krb5-2
    libkrb5-3
    libkrb5support0
    libk5crypto3
    libcom-err2
    libldap-2.5-0
    libzstd1
    libbrotli1
    libgnutls30
    libhogweed6
    libnettle8
    libgmp10
    libtasn1-6
    libp11-kit0
    libidn12
    libffi8
    # Transitive deps discovered on first run:
    libsasl2-2          # libldap SASL plugin
    libunistring2       # libidn2 needs it
    libkeyutils1        # libkrb5 needs it
)

echo "Looking up package paths in Debian $DIST arm64 index ..."
PKG_INDEX=$(mktemp)
PKG_GZ="$PKG_INDEX.gz"
LOOKUP=$(mktemp)
trap 'rm -f "$PKG_INDEX" "$PKG_GZ" "$LOOKUP"' EXIT
curl -fsL --connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 \
    -o "$PKG_GZ" "$MIRROR/dists/$DIST/main/binary-arm64/Packages.gz" || {
    echo "Error: could not download Packages.gz from $MIRROR" >&2
    exit 1
}
gunzip -f "$PKG_GZ" 2>/dev/null || true

# Build a lookup file: "package_name pool_path" per line. awk handles the
# multi-line stanza format cleanly.
awk '
    /^Package: / { pkg=$2 }
    /^Filename: / { if (pkg != "") { print pkg " " $2; pkg="" } }
' "$PKG_INDEX" > "$LOOKUP"

echo "Fetching ${#PACKAGES[@]} curl dependency libraries into $ROOTFS/lib/ ..."
SUCCESS=0
FAILED=0
for pkg in "${PACKAGES[@]}"; do
    pkg_path=$(awk -v p="$pkg" '$1==p { print $2; exit }' "$LOOKUP")
    if [ -z "$pkg_path" ]; then
        echo "  $pkg: NOT FOUND in index (skipping)"
        FAILED=$((FAILED + 1))
        continue
    fi
    pkg_name=$(basename "$pkg_path")
    tmp_dir=$(mktemp -d)
    echo -n "  $pkg_name ... "
    if curl -fsL --connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 \
            -o "$tmp_dir/pkg.deb" "$MIRROR/$pkg_path" 2>/dev/null; then
        cd "$tmp_dir"
        ar x pkg.deb data.tar.xz 2>/dev/null || ar x pkg.deb data.tar.gz 2>/dev/null
        if [ -f data.tar.xz ]; then
            tar -xJf data.tar.xz 2>/dev/null
        elif [ -f data.tar.gz ]; then
            tar -xzf data.tar.gz 2>/dev/null
        fi
        find . -name "lib*.so*" -exec cp -f {} "$ROOTFS/lib/" \; 2>/dev/null
        cd "$PROJECT_ROOT"
        rm -rf "$tmp_dir"
        echo "OK"
        SUCCESS=$((SUCCESS + 1))
    else
        echo "FAILED (download error)"
        rm -rf "$tmp_dir"
        FAILED=$((FAILED + 1))
    fi
done

# Fix symlinks for any newly-installed libraries
cd "$ROOTFS/lib"
for so_file in lib*.so.*; do
    [ -f "$so_file" ] || continue
    [ -L "$so_file" ] && continue
    base=$(echo "$so_file" | sed 's/\.so\..*//').so
    major=$(echo "$so_file" | sed 's/\(\.so\.[0-9]*\)\..*/\1/')
    [ -e "$base" ] || ln -sf "$so_file" "$base"
    [ -e "$major" ] || ln -sf "$so_file" "$major"
done

echo ""
echo "Done. $SUCCESS packages installed, $FAILED failed."
echo "Total .so files in $ROOTFS/lib/:"
ls -1 "$ROOTFS/lib"/lib*.so* 2>/dev/null | wc -l
