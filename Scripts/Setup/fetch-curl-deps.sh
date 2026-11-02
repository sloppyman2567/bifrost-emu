#!/bin/bash
# fetch-curl-deps.sh — download all shared libraries curl needs to run.
#
# curl has a deep dependency tree (libnghttp2, libidn2, librtmp, libssh2,
# libpsl, libgssapi_krb5, libldap, libzstd, libbrotli, etc.). This script
# fetches them all from the Debian arm64 package mirror and installs them
# into the rootfs.
#
# Usage: ./Scripts/Setup/fetch-curl-deps.sh [rootfs-dir]
#
# Pool paths come from the Debian Packages.gz index (debian-arm64-index.sh),
# which also sets the download limits. Exits non-zero, with the reason, when
# there is no usable index; a package that fails is reported and skipped.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
# shellcheck source=Scripts/Setup/debian-arm64-index.sh
source "$PROJECT_ROOT/Scripts/Setup/debian-arm64-index.sh"

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

LIBDIR="$(realpath -m "$ROOTFS/lib")"
if [ ! -d "$LIBDIR" ]; then
    echo "Error: $LIBDIR does not exist; create the rootfs first" >&2
    exit 1
fi

DEBIAN_LOOKUP=""
trap 'rm -f "$DEBIAN_LOOKUP"' EXIT
debian_index_load

echo "Fetching ${#PACKAGES[@]} curl dependency libraries into $LIBDIR/ ..."
SUCCESS=0
FAILED=0
for pkg in "${PACKAGES[@]}"; do
    if fetch_deb "$pkg" "$LIBDIR"; then
        SUCCESS=$((SUCCESS + 1))
    else
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
find "$LIBDIR" -maxdepth 1 -name 'lib*.so*' | wc -l
