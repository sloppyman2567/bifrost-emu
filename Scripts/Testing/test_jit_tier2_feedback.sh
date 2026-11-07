#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-tier2-feedback.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
source Scripts/Testing/host_library.sh
build_host_library "$task_tmp/build.log"
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource Tests/Unit/jit_tier2_feedback.cpp \
    "$HOST_LIB_DIR"/libbifrost.a $(sed -n "s/^LDFLAGS=//p" "$HOST_LIB_DIR"/.build-config) -o "$task_tmp/test"
for mode in single mt; do
    args=()
    [[ $mode != mt ]] || args=(mt)
    for chain in ordinary skip; do
        settings=(BIFROST_NO_CHAIN_SKIP=1)
        [[ $chain != skip ]] || settings=(BIFROST_CHAIN_SKIP=1)
        env -u BIFROST_ROOT -u BIFROST_CHAIN_SKIP -u BIFROST_NO_CHAIN_SKIP -u BIFROST_NO_CHAIN -u BIFROST_JIT_BLOCK_LIMIT -u BIFROST_JIT_SUPERBLOCK_LIMIT \
            BIFROST_TIER2=1 BIFROST_TIER2_HITS=32 BIFROST_IR_VALIDATE=1 \
            BIFROST_REGALLOC_CHECK=1 "${settings[@]}" timeout -s KILL 30 "$task_tmp/test" "${args[@]}"
    done
done
