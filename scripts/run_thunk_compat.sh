#!/usr/bin/env bash
# Deterministic guest/host ABI regressions. Requires a build with SDL2/GL.
set -euo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
cross_cc=${CROSS_CC:-"$repo_dir/tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc"}
make -C "$repo_dir" cross CROSS_CC="$cross_cc" \
    SRC="$repo_dir/ctest_real/test_thunk_compat.c" OUT="$tmp_dir/guest.elf"
"${CC:-cc}" -std=c11 -Wall -Wextra -shared -fPIC -DTHUNK_GL_PROBE \
    "$repo_dir/ctest_real/thunk_compat_host_probe.c" -o "$tmp_dir/gl_probe.so"
"${CC:-cc}" -std=c11 -Wall -Wextra -shared -fPIC -DTHUNK_VK_PROBE \
    "$repo_dir/ctest_real/thunk_compat_host_probe.c" -o "$tmp_dir/libvulkan.so.1"
for engine in jit interp; do
    options=()
    if [[ "$engine" == interp ]]; then options+=(--no-jit); fi
    env -u BIFROST_NO_THUNK_GRAPHICS BIFROST_NO_THUNK_AUDIO=1 BIFROST_NO_THUNK_DISPLAY=1 \
        SDL_VIDEODRIVER=dummy timeout 30 "$emu" "${options[@]}" "$tmp_dir/guest.elf" sdl
    env -u BIFROST_NO_THUNK_GRAPHICS BIFROST_NO_THUNK_AUDIO=1 BIFROST_NO_THUNK_DISPLAY=1 \
        LD_PRELOAD="$tmp_dir/gl_probe.so${LD_PRELOAD:+:$LD_PRELOAD}" \
        timeout 30 "$emu" "${options[@]}" "$tmp_dir/guest.elf" gl
    env -u BIFROST_NO_THUNK_GRAPHICS -u BIFROST_NO_THUNK_DISPLAY BIFROST_NO_THUNK_AUDIO=1 \
        SDL_VIDEODRIVER=dummy LD_LIBRARY_PATH="$tmp_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        timeout 30 "$emu" "${options[@]}" "$tmp_dir/guest.elf" vk
    echo "thunk compatibility: $engine passed"
done
