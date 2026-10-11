#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
source_file=Tests/Apps/test_android_surface.c
binary=Tests/Apps/test_android_surface.elf
if [[ ! -f "$binary" || "$source_file" -nt "$binary" ]]; then
    make cross SRC="$source_file" OUT="$binary"
fi
exec ./bifrost-emu "$@" "$binary" --demo-seconds 10
