#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
emu=${BIFROST_EMU:-./bifrost-emu}
fixture=Tests/Unit/test_pthread_mutex.elf
[[ -f $fixture && ! Tests/Unit/test_pthread_mutex.c -nt $fixture ]] || {
    echo 'Build a fresh test_pthread_mutex.elf first' >&2; exit 1;
}
log=$(mktemp /tmp/bifrost-tier2-mutex.XXXXXX)
trap 'rm -f -- "$log"' EXIT
for ((i=0; i<20; ++i)); do
    env -u BIFROST_ROOT -u BIFROST_JIT_VERIFY -u BIFROST_NO_FWD \
        BIFROST_TIER2=1 BIFROST_TIER2_HITS=32 BIFROST_IR_VALIDATE=1 \
        timeout -s KILL 15 "$emu" "$fixture" > "$log" 2>&1 || {
            cat "$log" >&2; exit 1;
        }
    rg -q 'counter = 8000 .* OK' "$log" && rg -q 'test_pthread_mutex: ALL PASS' "$log" || {
        cat "$log" >&2; exit 1;
    }
done
echo 'jit_tier2_mutex: 20 promotion-stress runs PASS'
