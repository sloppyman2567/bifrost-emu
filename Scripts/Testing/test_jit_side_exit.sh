#!/usr/bin/env bash
# Mid-block conditional exits (Source/FrostJIT/Compiler/SideExit.cpp). jit_side_exit.elf checks every loop shape against C (CBZ, CBNZ, TBZ, TBNZ and
# all 14 B.cond conditions), a fault inside a loop, and the same loops on five threads. It runs with exits on and off, with chaining off, and in the ordinary frame layout; with exits
# on, the -v statistics must show that blocks with a mid-block exit were translated (otherwise the test proves nothing).
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/jit_side_exit.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done
fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }
run() { env -u BIFROST_ROOT -u BIFROST_NO_SIDE_EXIT -u BIFROST_NO_CHAIN -u BIFROST_NO_CHAIN_SKIP "$@" timeout -s KILL 120 "$EMU" -v "$FIXTURE" 2>&1 || true; }

out=$(run); check "exits on: the fixture passes" 1 "$(grep -c 'jit_side_exit: ALL PASS' <<<"$out" || true)"
n=$(sed -n 's/.*side-exit blocks: \([0-9]*\).*/\1/p' <<<"$out" | tail -1)
check "exits on: at least forty blocks have a mid-block exit" 1 "$(awk -v n="${n:-0}" 'BEGIN { print (n >= 40) }')"
out=$(run BIFROST_NO_SIDE_EXIT=1); check "exits off: the fixture passes" 1 "$(grep -c 'jit_side_exit: ALL PASS' <<<"$out" || true)"
check "exits off: no block has a mid-block exit" 0 "$(sed -n 's/.*side-exit blocks: \([0-9]*\).*/\1/p' <<<"$out" | tail -1)"
out=$(run BIFROST_NO_CHAIN=1); check "no chaining: the fixture passes" 1 "$(grep -c 'jit_side_exit: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_NO_CHAIN_SKIP=1); check "ordinary frames: the fixture passes" 1 "$(grep -c 'jit_side_exit: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_IR_VALIDATE=1); check "IR validation on: the fixture passes" 1 "$(grep -c 'jit_side_exit: ALL PASS' <<<"$out" || true)"

[[ $fail -eq 0 ]] && echo "jit_side_exit: ALL PASS"
exit $fail
