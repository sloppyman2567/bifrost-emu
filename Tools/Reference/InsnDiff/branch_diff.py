#!/usr/bin/env python3
"""Generate branch_tests.S + branch_diff.c: taken/not-taken for every
conditional-branch form (b.cond x 16 NZCV, cbz/cbnz W/X, tbz/tbnz W/X all bits).

Each test: f(nzcv, x5) -> 1 if taken else 0. Run under QEMU and Bifrost; diff.
"""
conds = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]
tests = []
for c in conds:
    tests.append(f"b.{c} 1f")
for op in ("cbz", "cbnz"):
    for r in ("w5", "x5"):
        tests.append(f"{op} {r}, 1f")
for op in ("tbz", "tbnz"):
    for bit in range(64):
        r = "w5" if bit < 32 else "x5"
        tests.append(f"{op} {r}, #{bit}, 1f")
        if bit < 32:
            tests.append(f"{op} x5, #{bit}, 1f")  # assembler emits the same encoding (b5=0)
with open("branch_tests.S", "w") as s:
    s.write("    .text\n")
    for i, t in enumerate(tests):
        # x0 = nzcv, x1 = value placed in x5
        s.write(f"    .globl b{i}\nb{i}:\n    msr nzcv, x0\n    mov x5, x1\n    {t}\n"
                "    mov x0, #0\n    ret\n1:  mov x0, #1\n    ret\n")
    s.write("    .section .rodata\n    .p2align 3\n    .globl g_btests\ng_btests:\n")
    for i in range(len(tests)):
        s.write(f"    .quad b{i}\n")
with open("branch_table.h", "w") as h:
    h.write(f"#define NB {len(tests)}\nstatic const char* g_bnames[] = {{\n")
    for t in tests:
        h.write(f'  "{t}",\n')
    h.write("};\n")
print(len(tests), "branch tests")
