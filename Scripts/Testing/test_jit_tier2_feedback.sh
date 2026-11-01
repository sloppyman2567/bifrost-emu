#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
task_tmp=$(mktemp -d /tmp/bifrost-tier2-feedback.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
make USE_SDL2=0 USE_THUNK_GL=0 lib > "$task_tmp/build.log" 2>&1 || {
    cat "$task_tmp/build.log" >&2; exit 1;
}
"${CXX:-g++}" -O2 -std=c++17 -pthread -IInclude -ISource Tests/Unit/jit_tier2_feedback.cpp \
    build/release-sdl0-gl0/libbifrost.a $(sed -n "s/^LDFLAGS=//p" build/release-sdl0-gl0/.build-config) -o "$task_tmp/test"
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
