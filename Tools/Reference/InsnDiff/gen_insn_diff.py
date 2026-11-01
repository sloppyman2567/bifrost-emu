#!/usr/bin/env python3
"""Generate a single-instruction differential test (QEMU vs Bifrost).

Input: objdump lines "hex<TAB>mnemonic<TAB>operands". Output: insn_tests.S
(one harness-wrapped test per unique encoding) and insn_table.h.
"""
import re
import sys

SKIP = re.compile(r"^(b|bl|br|blr|ret|b\..*|cbn?z|tbn?z|svc|brk|hlt|udf|mrs|msr|hint|bti|"
                  r"pac.*|aut.*|xpac.*|isb|wfe|wfi|yield|sev.*|clrex|eret|.*\.word|nop)$")
MEM = re.compile(r"^(ld|st|ca|sw|prf)")

seen, tests = set(), []
for line in open(sys.argv[1]):
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 2:
        continue
    word, mnem = parts[0].strip(), parts[1].strip()
    if word in seen or SKIP.match(mnem):
        continue
    seen.add(word)
    w = int(word, 16)
    text = (mnem + " " + (parts[2] if len(parts) > 2 else "")).strip()
    is_mem = bool(MEM.match(mnem))
    # Literal loads (ldr Xt, label) are PC-relative: no base register.
    literal = is_mem and "[" not in text
    tests.append((w, text, is_mem and not literal, (w >> 5) & 31, (w >> 16) & 31))

pre = """    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    adrp x2, g_out
    str x1, [x2, :lo12:g_out]
    mov x2, sp
    adrp x3, g_hostsp
    str x2, [x3, :lo12:g_hostsp]
    mrs x2, tpidr_el0
    adrp x3, g_tp
    str x2, [x3, :lo12:g_tp]
""" + "".join(f"    ldp q{i}, q{i+1}, [x0, #{272 + 16*i}]\n" for i in range(0, 32, 2)) + """    ldr x2, [x0, #256]
    msr nzcv, x2
    ldr x2, [x0, #248]
    mov sp, x2
""" + "".join(f"    ldp x{i}, x{i+1}, [x0, #{8*i}]\n" for i in range(1, 30, 2)) + """    ldr x30, [x0, #240]
    ldr x0, [x0]
"""
post = """    msr tpidr_el0, x0
    adrp x0, g_out
    ldr x0, [x0, :lo12:g_out]
""" + "".join(f"    stp x{i}, x{i+1}, [x0, #{8*i}]\n" for i in range(1, 30, 2)) + """    mrs x1, tpidr_el0
    str x1, [x0]
    mov x1, sp
    str x1, [x0, #248]
    mrs x1, nzcv
    str x1, [x0, #256]
""" + "".join(f"    stp q{i}, q{i+1}, [x0, #{272 + 16*i}]\n" for i in range(0, 32, 2)) + """    adrp x1, g_tp
    ldr x1, [x1, :lo12:g_tp]
    msr tpidr_el0, x1
    adrp x1, g_hostsp
    ldr x1, [x1, :lo12:g_hostsp]
    mov sp, x1
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x29, x30, [sp], #160
    ret
"""
with open("insn_tests.S", "w") as s:
    s.write("    .text\n    .p2align 2\n")
    for i, (w, text, *_rest) in enumerate(tests):
        s.write(f"    .globl t{i}\nt{i}:  // {text}\n{pre}    .inst {w:#010x}\n{post}")
    s.write("    .section .rodata\n    .p2align 3\n    .globl g_tests\ng_tests:\n")
    for i in range(len(tests)):
        s.write(f"    .quad t{i}\n")
with open("insn_table.h", "w") as h:
    h.write(f"#define NTESTS {len(tests)}\n")
    h.write("static const struct { unsigned word; unsigned char mem, rn, rm; const char* text; } g_info[] = {\n")
    for w, text, mem, rn, rm in tests:
        esc = text.replace("\\", "\\\\").replace('"', '\\"')
        h.write(f'  {{{w:#010x}u, {int(mem)}, {rn}, {rm}, "{esc}"}},\n')
    h.write("};\n")
print(f"{len(tests)} tests")
