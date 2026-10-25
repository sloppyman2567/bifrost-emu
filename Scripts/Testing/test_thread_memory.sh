#!/usr/bin/env bash
# Resident memory per idle guest thread. The emulator's peak RSS (VmHWM) with 200 idle threads may exceed
# the one-thread run by at most 120 MB (600 KiB per thread); with a 1.7 MiB eager decode cache per vCPU it
# was about 350 MB. Measured from /proc on the host, so it needs no timing assumptions beyond the 3 s hold.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/test_thread_memory.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
if [[ ! -f "$fixture" ]]; then echo "Missing $(basename "$fixture"); build with make cross" >&2; exit 1; fi
work=$(mktemp -d /tmp/bifrost-thread-memory-test.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
peak_mb() {  # peak_mb THREADS: the emulator's VmHWM in MB
    env -u BIFROST_ROOT "$emu" "$fixture" "$1" > "$work/out" 2>&1 &
    local pid=$! hwm=0 v
    while kill -0 "$pid" 2>/dev/null; do
        v=$(awk '/VmHWM/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
        [[ -n "$v" ]] && hwm=$v
        sleep 0.05
    done
    wait "$pid"
    grep -q "$1 threads joined" "$work/out" || { cat "$work/out" >&2; exit 1; }
    echo $((hwm / 1024))
}
one=$(peak_mb 1)
many=$(peak_mb 200)
echo "peak RSS: $one MB with 1 thread, $many MB with 200 threads (+$((many - one)) MB)"
if (( many - one > 120 )); then echo "FAIL: idle threads cost $(( (many - one) * 1024 / 200 )) KiB each" >&2; exit 1; fi
echo "thread_memory: ALL PASS"
