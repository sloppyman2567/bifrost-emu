#!/usr/bin/env bash
# Guest SDL public-struct conversion, including copied formats and palettes.
set -euo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
make -C "$repo_dir" cross SRC="$repo_dir/ctest_real/test_sdl_surface_format.c" OUT="$tmp_dir/guest.elf"
for mode in jit interp; do
    flags=()
    [[ "$mode" == interp ]] && flags+=(--no-jit)
    env -u BIFROST_ROOT -u BIFROST_NO_THUNK_GRAPHICS \
        BIFROST_NO_THUNK_AUDIO=1 BIFROST_NO_THUNK_DISPLAY=1 \
        timeout 30 "$emu" "${flags[@]}" "$tmp_dir/guest.elf"
done
