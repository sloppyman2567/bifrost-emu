#!/usr/bin/env bash
# Register passing between chained blocks (Source/FrostJIT/Compiler/RegisterPassing.cpp). jit_reg_pass.elf runs hand-written block chains,
# a diamond, register moves across edges, a call in the middle of a chain, five-register merges and a faulting loop against C, then repeats
# them on four threads. It runs with BIFROST_CARRY=1 (the feature on), with BIFROST_NO_REG_PASS=1, with chaining off, in the ordinary frame
# layout and with IR validation; with the feature on, the -v statistics must show chain edges into a register entry (otherwise the test
# proves nothing).
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/jit_reg_pass.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done
fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }
run() { env -u BIFROST_ROOT -u BIFROST_CARRY -u BIFROST_NO_REG_PASS -u BIFROST_NO_CHAIN -u BIFROST_NO_CHAIN_SKIP "$@" timeout -s KILL 120 "$EMU" -v "$FIXTURE" 2>&1 || true; }
passed() { sed -n 's/.*into the register entry=\([0-9]*\).*/\1/p' <<<"$1" | tail -1; }

out=$(run BIFROST_CARRY=1); check "register passing on: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
n=$(passed "$out")
check "register passing on: at least twenty chain edges enter a register entry" 1 "$(awk -v n="${n:-0}" 'BEGIN { print (n >= 20) }')"
out=$(run BIFROST_CARRY=1 BIFROST_NO_REG_PASS=1); check "register passing off: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
check "register passing off: no edge enters a register entry" 0 "$(passed "$out" | grep -c . || true)"
out=$(run); check "Carry off: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_CARRY=1 BIFROST_NO_CHAIN=1); check "no chaining: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_CARRY=1 BIFROST_NO_CHAIN_SKIP=1); check "ordinary frames: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_CARRY=1 BIFROST_IR_VALIDATE=1); check "IR validation on: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"
out=$(run BIFROST_CARRY=1 BIFROST_NO_SIDE_EXIT=1); check "mid-block exits off: the fixture passes" 1 "$(grep -c 'jit_reg_pass: ALL PASS' <<<"$out" || true)"

[[ $fail -eq 0 ]] && echo "jit_reg_pass: ALL PASS"
exit $fail
