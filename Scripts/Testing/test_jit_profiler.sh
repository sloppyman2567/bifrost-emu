#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-jit-profiler.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource \
    Tests/Unit/jit_profiler.cpp "$HOST_LIB_DIR"/libbifrost.a \
    $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/test_jit_profiler"
BIFROST_PROF=1 BIFROST_JIT_CACHE_MB=1024 \
    BIFROST_REGALLOC_PROFILE="$task_tmp/allocation.tsv" \
    "$task_tmp/test_jit_profiler"
