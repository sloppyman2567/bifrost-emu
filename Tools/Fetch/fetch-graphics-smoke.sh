#!/usr/bin/env bash
# Independent ARM64 graphics binaries, from Debian's Mesa demos package.
set -euo pipefail
cd "$(dirname "$0")/../.."
dest=Tools/graphics-smoke
mkdir -p "$dest/mesa"
package="$dest/mesa-utils-bin_arm64.deb"
if [[ ! -f "$package" ]]; then
    curl --fail --location --output "$package.tmp" \
      'https://deb.debian.org/debian/pool/main/m/mesa-demos/mesa-utils-bin_9.0.0-2+b3_arm64.deb'
    mv "$package.tmp" "$package"
fi
printf '%s  %s\n' \
  0027480ef4dca9b70d004aad29005c0a89872196c39ccbeb4ae4946b9064b24e "$package" | sha256sum --check
ar p "$package" data.tar.xz | tar -xJ -C "$dest/mesa"
file "$dest/mesa/usr/bin/es2tri.aarch64-linux-gnu"
