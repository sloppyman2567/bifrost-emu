#!/usr/bin/env bash
# Repeated SDL window, guest mapping, thread, and shutdown check.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
rounds=${1:-64}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

make -C "$repo_dir" cross \
  SRC="$repo_dir/ctest_real/test_thunk_soak.c" \
  OUT="$tmp_dir/test_thunk_soak.elf" CROSS_EXTRA=-pthread
env -u BIFROST_NO_THUNK_GRAPHICS \
  BIFROST_NO_THUNK_AUDIO=1 BIFROST_NO_THUNK_DISPLAY=1 \
  SDL_VIDEODRIVER=dummy \
  timeout 120 "$emu" "$tmp_dir/test_thunk_soak.elf" "$rounds"
