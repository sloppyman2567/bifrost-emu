#!/usr/bin/env bash
# BIFROST_PROF prints a `[stall]` line for a guest call that takes a second or more on a busy thread, naming the thread, the
# call, its arguments (a futex's address) and its caller; a thread that is not busy is not reported, and nothing prints
# without BIFROST_PROF or for calls under a second. Run with the JIT and with --no-jit.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
STALL="$REPO_ROOT/Tests/Unit/test_wait_stall.elf"
SHORT="$REPO_ROOT/Tests/Unit/test_wait_profile.elf"
for need in "$EMU" "$STALL" "$SHORT"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }

for mode in jit no-jit; do
    args=(); [[ $mode = no-jit ]] && args=(--no-jit)
    out=$(BIFROST_PROF=1 timeout 60 "$EMU" "${args[@]}" "$STALL" 2>&1 || true)
    busy=$(sed -n 's/^futex words: busy=\(0x[0-9a-f]*\) .*/\1/p' <<<"$out")
    check "$mode: the fixture passes" 1 "$(grep -c 'test_wait_stall: ALL PASS' <<<"$out")"
    line=$(grep '\[stall\].*"st-futex"' <<<"$out" || true)
    check "$mode: the busy futex waiter is reported once" 1 "$(grep -c . <<<"$line")"
    check "$mode: it names the futex call" 1 "$(grep -c 'futex took' <<<"$line")"
    check "$mode: it gives the futex address" 1 "$(grep -c "a0=$busy " <<<"$line")"
    check "$mode: the wait is a second or more" 1 "$(awk '{ for (i = 1; i <= NF; i++) if ($i == "took") { print ($(i+1) >= 1.0) ? 1 : 0; exit } }' <<<"$line")"
    check "$mode: it names a caller" 0 "$(grep -c 'caller=? *$' <<<"$line")"
    check "$mode: it carries the time of day" 1 "$(grep -c -E '\[stall\] [0-9]{2}-[0-9]{2} [0-9:]{8}\.[0-9]{3} ' <<<"$line")"
    check "$mode: the busy sleeper is reported as nanosleep" 1 "$(grep '\[stall\].*"st-sleep"' <<<"$out" | grep -c 'nanosleep took')"
    check "$mode: the idle thread is not reported" 0 "$(grep -c '\[stall\].*"st-idle"' <<<"$out")"
    check "$mode: the idle main thread is not reported" 0 "$(grep -c '\[stall\].*tid 1 ' <<<"$out")"
done

check "no [stall] line without BIFROST_PROF" 0 "$(timeout 60 "$EMU" "$STALL" 2>&1 | grep -c '\[stall\]' || true)"
check "no [stall] line for calls under a second" 0 "$(BIFROST_PROF=1 timeout 60 "$EMU" "$SHORT" 2>&1 | grep -c '\[stall\]' || true)"

[[ $fail -eq 0 ]] && echo "wait_stall: ALL PASS"
exit $fail
