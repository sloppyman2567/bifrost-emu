#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-memory-high-gap.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource \
    Tests/Unit/memory_high_gap.cpp "$HOST_LIB_DIR"/libbifrost.a \
    $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/memory_high_gap"
"$task_tmp/memory_high_gap"
