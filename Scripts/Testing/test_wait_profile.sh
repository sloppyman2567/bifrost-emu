#!/usr/bin/env bash
# BIFROST_PROF's waiting report charges each guest thread's time to the kind of call it spent it in: a sleeping,
# futex-waiting, polling and pipe-reading thread to sleep, futex, poll and io, a computing one to cpu. Run with the
# JIT and with --no-jit; without BIFROST_PROF nothing is printed.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/test_wait_profile.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

fail=0
# percent NAME COLUMN: the number printed for COLUMN ("cpu", "futex", ...) on the line of the thread called NAME
percent() { awk -v name="\"$1\"" -v col="$2" '
    index($0, name) { for (i = 1; i <= NF; i++) if ($i == col || $i == col "%") { v = $(i + 1); if ($i ~ /%$/) v = $i; gsub(/%/, "", v); print v; exit } }' <<<"$report"; }
want() { # description, thread, column: the column is at least half of the thread's lifetime
    local got; got=$(percent "$2" "$3")
    if [[ -n $got ]] && awk -v v="$got" 'BEGIN { exit !(v >= 50) }'; then echo "PASS $mode $1 ($got%)"
    else echo "FAIL $mode $1 (got '${got:-nothing}')"; fail=1; fi
}

for mode in "jit" "no-jit"; do
    args=(-v); [[ $mode = no-jit ]] && args=(--no-jit -v)
    report=$(BIFROST_PROF=1 timeout 60 "$EMU" "${args[@]}" "$FIXTURE" 2>&1 | grep -E '\] waiting,|\]   tid |ALL PASS|FAIL' || true)
    grep -q 'test_wait_profile: ALL PASS' <<<"$report" || { echo "FAIL $mode fixture did not pass"; fail=1; }
    grep -q '\] waiting, ' <<<"$report" || { echo "FAIL $mode no waiting summary line"; fail=1; }
    want "a sleeping thread's time is sleep" w-sleep sleep
    want "a futex-waiting thread's time is futex" w-futex futex
    want "a polling thread's time is poll" w-poll poll
    want "a thread blocked on a pipe is inside io" w-io io
    want "a computing thread is on the cpu" w-spin cpu
    # CPU time is counted from the thread's first syscall, like its lifetime: startup work before it must not push a share past 100%.
    over=$(awk '/\]   tid / { for (i = 1; i <= NF; i++) if ($i == "cpu") { v = $(i + 1); gsub(/[%,|]/, "", v); if (v + 0 > 105) print $0 } }' <<<"$report")
    if [[ -z $over ]]; then echo "PASS $mode no thread has a cpu share past 100%"; else echo "FAIL $mode cpu share past 100%: $over"; fail=1; fi
done

if [[ $(timeout 60 "$EMU" -v "$FIXTURE" 2>&1 | grep -c '\] waiting') -ne 0 ]]; then
    echo "FAIL the report is printed without BIFROST_PROF"; fail=1
else echo "PASS no report without BIFROST_PROF"; fi

[[ $fail -eq 0 ]] && echo "wait_profile: ALL PASS"
exit $fail
