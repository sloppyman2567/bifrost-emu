#!/usr/bin/env bash
# The RAM a guest sees: the host's real pressure under a capped total (default 8 GiB,
# BIFROST_GUEST_MEMORY_MB), the same through /proc/meminfo and sysinfo.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/test_guest_meminfo.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
if [[ ! -f "$fixture" ]]; then echo "Missing $(basename "$fixture"); build with make cross" >&2; exit 1; fi
work=$(mktemp -d /tmp/bifrost-guest-memory-test.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
g++ -O2 -std=c++17 -I"$repo/Source" "$repo/Tests/Host/guest_memory.cpp" "$repo/Source/System/Core/GuestMemoryView.cpp" -o "$work/policy"
"$work/policy"
host_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
min_kb() { echo $(( $1 < $2 ? $1 : $2 )); }
run() {  # run EXPECTED_KB [ENV...]
    local expected=$1; shift
    env -u BIFROST_ROOT -u BIFROST_GUEST_MEMORY_MB "$@" timeout -s KILL 30 "$emu" "$fixture" "$expected" | tee "$work/out"
    grep -q 'test_guest_meminfo: ALL PASS' "$work/out"
}
run "$(min_kb "$host_kb" 8388608)"
run "$(min_kb "$host_kb" 2097152)" BIFROST_GUEST_MEMORY_MB=2048
run "$host_kb" BIFROST_GUEST_MEMORY_MB=0
echo "guest_memory: ALL PASS"
