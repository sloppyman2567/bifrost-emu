#!/usr/bin/env bash
# Carry follows the hot side of a mid-block exit (Source/FrostJIT/Carry/HotSide.cpp). jit_carry_follow.elf runs loops with a forward cbnz, tbnz
# or b.ne whose direction comes from a pattern (always taken, never, only for bit 1, alternating, mostly), in phases so that a branch is flipped and
# flipped back, against C, then repeats them on four threads. It runs with BIFROST_CARRY=1, with BIFROST_NO_CARRY_FOLLOW=1, with Carry off, with
# chaining off, in the ordinary frame layout and with IR validation; with the feature on, the -v statistics must show flipped branches (otherwise
# the test proves nothing), and with it off none.
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/jit_carry_follow.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done
fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }
run() { env -u BIFROST_ROOT -u BIFROST_CARRY -u BIFROST_NO_CARRY_FOLLOW -u BIFROST_NO_CHAIN -u BIFROST_NO_CHAIN_SKIP "$@" timeout -s KILL 120 "$EMU" -v "$FIXTURE" 2>&1 || true; }
flipped() { sed -n 's/.*branches flipped=\([0-9]*\).*/\1/p' <<<"$1" | tail -1; }
passes() { grep -c 'jit_carry_follow: ALL PASS' <<<"$1" || true; }

out=$(run BIFROST_CARRY=1); check "following on: the fixture passes" 1 "$(passes "$out")"
check "following on: at least one branch is flipped" 1 "$(awk -v n="$(flipped "$out")" 'BEGIN { print (n+0 >= 1) }')"
out=$(run BIFROST_CARRY=1 BIFROST_NO_CARRY_FOLLOW=1); check "following off: the fixture passes" 1 "$(passes "$out")"
check "following off: no profile, no flip" 0 "$(flipped "$out" | grep -c . || true)"
out=$(run); check "Carry off: the fixture passes" 1 "$(passes "$out")"
out=$(run BIFROST_CARRY=1 BIFROST_NO_CHAIN=1); check "no chaining: the fixture passes" 1 "$(passes "$out")"
out=$(run BIFROST_CARRY=1 BIFROST_NO_CHAIN_SKIP=1); check "ordinary frames: the fixture passes" 1 "$(passes "$out")"
out=$(run BIFROST_CARRY=1 BIFROST_IR_VALIDATE=1); check "IR validation on: the fixture passes" 1 "$(passes "$out")"
out=$(run BIFROST_CARRY=1 BIFROST_NO_REG_PASS=1); check "register passing off: the fixture passes" 1 "$(passes "$out")"

[[ $fail -eq 0 ]] && echo "jit_carry_follow: ALL PASS"
exit $fail
