#!/usr/bin/env bash
# The Carry loop report (BIFROST_PROF with BIFROST_PC_HIST) finds a hot loop of several blocks and the guest registers it
# touches. carry_loop.elf is a hand-written four-block loop over five registers; it must come out as one loop of at least
# two blocks that holds most of the JIT samples and uses at most eight registers. Without BIFROST_PC_HIST nothing prints.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/carry_loop.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }

# The loop is checked as Lean splits it: Carry would merge the diamond into fewer blocks and fewer registers.
out=$(env BIFROST_CARRY=0 BIFROST_PROF=1 BIFROST_PC_HIST=1 timeout 120 "$EMU" "$FIXTURE" 2>&1 || true)
check "the fixture passes" 1 "$(grep -c 'carry_loop: ALL PASS' <<<"$out" || true)"
line=$(grep -m1 '\[carry\] loop head=' <<<"$out" || true)
if [[ -z $line ]]; then
    echo "FAIL no loop line in the report"; fail=1
else
    blocks=$(sed -n 's/.* blocks=\([0-9]*\) .*/\1/p' <<<"$line")
    share=$(sed -n 's/.* samples=\([0-9]*\)\..*%.*/\1/p' <<<"$line")
    registers=$(sed -n 's/.* registers=\([0-9]*\) .*/\1/p' <<<"$line")
    check "the loop spans at least two blocks" 1 "$(awk -v b="${blocks:-0}" 'BEGIN { print (b >= 2) }')"
    check "the loop holds most of the JIT samples" 1 "$(awk -v s="${share:-0}" 'BEGIN { print (s >= 50) }')"
    check "the loop touches 5 to 8 guest registers" 1 "$(awk -v r="${registers:-0}" 'BEGIN { print (r >= 5 && r <= 8) }')"
fi

out=$(BIFROST_PROF=1 timeout 120 "$EMU" "$FIXTURE" 2>&1 || true)
check "no loop report without BIFROST_PC_HIST" 0 "$(grep -c '\[carry\]' <<<"$out" || true)"

[[ $fail -eq 0 ]] && echo "carry_report: ALL PASS"
exit $fail
