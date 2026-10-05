#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source_file=ctest_real/test_android_surface.c
binary=ctest_real/test_android_surface.elf
if [[ ! -f "$binary" || "$source_file" -nt "$binary" ]]; then
    make cross SRC="$source_file" OUT="$binary"
fi
exec ./bifrost-emu "$@" "$binary" --demo-seconds 10
