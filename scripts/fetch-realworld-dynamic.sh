#!/bin/bash
# fetch-realworld-dynamic.sh — fetch dynamic AArch64 binaries (iperf3,
# coreutils) from Debian arm64 for real-world dynamic testing.
#
# These are DYNAMICALLY LINKED binaries that need the rootfs (glibc + deps).
# Place in ctest_real/realworld/ alongside the static busybox.
#
# Usage:
#   ./scripts/fetch-realworld-dynamic.sh
#
# Requirements:
#   - rootfs set up (./scripts/setup-rootfs.sh with glibc toolchain)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

DEST="ctest_real/realworld"
mkdir -p "$DEST"

DOWNLOAD_TIMEOUT=90
CURL_OPTS=(--connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 -fL)

# Debian arm64 mirror — packages are .deb files containing the binary.
DEB_MIRROR="http://ftp.debian.org/debian/pool/main"

# Helper: download a .deb, extract the binary from data.tar.
# Args: package_name deb_url binary_path_inside_deb dest_name
fetch_deb_binary() {
    local pkg="$1" url="$2" inner="$3" dest="$4"
    local tmpdir
    tmpdir=$(mktemp -d) || { echo "  (mktemp failed)" >&2; return 1; }
    local debfile="$tmpdir/$pkg.deb"

    if ! curl "${CURL_OPTS[@]}" -o "$debfile" "$url" 2>/dev/null; then
        echo "  (could not download $pkg from $url)"
        rm -rf "$tmpdir"
        return 1
    fi

    # .deb is an ar archive: debian-binary, control.tar.*, data.tar.*
    # Extract data.tar.xz (or .gz) and find the binary.
    cd "$tmpdir"
    ar x "$debfile" 2>/dev/null || {
        echo "  (ar extract failed for $pkg)"
        cd "$PROJECT_ROOT"; rm -rf "$tmpdir"; return 1
    }

    # Try data.tar.xz first, then data.tar.gz
    local datafile=""
    if [ -f data.tar.xz ]; then datafile=data.tar.xz
    elif [ -f data.tar.gz ]; then datafile=data.tar.gz
    elif [ -f data.tar.zst ]; then datafile=data.tar.zst
    fi

    if [ -z "$datafile" ]; then
        echo "  (no data.tar in $pkg.deb)"
        cd "$PROJECT_ROOT"; rm -rf "$tmpdir"; return 1
    fi

    # Extract the binary
    case "$datafile" in
        *.xz)  tar xJf "$datafile" "./$inner" 2>/dev/null || true ;;
        *.gz)  tar xzf "$datafile" "./$inner" 2>/dev/null || true ;;
        *.zst) tar --zstd -xf "$datafile" "./$inner" 2>/dev/null || true ;;
    esac

    if [ -f "./$inner" ]; then
        cp "./$inner" "$PROJECT_ROOT/$dest"
        chmod +x "$PROJECT_ROOT/$dest"
        echo "  OK: $dest"
        cd "$PROJECT_ROOT"; rm -rf "$tmpdir"
        return 0
    else
        echo "  (binary $inner not found in $pkg.deb)"
        cd "$PROJECT_ROOT"; rm -rf "$tmpdir"
        return 1
    fi
}

echo "=== Fetching dynamic real-world binaries (Debian arm64) ==="
echo ""

# ── iperf3 ────────────────────────────────────────────────────────────
IPERF3_DEST="$DEST/iperf3-aarch64"
if [ -x "$IPERF3_DEST" ]; then
    echo "iperf3 already present"
else
    echo "Fetching iperf3..."
    # Try known versions newest-to-oldest.
    IPERF3_URL=""
    for ver in "3.20-2.1" "3.18-2+deb13u2" "3.12-1+deb12u2" "3.9-1+deb11u1"; do
        url="$DEB_MIRROR/i/iperf3/iperf3_${ver}_arm64.deb"
        if curl --connect-timeout 5 --max-time 10 -fL -I "$url" >/dev/null 2>&1; then
            IPERF3_URL="$url"
            break
        fi
    done
    if [ -n "$IPERF3_URL" ]; then
        fetch_deb_binary "iperf3" "$IPERF3_URL" "usr/bin/iperf3" "$IPERF3_DEST" || true
        # Also fetch libiperf0 (runtime dependency) into rootfs/lib/
        LIBIPERF_VER="${ver%_*}"  # strip _arm64.deb suffix variant
        # The libiperf0 version matches the iperf3 version
        liburl="$DEB_MIRROR/i/iperf3/libiperf0_${ver}_arm64.deb"
        if [ -d rootfs/lib ]; then
            echo "  Fetching libiperf0..."
            ltmp=$(mktemp -d)
            if curl "${CURL_OPTS[@]}" -o "$ltmp/libiperf.deb" "$liburl" 2>/dev/null; then
                cd "$ltmp" && ar x libiperf.deb 2>/dev/null
                for df in data.tar.xz data.tar.gz data.tar.zst; do
                    [ -f "$df" ] && datafile="$df" && break
                done
                if [ -n "${datafile:-}" ]; then
                    case "$datafile" in
                        *.xz)  tar xJf "$datafile" 2>/dev/null || true ;;
                        *.gz)  tar xzf "$datafile" 2>/dev/null || true ;;
                        *.zst) tar --zstd -xf "$datafile" 2>/dev/null || true ;;
                    esac
                    # Copy the .so file
                    sofile=$(find . -name 'libiperf.so.0.*' -not -type l 2>/dev/null | head -1)
                    if [ -n "$sofile" ]; then
                        cp "$sofile" "$PROJECT_ROOT/rootfs/lib/"
                        # Create symlink
                        soname=$(basename "$sofile" | sed 's/\.so\.0\..*/\.so\.0/')
                        ln -sf "$(basename "$sofile")" "$PROJECT_ROOT/rootfs/lib/$soname"
                        echo "    OK: $soname"
                    fi
                fi
                cd "$PROJECT_ROOT"
            fi
            rm -rf "$ltmp"
        fi
    else
        echo "  (could not find iperf3 on Debian mirror — skipping)"
    fi
fi
echo ""

# ── coreutils ─────────────────────────────────────────────────────────
# coreutils is a single package containing echo, cat, uname, date, readlink, etc.
# Binary: /usr/bin/echo, /usr/bin/cat, etc.
COREUTILS_VERSIONS="9.10-1 9.7-3 9.1-1 8.32-4"
COREUTILS_URL=""
for ver in $COREUTILS_VERSIONS; do
    url="$DEB_MIRROR/c/coreutils/coreutils_${ver}_arm64.deb"
    if curl --connect-timeout 5 --max-time 10 -fL -I "$url" >/dev/null 2>&1; then
        COREUTILS_URL="$url"
        break
    fi
done

if [ -z "$COREUTILS_URL" ]; then
    echo "  (could not find coreutils on Debian mirror — skipping)"
else
    echo "Fetching coreutils (single .deb, extract multiple binaries)..."
    tmpdir=$(mktemp -d)
    debfile="$tmpdir/coreutils.deb"
    if curl "${CURL_OPTS[@]}" -o "$debfile" "$COREUTILS_URL" 2>/dev/null; then
        cd "$tmpdir"
        ar x "$debfile" 2>/dev/null
        datafile=""
        for f in data.tar.xz data.tar.gz data.tar.zst; do
            [ -f "$f" ] && datafile="$f" && break
        done
        if [ -n "$datafile" ]; then
            # Extract all of /usr/bin/
            case "$datafile" in
                *.xz)  tar xJf "$datafile" 2>/dev/null || true ;;
                *.gz)  tar xzf "$datafile" 2>/dev/null || true ;;
                *.zst) tar --zstd -xf "$datafile" 2>/dev/null || true ;;
            esac
            for bin in echo cat uname date readlink; do
                src="./usr/bin/$bin"
                dst="$PROJECT_ROOT/$DEST/$bin"
                if [ -f "$src" ]; then
                    cp "$src" "$dst"
                    chmod +x "$dst"
                    echo "  OK: $DEST/$bin"
                else
                    echo "  (missing $bin in coreutils.deb)"
                fi
            done
        fi
        cd "$PROJECT_ROOT"
    else
        echo "  (could not download coreutils .deb)"
    fi
    rm -rf "$tmpdir"
fi

echo ""
echo "=== Dynamic real-world binaries ready ==="
ls -lh "$DEST"/iperf3-aarch64 "$DEST"/echo "$DEST"/cat "$DEST"/uname "$DEST"/date "$DEST"/readlink 2>/dev/null || true
echo ""
echo "To run real-world dynamic tests:"
echo "  BIFROST_ROOT=./rootfs ./scripts/run_tests.sh --test-all"
