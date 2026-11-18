#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-emitter-slow-paths.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
# -fno-access-control: the test calls FrostJIT's private emitters directly.
"${CXX:-g++}" -O1 -std=c++17 -pthread -fno-access-control -IInclude -ISource Tests/Unit/jit_emitter_slow_paths.cpp \
    "$HOST_LIB_DIR"/libbifrost.a $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/test"
# BIFROST_NO_WEX: the test runs code it emitted into the buffer without toggling write/execute.
env -u BIFROST_ROOT BIFROST_NO_WEX=1 timeout -s KILL 60 "$task_tmp/test"
