#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-tier2-rate.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource Tests/Unit/jit_tier2_rate.cpp \
    "$HOST_LIB_DIR"/libbifrost.a $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/test"
# A fixed BIFROST_TIER2_HITS skips the gate this test is for.
env -u BIFROST_ROOT -u BIFROST_TIER2_HITS BIFROST_TIER2=1 timeout -s KILL 60 "$task_tmp/test"
