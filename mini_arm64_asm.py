#!/usr/bin/env python3
"""
mini_arm64_asm.py - tiny ARM64 assembler + static ELF emitter.

This is NOT a full assembler - it supports just enough instructions
to write test programs for the emulator (hello world, echo loop,
interactive echo). The instruction set supported:

  mov  Xd, #imm              (MOVZ, MOVK with hw=0,1,2,3)
  movk Xd, #imm, lsl #N
  add  Xd, Xn, #imm
  add  Xd, Xn, Xm
  sub  Xd, Xn, #imm
  subs Xd, Xn, #imm
  cmp  Xn, #imm              (alias: subs XZR, Xn, #imm)
  cmp  Xn, Xm                (alias: subs XZR, Xn, Xm)
  str  Xt, [Xn, #imm]
  ldr  Xt, [Xn, #imm]
  strb Xt, [Xn, #imm]
  ldrb Xt, [Xn, #imm]
  stp  Xt1, Xt2, [Xn, #imm]
  ldp  Xt1, Xt2, [Xn, #imm]
  adr  Xd, label
  b    label
  bl   label
  ret
  svc  #0
  bz/bnz/cbz/cbnz Xn, label

Labels are written as "name:" on their own line.
Comments start with ";" or "#".
Register names: x0-x30, sp, wzr/xzr.

Usage:
  python3 mini_arm64_asm.py prog.s -o prog.elf
"""

import sys
import struct
import argparse


REGISTERS = {f"x{i}": i for i in range(31)}
REGISTERS.update({f"w{i}": i for i in range(31)})
REGISTERS["sp"] = 31
REGISTERS["xzr"] = 31
REGISTERS["wzr"] = 31


def reg(tok):
    tok = tok.strip().lower()
    if tok not in REGISTERS:
        raise ValueError(f"unknown register: {tok}")
    return REGISTERS[tok]


def is_xreg(tok):
    return tok.lower().startswith("x") or tok.lower() in ("sp", "xzr")


def imm(tok):
    tok = tok.strip()
    neg = False
    if tok.startswith("-"):
        neg = True
        tok = tok[1:]
    if tok.startswith("#"): tok = tok[1:]
    if tok.startswith("0x") or tok.startswith("0X"):
        v = int(tok, 16)
    elif tok.startswith("0b"):
        v = int(tok, 2)
    else:
        v = int(tok)
    return -v if neg else v


# ----- instruction encoders ------------------------------------------------

def enc_movz(sf, rd, imm16, hw):
    return (sf << 31) | (0b10 << 29) | (0b100101 << 23) | (hw << 21) | \
           ((imm16 & 0xFFFF) << 5) | rd

def enc_movk(sf, rd, imm16, hw):
    return (sf << 31) | (0b11 << 29) | (0b100101 << 23) | (hw << 21) | \
           ((imm16 & 0xFFFF) << 5) | rd

def enc_movn(sf, rd, imm16, hw):
    return (sf << 31) | (0b00 << 29) | (0b100101 << 23) | (hw << 21) | \
           ((imm16 & 0xFFFF) << 5) | rd

def enc_add_imm(sf, rd, rn, imm12, sh=0, set_flags=False):
    opc = 0b11 if set_flags else 0b00
    if not set_flags and opc == 0b11:
        opc = 0b01
    # ADDS = 01, SUBS = 11
    if set_flags:
        opc = 0b01  # ADDS
    return (sf << 31) | (opc << 29) | (1 << 28) | (0 << 27) | \
           (0 << 26) | (0b010 << 23) | (sh << 22) | ((imm12 & 0xFFF) << 10) | \
           (rn << 5) | rd

def enc_sub_imm(sf, rd, rn, imm12, sh=0, set_flags=False):
    opc = 0b11 if set_flags else 0b10
    return (sf << 31) | (opc << 29) | (1 << 28) | (0 << 27) | \
           (0 << 26) | (0b010 << 23) | (sh << 22) | ((imm12 & 0xFFF) << 10) | \
           (rn << 5) | rd

def enc_add_reg(sf, rd, rn, rm, shift=0, imm6=0, set_flags=False):
    opc = 0b01 if set_flags else 0b00
    return (sf << 31) | (opc << 29) | (0b01011 << 24) | (shift << 22) | \
           (0 << 21) | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd

def enc_sub_reg(sf, rd, rn, rm, shift=0, imm6=0, set_flags=False):
    opc = 0b11 if set_flags else 0b10
    return (sf << 31) | (opc << 29) | (0b01011 << 24) | (shift << 22) | \
           (0 << 21) | (rm << 16) | ((imm6 & 0x3F) << 10) | (rn << 5) | rd

def enc_str_imm(size, rt, rn, imm12):
    # unsigned offset immediate
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b01 << 24) | \
           (0b00 << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt

def enc_ldr_imm(size, rt, rn, imm12):
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b01 << 24) | \
           (0b01 << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rt

def enc_str_imm_signed(size, rt, rn, imm9):
    # STUR - 0b00 unscaled
    imm9 &= 0x1FF
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | \
           (0b00 << 22) | (0 << 21) | (imm9 << 12) | (0b10 << 10) | \
           (rn << 5) | rt

def enc_ldr_imm_signed(size, rt, rn, imm9):
    imm9 &= 0x1FF
    return (size << 30) | (0b111 << 27) | (0 << 26) | (0b00 << 24) | \
           (0b01 << 22) | (0 << 21) | (imm9 << 12) | (0b10 << 10) | \
           (rn << 5) | rt

def enc_stp(sf, rt1, rt2, rn, imm7, mode="offset"):
    opc = 0b10 if sf else 0b00
    mode_bits = {"offset": 0b00, "post": 0b01, "pre": 0b11}[mode]
    return (opc << 30) | (0b101 << 27) | (0 << 26) | (0 << 25) | \
           (mode_bits << 23) | ((imm7 & 0x7F) << 15) | (rt2 << 10) | \
           (rn << 5) | rt1

def enc_ldp(sf, rt1, rt2, rn, imm7, mode="offset"):
    opc = 0b10 if sf else 0b00
    mode_bits = {"offset": 0b00, "post": 0b01, "pre": 0b11}[mode]
    return (opc << 30) | (0b101 << 27) | (0 << 26) | (1 << 22) | \
           (mode_bits << 23) | ((imm7 & 0x7F) << 15) | (rt2 << 10) | \
           (rn << 5) | rt1

def enc_b(imm26):
    imm26 &= 0x3FFFFFF
    return (0b000101 << 26) | imm26

def enc_bl(imm26):
    imm26 &= 0x3FFFFFF
    return (0b100101 << 26) | imm26

def enc_bcond(cond, imm19):
    imm19 &= 0x7FFFF
    return (0b01010100 << 24) | (imm19 << 5) | (0 << 4) | (cond & 0xF)

def enc_cbz(sf, rt, imm19):
    imm19 &= 0x7FFFF
    return (sf << 31) | (0b011010 << 25) | (0 << 24) | (imm19 << 5) | rt

def enc_cbnz(sf, rt, imm19):
    imm19 &= 0x7FFFF
    return (sf << 31) | (0b011010 << 25) | (1 << 24) | (imm19 << 5) | rt

def enc_ret(rn=30):
    # 1101 0110 0101 1111 0000 00 Rn 00000 = 0xD65F0000 | (Rn<<5)
    return (0xD6 << 24) | (0b010 << 21) | (0b11111 << 16) | \
           (0b000000 << 10) | (rn << 5) | 0b00000

def enc_svc(imm16=0):
    # 1101 0100 000 imm16 000 00001 = 0xD4000001 | (imm16<<5)
    return (0xD4 << 24) | ((imm16 & 0xFFFF) << 5) | 0b00001

def enc_br(rn):
    # 1101 0110 0011 1111 0000 00 Rn 00000 = 0xD61F0000 | (Rn<<5)
    return (0xD6 << 24) | (0b001 << 21) | (0b11111 << 16) | \
           (0b000000 << 10) | (rn << 5) | 0b00000

def enc_blr(rn):
    # 1101 0110 0011 1111 0000 01 Rn 00000 = 0xD63F0000 | (Rn<<5)
    return (0xD6 << 24) | (0b001 << 21) | (0b11111 << 16) | \
           (0b000001 << 10) | (rn << 5) | 0b00000

def enc_adr(rd, imm):
    # imm is byte offset from PC, must be multiple of 1 (ADR allows any)
    imm21 = imm & 0x1FFFFF
    immlo = imm21 & 0x3
    immhi = (imm21 >> 2) & 0x7FFFF
    return (0 << 31) | (immlo << 29) | (0b10000 << 24) | (immhi << 5) | rd

def enc_adrp(rd, imm):
    # imm is the *byte offset* / 4096
    imm21 = (imm >> 12) & 0x1FFFFF
    immlo = imm21 & 0x3
    immhi = (imm21 >> 2) & 0x7FFFF
    return (1 << 31) | (immlo << 29) | (0b10000 << 24) | (immhi << 5) | rd

def enc_madd(sf, rd, rn, rm, ra):
    return (sf << 31) | (0b00 << 29) | (0b11011 << 24) | (rm << 16) | \
           (0 << 15) | (ra << 10) | (rn << 5) | rd

def enc_msub(sf, rd, rn, rm, ra):
    return (sf << 31) | (0b00 << 29) | (0b11011 << 24) | (rm << 16) | \
           (1 << 15) | (ra << 10) | (rn << 5) | rd

def enc_orr_reg(sf, rd, rn, rm):
    # ORR Rd, Rn, Rm (no shift)
    return (sf << 31) | (0b01 << 29) | (0b01010 << 24) | (0 << 22) | \
           (0 << 21) | (rm << 16) | (0 << 10) | (rn << 5) | rd


CONDS = {
    "eq": 0x0, "ne": 0x1, "cs": 0x2, "hs": 0x2, "cc": 0x3, "lo": 0x3,
    "mi": 0x4, "pl": 0x5, "vs": 0x6, "vc": 0x7,
    "hi": 0x8, "ls": 0x9, "ge": 0xa, "lt": 0xb, "gt": 0xc, "le": 0xd,
    "al": 0xe,
}


# ----- assembler -----------------------------------------------------------

def assemble(text):
    """Return a list of (addr, word) tuples. 4-byte aligned."""
    lines = text.splitlines()
    # First pass: collect labels and instructions
    addr = 0
    pending = []   # (addr, opcode, args_list, lineno, kind)
    labels = {}
    for lineno, raw in enumerate(lines, 1):
        # Strip comments. Use ';' as comment char always.
        # '#' is the immediate prefix in ARM64 syntax, so only treat as
        # comment when preceded by whitespace and not followed by a digit/x.
        line = raw
        if ";" in line:
            line = line.split(";", 1)[0]
        # Find a "#" that's a comment vs immediate. A comment "# ..." has
        # whitespace before the # and non-digit (or 'x') right after.
        i = 0
        while i < len(line):
            if line[i] == "#":
                prev = line[i-1] if i > 0 else " "
                nxt = line[i+1] if i+1 < len(line) else " "
                if prev.isspace() and not (nxt.isdigit() or nxt in "-+xX"):
                    line = line[:i]
                    break
            i += 1
        line = line.strip()
        if not line:
            continue
        # Label?
        # Look for a leading "label:" (only one label per line, identifier
        # followed by colon at the very start). Don't split inside quotes.
        if line and (line[0].isalpha() or line[0] == '_'):
            # Find the first ':' that's not inside quotes.
            in_q = False
            for i, c in enumerate(line):
                if c == '"': in_q = not in_q
                elif c == ':' and not in_q:
                    label = line[:i].strip()
                    if label and all(c.isalnum() or c in "._" for c in label):
                        labels[label] = addr
                        line = line[i+1:].strip()
                        break
                    else:
                        break  # not a valid label, leave the line alone
        if not line:
            continue
        # Split mnemonic from operands
        parts = line.split(None, 1)
        mnem = parts[0].lower()
        operands = parts[1] if len(parts) > 1 else ""
        ops = split_operands(operands)
        if mnem == ".byte":
            data = bytes(imm(o) & 0xFF for o in ops)
            for i, b in enumerate(data):
                pending.append((addr + i, ".byte", b, lineno, "byte"))
            addr += len(data)
            # Pad to 4-byte alignment
            while addr % 4 != 0:
                pending.append((addr, ".byte", 0, lineno, "byte"))
                addr += 1
        elif mnem == ".ascii":
            # operands are quoted strings; re-join
            s = operands.strip()
            if s.startswith('"'):
                end = s.rfind('"')
                if end > 0:
                    s = s[1:end]
            data = s.encode().decode("unicode_escape").encode("latin-1")
            for i, b in enumerate(data):
                pending.append((addr + i, ".byte", b, lineno, "byte"))
            addr += len(data)
            while addr % 4 != 0:
                pending.append((addr, ".byte", 0, lineno, "byte"))
                addr += 1
        else:
            pending.append((addr, mnem, ops, lineno, "inst"))
            addr += 4
    # Second pass: encode
    out = []
    for cur_addr, mnem, ops, lineno, kind in pending:
        if kind == "byte":
            out.append((cur_addr, ops & 0xFF))  # bytes stored as 1, but in code region we'll handle them
            continue
        try:
            word = encode(cur_addr, mnem, ops, labels)
        except Exception as e:
            raise ValueError(f"line {lineno}: {mnem} {ops}: {e}")
        out.append((cur_addr, word))
    return out, labels


def split_operands(s):
    """Split operand string on commas, but keep [a, b] together."""
    out = []
    cur = ""
    depth = 0
    for c in s:
        if c == "[": depth += 1; cur += c
        elif c == "]": depth -= 1; cur += c
        elif c == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else: cur += c
    if cur.strip(): out.append(cur.strip())
    return out


def encode(addr, mnem, ops, labels):
    def is_w(tok): return tok.lower().startswith("w")
    def is_x(tok): return tok.lower().startswith("x") or tok.lower() in ("sp","xzr")

    if mnem in ("mov", "movz"):
        rd = reg(ops[0]); sf = 0 if is_w(ops[0]) else 1
        # mov Xd, Xm  -> ORR Xd, XZR, Xm
        if not ops[1].startswith("#") and ops[1].lower() not in ("sp",):
            # also handle mov Xd, Xm
            rm = reg(ops[1])
            return enc_orr_reg(sf, rd, 31, rm)
        v = imm(ops[1])
        return enc_movz_full(sf, rd, v)
    if mnem == "movn":
        rd = reg(ops[0]); sf = 0 if is_w(ops[0]) else 1
        v = imm(ops[1])
        return enc_movn_full(sf, rd, v)
    if mnem == "movk":
        rd = reg(ops[0]); sf = 0 if is_w(ops[0]) else 1
        v = imm(ops[1])
        hw = 0
        if len(ops) > 2 and ops[2].lower().startswith("lsl"):
            shift = imm(ops[2].split("#")[1] if "#" in ops[2] else ops[2].split()[-1])
            hw = shift // 16
        return enc_movk(sf, rd, v & 0xFFFF, hw)
    if mnem == "add":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        if ops[2].startswith("#"):
            return enc_add_imm(sf, rd, rn, imm(ops[2]) & 0xFFF)
        else:
            return enc_add_reg(sf, rd, rn, reg(ops[2]))
    if mnem == "adds":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        if ops[2].startswith("#"):
            return enc_add_imm(sf, rd, rn, imm(ops[2]) & 0xFFF, set_flags=True)
        else:
            return enc_add_reg(sf, rd, rn, reg(ops[2]), set_flags=True)
    if mnem == "subs":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        if ops[2].startswith("#"):
            return enc_sub_imm(sf, rd, rn, imm(ops[2]) & 0xFFF, set_flags=True)
        else:
            return enc_sub_reg(sf, rd, rn, reg(ops[2]), set_flags=True)
    if mnem == "sub":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        if ops[2].startswith("#"):
            return enc_sub_imm(sf, rd, rn, imm(ops[2]) & 0xFFF)
        else:
            return enc_sub_reg(sf, rd, rn, reg(ops[2]))
    if mnem == "cmp":
        rn = reg(ops[0]); sf = 0 if is_w(ops[0]) else 1
        if ops[1].startswith("#"):
            return enc_sub_imm(sf, 31, rn, imm(ops[1]) & 0xFFF, set_flags=True)
        else:
            return enc_sub_reg(sf, 31, rn, reg(ops[1]), set_flags=True)
    if mnem == "cmn":
        rn = reg(ops[0]); sf = 0 if is_w(ops[0]) else 1
        if ops[1].startswith("#"):
            return enc_add_imm(sf, 31, rn, imm(ops[1]) & 0xFFF, set_flags=True)
        else:
            return enc_add_reg(sf, 31, rn, reg(ops[1]), set_flags=True)
    if mnem == "neg":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        return enc_sub_reg(sf, rd, 31, rn)
    if mnem == "orr":
        rd = reg(ops[0]); rn = reg(ops[1]); sf = 0 if is_w(ops[0]) else 1
        return enc_orr_reg(sf, rd, rn, reg(ops[2]))
    if mnem in ("str", "ldr", "strb", "ldrb", "strh", "ldrh", "strw", "ldrw"):
        rt = reg(ops[0])
        # parse [Xn, #imm] or [Xn] or [Xn], #imm
        mem = ops[1]
        if mem.startswith("["):
            inner = mem[1:].split("]")[0]
            parts = [p.strip() for p in inner.split(",")]
            rn = reg(parts[0])
            offset = 0
            post_index = None
            if len(parts) > 1:
                offset = imm(parts[1])
            # post-index: ops[1] like "[Xn]", #imm
            if len(ops) > 2:
                # post-indexed
                post_index = imm(ops[2])
        else:
            raise ValueError("expected [Xn, #imm]")
        size_map = {"str": 3, "ldr": 3, "strb": 0, "ldrb": 0,
                    "strh": 1, "ldrh": 1, "strw": 2, "ldrw": 2}
        size = size_map[mnem]
        # post-index?
        if post_index is not None:
            # use signed offset encoding with post-index
            if mnem.startswith("str"):
                return enc_str_imm_signed(size, rt, rn, post_index) | (1 << 10)
            else:
                return enc_ldr_imm_signed(size, rt, rn, post_index) | (1 << 10)
        # If offset is unsigned and aligned, use unsigned form
        if offset >= 0 and (offset % (1 << size)) == 0:
            if mnem.startswith("str"):
                return enc_str_imm(size, rt, rn, offset >> size)
            else:
                return enc_ldr_imm(size, rt, rn, offset >> size)
        else:
            # Use signed (LDUR/STUR) form
            if mnem.startswith("str"):
                return enc_str_imm_signed(size, rt, rn, offset)
            else:
                return enc_ldr_imm_signed(size, rt, rn, offset)
    if mnem in ("stp", "ldp"):
        rt1 = reg(ops[0]); rt2 = reg(ops[1])
        sf = 1  # default to 64-bit
        if is_w(ops[0]): sf = 0
        mem = ops[2]
        if not mem.startswith("["):
            raise ValueError("expected [Xn, #imm]")
        inner = mem[1:].split("]")[0]
        parts = [p.strip() for p in inner.split(",")]
        rn = reg(parts[0])
        offset = 0
        if len(parts) > 1:
            offset = imm(parts[1])
        mode = "offset"
        # post-index: ops[2] like "[Xn]", #imm
        if len(ops) > 3:
            offset = imm(ops[3])
            mode = "post"
        esize = 8 if sf else 4
        if offset % esize != 0:
            raise ValueError("stp/ldp offset must be multiple of element size")
        imm7 = offset // esize
        if mnem == "stp":
            return enc_stp(sf, rt1, rt2, rn, imm7, mode=mode)
        else:
            return enc_ldp(sf, rt1, rt2, rn, imm7, mode=mode)
    if mnem == "b":
        target = labels[ops[0]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        return enc_b(offset // 4)
    if mnem == "bl":
        target = labels[ops[0]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        return enc_bl(offset // 4)
    if mnem == "ret":
        if ops:
            return enc_ret(reg(ops[0]))
        return enc_ret(30)
    if mnem == "br":
        return enc_br(reg(ops[0]))
    if mnem == "blr":
        return enc_blr(reg(ops[0]))
    if mnem == "svc":
        return enc_svc(imm(ops[0]) if ops else 0)
    if mnem == "extr":
        # EXTR Xd, Xn, Xm, #lsb
        # Encoding: sf 00 100111 N Rm imms Rn Rd
        # (bits[28:23] = 100111, N matches sf for EXTR)
        # For 64-bit (sf=1, N=1); for 32-bit (sf=0, N=0).
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2])
        sf = 0 if is_w(ops[0]) else 1
        n = sf  # N matches sf for EXTR
        imms = imm(ops[3]) & 0x3F
        return (sf << 31) | (0b00 << 29) | (0b100111 << 23) | (n << 22) | \
               (rm << 16) | (imms << 10) | (rn << 5) | rd
    if mnem == "adr":
        rd = reg(ops[0])
        target = labels[ops[1]]
        offset = target - addr
        return enc_adr(rd, offset)
    if mnem == "adrp":
        rd = reg(ops[0])
        target = labels[ops[1]]
        offset = target - (addr & ~0xFFF)
        return enc_adrp(rd, offset)
    if mnem == "madd":
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2]); ra = reg(ops[3])
        sf = 0 if is_w(ops[0]) else 1
        return enc_madd(sf, rd, rn, rm, ra)
    if mnem == "msub":
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2]); ra = reg(ops[3])
        sf = 0 if is_w(ops[0]) else 1
        return enc_msub(sf, rd, rn, rm, ra)
    if mnem == "mul":
        # mul Rd, Rn, Rm == madd Rd, Rn, Rm, XZR
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2])
        sf = 0 if is_w(ops[0]) else 1
        return enc_madd(sf, rd, rn, rm, 31)
    if mnem == "udiv":
        # UDIV Rd, Rn, Rm : sf 0 0 11010110 Rm 000010 Rn Rd
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2])
        sf = 0 if is_w(ops[0]) else 1
        return (sf << 31) | (0b00011010110 << 21) | (rm << 16) | \
               (0b000010 << 10) | (rn << 5) | rd
    if mnem == "sdiv":
        # SDIV Rd, Rn, Rm : sf 0 0 11010110 Rm 000011 Rn Rd
        rd = reg(ops[0]); rn = reg(ops[1]); rm = reg(ops[2])
        sf = 0 if is_w(ops[0]) else 1
        return (sf << 31) | (0b00011010110 << 21) | (rm << 16) | \
               (0b000011 << 10) | (rn << 5) | rd
    if mnem == "nop":
        return 0xD503201F
    if mnem in CONDS:
        # b.cond label
        target = labels[ops[0]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        return enc_bcond(CONDS[mnem], offset // 4)
    if mnem in ("b.eq","b.ne","b.cs","b.cc","b.mi","b.pl","b.vs","b.vc",
                "b.hi","b.ls","b.ge","b.lt","b.gt","b.le","b.al"):
        cond = mnem.split(".")[1]
        target = labels[ops[0]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        return enc_bcond(CONDS[cond], offset // 4)
    if mnem == "cbz":
        rt = reg(ops[0]); target = labels[ops[1]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        sf = 0 if is_w(ops[0]) else 1
        return enc_cbz(sf, rt, offset // 4)
    if mnem == "cbnz":
        rt = reg(ops[0]); target = labels[ops[1]]
        offset = target - addr
        if offset % 4: raise ValueError("branch target not 4-aligned")
        sf = 0 if is_w(ops[0]) else 1
        return enc_cbnz(sf, rt, offset // 4)
    raise ValueError(f"unsupported mnemonic: {mnem}")


def enc_movz_full(sf, rd, value):
    """Emit up to 4 MOVZ/MOVK instructions to materialize a 64-bit value.
       Returns the FIRST word. The caller is responsible for emitting the
       remaining MOVKs - but since we encode one instruction per line, we
       only support single-MOVZ MOV here (value must fit in 16 bits).
       Use movk for wider."""
    if value <= 0xFFFF:
        return enc_movz(sf, rd, value & 0xFFFF, 0)
    # If value > 16 bits, the caller should use movk.
    # For convenience, still emit just the low 16 bits via MOVZ.
    return enc_movz(sf, rd, value & 0xFFFF, 0)


# ----- ELF emission --------------------------------------------------------

def build_elf(code_bytes, entry=0x400000):
    """Wrap raw code in a minimal static ELF64 AArch64 executable."""
    base = entry & ~0xFFF
    # Layout:
    #   base + 0x0000 : ELF header (64 bytes)
    #   base + 0x0040 : program header (56 bytes)
    #   base + 0x0078 : code (page-aligned within file)
    # We'll keep it simple: ELF header + 1 PHDR + code in single PT_LOAD.
    ehdr_size = 64
    phdr_size = 56
    file_hdr_size = ehdr_size + phdr_size
    file_code_off = file_hdr_size
    file_size = file_hdr_size + len(code_bytes)

    # ELF header
    e_ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8
    e_type = 2  # ET_EXEC
    e_machine = 183  # EM_AARCH64
    e_version = 1
    e_entry = entry
    e_phoff = ehdr_size
    e_shoff = 0
    e_flags = 0
    e_ehsize = ehdr_size
    e_phentsize = phdr_size
    e_phnum = 1
    e_shentsize = 0
    e_shnum = 0
    e_shstrndx = 0
    ehdr = e_ident + struct.pack("<HHIQQQIHHHHHH",
        e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
        e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx)

    # Program header - single PT_LOAD covering headers + code
    p_type = 1   # PT_LOAD
    p_flags = 5  # PF_R | PF_X
    p_offset = 0
    p_vaddr = base
    p_paddr = base
    p_filesz = file_size
    p_memsz = file_size
    p_align = 0x1000
    phdr = struct.pack("<IIQQQQQQ",
        p_type, p_flags, p_offset, p_vaddr, p_paddr,
        p_filesz, p_memsz, p_align)

    return ehdr + phdr + code_bytes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--base", default="0x400000")
    args = ap.parse_args()

    with open(args.infile) as f:
        text = f.read()
    pairs, labels = assemble(text)
    if not pairs:
        sys.exit("no instructions emitted")
    base = int(args.base, 0)
    # Compute file offsets: code starts at base + 64 + 56 = base + 120
    code_off = 64 + 56
    # Build the code image: walk addresses, write each entry
    # into a bytearray. Instructions are 4-byte little-endian words,
    # bytes are single bytes.
    if pairs[0][0] != 0:
        sys.exit(f"first item at addr {pairs[0][0]}, expected 0")
    max_addr = max(a for a, _ in pairs)
    code = bytearray(max_addr + 4)
    for addr, val in pairs:
        if isinstance(val, int) and val <= 0xFF:
            # a byte
            code[addr] = val & 0xFF
        else:
            struct.pack_into("<I", code, addr, val & 0xFFFFFFFF)
    elf = build_elf(bytes(code), entry=base + code_off)
    with open(args.output, "wb") as f:
        f.write(elf)
    print(f"wrote {args.output} ({len(elf)} bytes, {len(pairs)} items, {len(labels)} labels)")


if __name__ == "__main__":
    main()
