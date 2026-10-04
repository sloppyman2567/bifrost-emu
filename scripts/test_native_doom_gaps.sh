#!/usr/bin/env bash
# Exhaustive captured-opcode checks against actual Tier-1 code generation.
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=${BIFROST_BUILD_DIR:-build/release-sdl1-gl1}
if [[ ! -f "$build_dir/src/jit/native_simd.o" ]]; then
    echo "Build the SDL/GL release configuration first (make USE_SDL2=1 USE_THUNK_GL=1)." >&2
    exit 1
fi
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
mapfile -d '' objects < <(find "$build_dir/src" -name '*.o' -print0)
"${CXX:-g++}" -std=c++17 -O2 -pthread -Iinclude -Isrc -I/usr/include/SDL2 \
    -DBIFROST_USE_SDL2 -DBIFROST_THUNK_HAVE_SDL2 \
    -DBIFROST_THUNK_HAVE_GL -DBIFROST_THUNK_HAVE_EGL \
    ctest/native_doom_gaps.cpp "${objects[@]}" -lffi -lSDL2 -lGL -lEGL \
    -o "$task_tmp/native_doom_gaps"
BIFROST_IR_VALIDATE=1 "$task_tmp/native_doom_gaps"
