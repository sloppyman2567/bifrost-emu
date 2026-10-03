#!/usr/bin/env bash
# Legacy GL ABI/buffer oracles and the OpenAL OSS device contract, both engines.
set -euo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
for test in doom3_gl oss_audio; do
 make -C "$repo_dir" cross SRC="$repo_dir/ctest_real/test_$test.c" OUT="$tmp_dir/$test.elf"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -shared -fPIC "$repo_dir/ctest_real/doom3_gl_host_probe.c" -o "$tmp_dir/gl_probe.so"
for engine in jit interp; do
 options=()
 [[ "$engine" == interp ]] && options+=(--no-jit)
 env -u BIFROST_ROOT -u BIFROST_NO_THUNK_GRAPHICS \
     BIFROST_NO_THUNK_AUDIO=1 BIFROST_NO_THUNK_DISPLAY=1 \
     LD_PRELOAD="$tmp_dir/gl_probe.so${LD_PRELOAD:+:$LD_PRELOAD}" \
     timeout 45 "$emu" "${options[@]}" "$tmp_dir/doom3_gl.elf"
 env -u BIFROST_ROOT -u BIFROST_NO_THUNK_AUDIO SDL_AUDIODRIVER=dummy \
     timeout 45 "$emu" "${options[@]}" "$tmp_dir/oss_audio.elf"
 echo "Doom 3 compatibility: $engine passed"
done
