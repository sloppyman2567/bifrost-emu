#!/bin/bash
# Fetch the Tracy profiler's client sources (public/ and LICENSE) for `make USE_TRACY=1`. The release is pinned by version and
# SHA-256. An existing copy of that version is kept. The profiler window that shows the timeline is a separate program: build
# it from the same release (see Docs/JIT.md, "Timeline with Tracy"); its version must match the client's.
set -euo pipefail
cd "$(dirname "$0")/.."
version=0.15.0
checksum=3d57da54aa67d7875da1e8fecd0bd5957675cef73c27dd52bffb6ab8d290a5e6
install_dir=tracy
if [[ -f "$install_dir/public/TracyClient.cpp" && "$(cat "$install_dir/VERSION" 2>/dev/null)" == "$version" ]]; then
    echo "Tracy $version already present at Tools/$install_dir/"; exit 0
fi
if [[ -e "$install_dir" ]]; then
    echo "Error: Tools/$install_dir exists with another version or incomplete; move it aside before retrying." >&2; exit 1
fi
stage=$(mktemp -d .tracy-fetch.XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
url="https://github.com/wolfpld/tracy/archive/refs/tags/v$version.tar.gz"
echo "Downloading $url ..."
if command -v curl >/dev/null 2>&1; then
    curl -fL --connect-timeout 15 --max-time 300 --retry 1 -o "$stage/tracy.tar.gz" "$url"
elif command -v wget >/dev/null 2>&1; then
    timeout -k 5 300 wget --timeout=15 --tries=2 -O "$stage/tracy.tar.gz" "$url"
else
    echo 'Error: need curl or wget to download.' >&2; exit 1
fi
printf '%s  %s\n' "$checksum" "$stage/tracy.tar.gz" | sha256sum -c -
mkdir "$stage/extract"
tar -xzf "$stage/tracy.tar.gz" -C "$stage/extract" --strip-components=1 "tracy-$version/public" "tracy-$version/LICENSE"
[[ -f "$stage/extract/public/TracyClient.cpp" ]] || { echo 'Error: archive has no public/TracyClient.cpp' >&2; exit 1; }
printf '%s\n' "$version" > "$stage/extract/VERSION"
mv "$stage/extract" "$install_dir"
echo "Done. Tracy $version client sources at Tools/$install_dir/public (BSD 3-clause, see Tools/$install_dir/LICENSE)."
