#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-android-loader.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
make -j6 USE_SDL2=0 USE_THUNK_GL=0 lib > "$task_tmp/build.log" 2>&1 || {
    cat "$task_tmp/build.log" >&2; exit 1;
}
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource Tests/Unit/android_loader_host.cpp \
    build/release-sdl0-gl0/libbifrost.a -lffi -o "$task_tmp/test"
env -u BIFROST_ROOT BIFROST_NO_ASLR=1 timeout -s KILL 30 "$task_tmp/test"
env -u BIFROST_ROOT -u BIFROST_NO_ASLR timeout -s KILL 30 "$task_tmp/test"
