#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-memory-high-gap.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
make USE_SDL2=1 USE_THUNK_GL=1 lib > "$task_tmp/build.log" 2>&1 || {
    cat "$task_tmp/build.log" >&2; exit 1;
}
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource \
    Tests/Unit/memory_high_gap.cpp build/release-sdl1-gl1/libbifrost.a \
    $(sed -n "s/^LDFLAGS=//p" build/release-sdl1-gl1/.build-config) -o "$task_tmp/memory_high_gap"
"$task_tmp/memory_high_gap"
