# debian-arm64-index.sh — sourced by the rootfs library fetchers: finds Debian
# arm64 packages by name in the suite's index, so a point release that replaces
# a version in the pool cannot break them, and unpacks their libraries.

DEBIAN_MIRROR="http://deb.debian.org/debian"
DEBIAN_DIST="bookworm"
# A .deb is typically under 2 MB; the cap stops a stalled mirror hanging the script.
DEB_TIMEOUT=90
# The index is about 12 MB, too much for that cap on a slow link. A stall still
# fails quickly: under 1 KiB/s for a minute.
INDEX_TIMEOUT=900

# Sets DEBIAN_LOOKUP to a temporary file of "package pool_path" lines; the
# caller removes it. Returns 1 with the reason on stderr if there is no usable index.
debian_index_load() {
    local packed progress=(-sS)
    packed=$(mktemp)
    DEBIAN_LOOKUP=$(mktemp)
    [ -t 2 ] && progress=(--progress-bar)
    echo "Downloading the Debian $DEBIAN_DIST arm64 package index (about 12 MB) ..."
    if ! curl -fL "${progress[@]}" --connect-timeout 15 --speed-limit 1024 --speed-time 60 \
            --max-time "$INDEX_TIMEOUT" --retry 1 \
            -o "$packed" "$DEBIAN_MIRROR/dists/$DEBIAN_DIST/main/binary-arm64/Packages.gz"; then
        echo "Error: could not download Packages.gz from $DEBIAN_MIRROR" >&2
        rm -f "$packed"
        return 1
    fi
    if ! gunzip -c "$packed" | awk '
            /^Package: / { pkg=$2 }
            /^Filename: / { if (pkg != "") { print pkg " " $2; pkg="" } }
        ' > "$DEBIAN_LOOKUP" || [ ! -s "$DEBIAN_LOOKUP" ]; then
        echo "Error: the Packages.gz from $DEBIAN_MIRROR is corrupt or lists no packages" >&2
        rm -f "$packed"
        return 1
    fi
    rm -f "$packed"
}

debian_pool_path() {
    awk -v p="$1" '$1==p { print $2; exit }' "$DEBIAN_LOOKUP"
}

# Copies the lib*.so* files of the package named $1 into the absolute directory $2.
# Prints one status line. Returns 1 on failure, which callers count and move past.
fetch_deb() {
    local pkg="$1" libdir="$2" pkg_path tmp_dir error
    pkg_path=$(debian_pool_path "$pkg")
    if [ -z "$pkg_path" ]; then
        echo "  $pkg: NOT FOUND in index (skipping)"
        return 1
    fi
    tmp_dir=$(mktemp -d)
    echo -n "  $(basename "$pkg_path") ... "
    if ! error=$(curl -fsSL --connect-timeout 15 --max-time "$DEB_TIMEOUT" --retry 1 \
            -o "$tmp_dir/pkg.deb" "$DEBIAN_MIRROR/$pkg_path" 2>&1); then
        echo "FAILED: $error"
        rm -rf "$tmp_dir"
        return 1
    fi
    if ! error=$(cd "$tmp_dir" && { ar x pkg.deb data.tar.xz || ar x pkg.deb data.tar.gz; } 2>&1 \
            && tar -xf data.tar.* 2>&1 \
            && find . -name "lib*.so*" -exec cp -f {} "$libdir/" \; 2>&1); then
        echo "FAILED: could not unpack: $error"
        rm -rf "$tmp_dir"
        return 1
    fi
    rm -rf "$tmp_dir"
    echo "OK"
}
