#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-mt-publication.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource Tests/Unit/jit_mt_publication.cpp \
    "$HOST_LIB_DIR"/libbifrost.a $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/test"
for layout in default ordinary; do
    args=()
    [[ "$layout" == ordinary ]] && args+=(BIFROST_NO_CHAIN_SKIP=1)
    env -u BIFROST_ROOT -u BIFROST_TIER2 -u BIFROST_NO_CHAIN -u BIFROST_CHAIN_SKIP -u BIFROST_NO_CHAIN_SKIP \
        BIFROST_TIER2=1 BIFROST_TIER2_HITS=1000 "${args[@]}" timeout -s KILL 30 "$task_tmp/test"
done
