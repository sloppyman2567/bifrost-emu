#!/usr/bin/env bash
# AAudio startup/continuation with ordinary and delayed host wait delivery.
set -euo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
tmp_dir=$(mktemp -d /tmp/bifrost-audio-callbacks.XXXXXX)
trap 'rm -rf -- "$tmp_dir"' EXIT
make -C "$repo_dir" cross SRC="$repo_dir/ctest_real/test_android_audio.c" OUT="$tmp_dir/guest.elf"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -shared -fPIC -O2 \
    "$repo_dir/ctest/host/audio_wait_delay.c" -ldl -pthread -o "$tmp_dir/wait-delay.so"
for mode in jit interp; do
    flags=()
    [[ "$mode" == interp ]] && flags+=(--no-jit)
    for delivery in normal delayed; do
        preload=()
        [[ "$delivery" == delayed ]] && preload+=("LD_PRELOAD=$tmp_dir/wait-delay.so")
        for run in {1..5}; do
            if ! env -u LD_PRELOAD -u BIFROST_ROOT -u BIFROST_NO_THUNK_AUDIO \
                SDL_AUDIODRIVER=dummy BIFROST_AUDIO_PUMP=1 "${preload[@]}" \
                timeout -s KILL 10 "$emu" "${flags[@]}" "$tmp_dir/guest.elf" \
                > "$tmp_dir/run.log" 2>&1; then
                cat "$tmp_dir/run.log" >&2
                echo "FAIL: AAudio $mode/$delivery run $run" >&2
                exit 1
            fi
            if ! grep -q '^ALL PASS$' "$tmp_dir/run.log"; then
                cat "$tmp_dir/run.log" >&2
                echo "FAIL: AAudio $mode/$delivery did not complete" >&2
                exit 1
            fi
        done
        echo "PASS: AAudio $mode/$delivery (5 runs)"
    done
done
