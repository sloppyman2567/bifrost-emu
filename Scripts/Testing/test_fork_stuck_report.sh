#!/usr/bin/env bash
# BIFROST_FORK_STUCK_SECONDS: a fork child still not through an execve reports where it is blocked; one that
# execs, or a run without the variable, stays silent.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/test_fork_stuck_report.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
[[ -f "$fixture" ]] || { echo "Missing $(basename "$fixture"); build with make setup-tests" >&2; exit 1; }
logs=$(mktemp -d /tmp/bifrost-fork-stuck.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT
run() { env -u BIFROST_ROOT "$@" timeout -s KILL 60 "$emu" "$fixture" "${mode}" > "$logs/$name.log" 2>&1; }
mode=stuck name=reported run BIFROST_FORK_STUCK_SECONDS=1
mode=stuck name=off run BIFROST_FORK_STUCK_SECONDS=0
mode=exec name=exec run BIFROST_FORK_STUCK_SECONDS=1
rg -q '^\[fork-stuck\] pid [0-9]+ has not finished an execve after 1 s: guest pc 0x[0-9a-f]+ \(.*test_fork_stuck_report\.elf\+0x[0-9a-f]+\)' \
    "$logs/reported.log" || { cat "$logs/reported.log"; exit 1; }
rg -q '^\[fork-stuck\] host stack of the forking thread:' "$logs/reported.log" || { cat "$logs/reported.log"; exit 1; }
[[ $(rg -c 'bifrost-emu\(' "$logs/reported.log") -ge 3 ]] || { cat "$logs/reported.log"; exit 1; }
for name in off exec; do
    if rg -q 'fork-stuck' "$logs/$name.log"; then cat "$logs/$name.log"; exit 1; fi
    rg -q 'parent done' "$logs/$name.log" || { cat "$logs/$name.log"; exit 1; }
done
echo "fork_stuck_report: ALL PASS"
