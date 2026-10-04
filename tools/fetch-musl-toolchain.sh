#!/bin/bash
# Fetch AArch64 musl GCC. Existing toolchains are preserved. CI selects the
# pinned Bootlin source; musl.cc remains the default with Bootlin fallback.
set -euo pipefail
cd "$(dirname "$0")"
source=${1:-musl.cc}
if [[ "$source" != musl.cc && "$source" != --bootlin ]]; then
    echo 'Usage: fetch-musl-toolchain.sh [--bootlin]' >&2; exit 2
fi
install_dir=aarch64-linux-musl-cross
if [[ -x "$install_dir/bin/aarch64-linux-musl-gcc" ]]; then
    echo "musl toolchain already present at tools/$install_dir/"; exit 0
fi
if [[ -e "$install_dir" ]]; then
    echo "Error: incomplete $install_dir exists; move it aside before retrying." >&2; exit 1
fi
stage=$(mktemp -d .musl-fetch.XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
download() {
    local url=$1 output=$2 rc
    echo "Downloading $url ..."
    if command -v curl >/dev/null 2>&1; then
        if curl -fL --connect-timeout 15 --max-time 180 --retry 1 -o "$output" "$url"; then return 0; else rc=$?; fi
        echo "Error: curl download failed (exit $rc): $url" >&2
    elif command -v wget >/dev/null 2>&1; then
        if timeout -k 5 180 wget --timeout=15 --tries=2 -O "$output" "$url"; then return 0; else rc=$?; fi
        echo "Error: wget download failed (exit $rc): $url" >&2
    else
        echo 'Error: need curl or wget to download.' >&2; return 1
    fi
    return "$rc"
}
if [[ "$source" == musl.cc ]] && download https://musl.cc/aarch64-linux-musl-cross.tgz "$stage/toolchain.tgz"; then
    tar -xzf "$stage/toolchain.tgz" -C "$stage"
else
    echo 'Using checksum-pinned Bootlin musl toolchain.'
    name=aarch64--musl--stable-2024.05-1
    checksum=f847da1195325525f3f07eef045ef40c6b48464a37e0f7fea77360dfe0bc1aa1
    download "https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$name.tar.xz" "$stage/toolchain.tar.xz"
    printf '%s  %s\n' "$checksum" "$stage/toolchain.tar.xz" | sha256sum -c -
    tar -xf "$stage/toolchain.tar.xz" -C "$stage"
    mv "$stage/$name" "$stage/$install_dir"
    # Preserve the established compiler and runtime lookup paths. Bootlin's
    # real compiler keeps its original target tuple and relative SDK layout.
    for tool in "$stage/$install_dir"/bin/aarch64-buildroot-linux-musl-*; do
        suffix=${tool##*/aarch64-buildroot-linux-musl-}
        ln -s "${tool##*/}" "$stage/$install_dir/bin/aarch64-linux-musl-$suffix"
    done
    ln -s aarch64-buildroot-linux-musl/sysroot "$stage/$install_dir/aarch64-linux-musl"
fi
[[ -x "$stage/$install_dir/bin/aarch64-linux-musl-gcc" ]] || { echo 'Error: archive has no expected compiler' >&2; exit 1; }
mv "$stage/$install_dir" "$install_dir"
if [[ -f "$install_dir/relocate-sdk.sh" ]]; then
    bash "$install_dir/relocate-sdk.sh"
fi
"$install_dir/bin/aarch64-linux-musl-gcc" --version | head -1
echo "Done. Toolchain at tools/$install_dir/bin/aarch64-linux-musl-gcc"
