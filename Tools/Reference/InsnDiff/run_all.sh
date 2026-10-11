#!/bin/bash
# Run the single-instruction diff under QEMU and both Bifrost backends.
# Usage: run_all.sh <bifrost-emu> [high]
T="$(dirname "$0")"
EMU="$1"; MODE="${2:-}"; S="${MODE:-low}"
timeout -s KILL 300 qemu-aarch64 -cpu max "$T/insn_diff.elf" $MODE > "$T/qemu_$S.out" || echo "qemu rc=$?"
timeout -s KILL 300 "$EMU" --no-jit "$T/insn_diff.elf" $MODE > "$T/interp_$S.out" 2>/dev/null || echo "interp rc=$?"
timeout -s KILL 300 "$EMU" "$T/insn_diff.elf" $MODE > "$T/jit_$S.out" 2>/dev/null || echo "jit rc=$?"
python3 -I - "$T" "$S" <<'EOF'
import sys
d, s = sys.argv[1] + "/", sys.argv[2]
rows = [l.strip() for l in open(d + "insn_table.h") if l.startswith("  {")]
q = open(d + f"qemu_{s}.out").read().split("\n")
for backend in ("interp", "jit"):
    b = open(d + f"{backend}_{s}.out").read().split("\n")
    diff = {}
    for x, y in zip(q, b):
        if x and x != y:
            diff.setdefault(int(x.split()[0]), set()).add(int(x.split()[1]))
    # Noise: lone store-exclusives (monitor state), data words disassembled
    # as code, and atomics/acquire forms that differ only for the
    # deliberately misaligned vector 2 (QEMU alignment-faults them).
    atomic = ("ldar", "stlr", "ldx", "ldax", "stx", "stlx", "cas", "swp", "ldadd", "ldclr",
              "ldeor", "ldset", "ldsmax", "ldumax", "ldsmin", "ldumin", "ldapr")
    def noise(t):
        text = rows[t].split('"')[1]
        if text.startswith(".inst") or "undefined" in text or " z" in text or "stg " in text:
            return True
        return text.startswith(atomic) and diff[t] <= {2}
    bad = [t for t in sorted(diff) if not noise(t)]
    if len(b) < len(q):
        last = b[-2].split()[0] if len(b) > 1 else "?"
        print(f"  {backend}: output stopped after test {last}")
    print(f"{backend}/{s}: {len(b)}/{len(q)} lines, {len(bad)} mismatching (excl. lone stxr)")
    for t in bad[:40]:
        print("  ", t, rows[t])
EOF
