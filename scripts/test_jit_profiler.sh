#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-jit-profiler.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
make USE_SDL2=1 USE_THUNK_GL=1 lib > "$task_tmp/build.log" 2>&1 || {
    cat "$task_tmp/build.log" >&2; exit 1;
}
"${CXX:-g++}" -O2 -std=c++17 -pthread -Iinclude -Isrc \
    ctest/jit_profiler.cpp build/release-sdl1-gl1/libbifrost.a \
    -lffi -lSDL2 -lGL -lEGL -o "$task_tmp/test_jit_profiler"
BIFROST_PROF=1 BIFROST_JIT_CACHE_MB=1024 \
    "$task_tmp/test_jit_profiler"
