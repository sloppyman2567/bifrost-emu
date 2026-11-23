#!/usr/bin/env bash
# BIFROST_PROF prints a `[stall]` line for a guest call that takes a second or more on a busy thread, naming the thread, the
# call, its arguments (a futex's address), what its descriptor is (a pipe, a socket) and its caller; a thread that is not busy is
# not reported, and nothing prints without BIFROST_PROF or for calls under a second. A call site that stalls again and again
# prints five lines and is counted in the report. Run with the JIT and with --no-jit.
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
    pipe_fd=$(sed -n 's/^descriptors: pipe=\([0-9]*\) .*/\1/p' <<<"$out")
    sock_fd=$(sed -n 's/^descriptors: .* socket=\([0-9]*\)$/\1/p' <<<"$out")
    pipeline=$(grep '\[stall\].*"st-pipe"' <<<"$out" || true)
    sockline=$(grep '\[stall\].*"st-sock"' <<<"$out" || true)
    check "$mode: a blocked pipe read is reported as read" 1 "$(grep -c 'read took' <<<"$pipeline")"
    check "$mode: it says the descriptor is a pipe" 1 "$(grep -c " fd=$pipe_fd pipe:\[[0-9]*\] caller=" <<<"$pipeline")"
    check "$mode: a blocked socket read is reported as read" 1 "$(grep -c 'read took' <<<"$sockline")"
    check "$mode: it says the descriptor is a unix stream socket" 1 "$(grep -c " fd=$sock_fd unix stream .*caller=" <<<"$sockline")"
    check "$mode: a futex line has no descriptor" 0 "$(grep -c ' fd=' <<<"$line")"
    loops=$(grep '\[stall\].*"st-loop"' <<<"$out" || true)
    check "$mode: a call site prints five stalls of seven" 5 "$(grep -c . <<<"$loops")"
    check "$mode: only the fifth says it is the last line" 1 "$(grep -c 'last line for this call site' <<<"$loops")"
    check "$mode: the report counts all seven" 1 "$(grep -c 'repeated stalls: tid [0-9]* "st-loop" nanosleep x7, ' <<<"$out")"
    check "$mode: the other threads are not in the repeated list" 0 "$(grep 'repeated stalls' <<<"$out" | grep -c -v '"st-loop"')"
    check "$mode: the idle thread is not reported" 0 "$(grep -c '\[stall\].*"st-idle"' <<<"$out")"
    check "$mode: the idle main thread is not reported" 0 "$(grep -c '\[stall\].*tid 1 ' <<<"$out")"
done

check "no [stall] line without BIFROST_PROF" 0 "$(timeout 60 "$EMU" "$STALL" 2>&1 | grep -c '\[stall\]' || true)"
check "no [stall] line for calls under a second" 0 "$(BIFROST_PROF=1 timeout 60 "$EMU" "$SHORT" 2>&1 | grep -c '\[stall\]' || true)"

[[ $fail -eq 0 ]] && echo "wait_stall: ALL PASS"
exit $fail
