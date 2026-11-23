#!/usr/bin/env bash
# BIFROST_PROF's report prints at exit without -v, and its guest-instruction count covers what chained blocks and
# self-loop back edges do. The count is compared with the interpreter's for the same program (it steps every
# instruction); the report itself is checked with the JIT and with --no-jit.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
COUNT="$REPO_ROOT/Tests/Unit/jit_stats_count.elf"
REPORT="$REPO_ROOT/Tests/Unit/test_wait_profile.elf"
for need in "$EMU" "$COUNT" "$REPORT"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }

# The exit report without -v: it is what BIFROST_PROF asks for, and run() reaches it for an exit_group too.
for mode in jit no-jit; do
    args=(); [[ $mode = no-jit ]] && args=(--no-jit)
    out=$(BIFROST_PROF=1 timeout 60 "$EMU" "${args[@]}" "$REPORT" 2>&1 || true)
    check "$mode: the waiting report prints at exit without -v" 1 "$(grep -c '\] waiting, ' <<<"$out" || true)"
    if [[ $mode = jit ]]; then
        check "jit: the JIT sample report prints at exit without -v" 1 "$(grep -c '\] JIT samples: ' <<<"$out" || true)"
    fi
done

out=$(timeout 60 "$EMU" "$REPORT" 2>&1 || true)
check "no report without BIFROST_PROF" 0 "$(grep -cE '\] JIT samples: |\] waiting, ' <<<"$out" || true)"

# The counters: the JIT and the interpreter must report the same guest instructions for the same program.
jit_out=$(timeout 120 "$EMU" -v "$COUNT" 2>&1 || true)
interp_out=$(timeout 120 "$EMU" --no-jit -v "$COUNT" 2>&1 || true)
check "the fixture passes under the JIT" 1 "$(grep -c 'done: acc=0x' <<<"$jit_out" || true)"
check "the fixture passes under the interpreter" 1 "$(grep -c 'done: acc=0x' <<<"$interp_out" || true)"
jit=$(grep -oE 'executed \([0-9]+ instructions' <<<"$jit_out" | grep -oE '[0-9]+' | head -1 || true)
interp=$(grep -oE 'executed [0-9]+ instructions' <<<"$interp_out" | grep -oE '[0-9]+' | head -1 || true)
if [[ -n "${jit:-}" && -n "${interp:-}" ]] && awk -v j="$jit" -v i="$interp" 'BEGIN { exit !(j >= 0.98 * i && j <= 1.02 * i) }'; then
    echo "PASS the JIT's guest-instruction count matches the interpreter's ($jit vs $interp)"
else
    echo "FAIL the JIT's guest-instruction count ('${jit:-nothing}') does not match the interpreter's ('${interp:-nothing}')"
    fail=1
fi

[[ $fail -eq 0 ]] && echo "jit_instruction_count: ALL PASS"
exit $fail
