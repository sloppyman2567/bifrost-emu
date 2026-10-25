#!/usr/bin/env bash
# A fork child that execs drops the parent's memory instead of copying it: while it sits in the new program
# (Tests/Unit/test_fork_exec_memory.c) its private memory must be far below the parent's 256 MiB.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/test_fork_exec_memory.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
[[ -f "$fixture" ]] || { echo "Missing $(basename "$fixture"); build with make setup-tests" >&2; exit 1; }
limit_kib=$((96 * 1024))
log=$(mktemp /tmp/bifrost-fork-exec-memory.XXXXXX)
pid=
cleanup() { [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true; rm -f -- "$log"; }
trap cleanup EXIT
env -u BIFROST_ROOT timeout -s KILL 60 "$emu" "$fixture" > "$log" 2>&1 &
pid=$!
child=
for _ in $(seq 1 300); do
    if grep -q '^EXECED$' "$log" && grep -q '^CHILD [0-9]' "$log"; then
        child=$(sed -n 's/^CHILD \([0-9]*\)$/\1/p' "$log" | head -1)
        break
    fi
    sleep 0.1
done
[[ -n "$child" ]] || { echo "the child never reached the exec'd program"; cat "$log"; exit 1; }
private_kib=$(awk '/^Private_Dirty:/ {sum += $2} END {print sum + 0}' "/proc/$child/smaps_rollup")
wait "$pid"
pid=
grep -q 'test_fork_exec_memory: child exited' "$log" || { cat "$log"; exit 1; }
if (( private_kib >= limit_kib )); then
    echo "the exec'd child holds $((private_kib / 1024)) MiB of private memory (limit $((limit_kib / 1024)) MiB)"
    exit 1
fi
echo "fork_exec_memory: ALL PASS (exec'd child private memory $((private_kib / 1024)) MiB)"
