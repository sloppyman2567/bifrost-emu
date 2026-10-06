#!/usr/bin/env python3
"""Minimal dex method disassembler: dexmethod.py classes.dex Lpkg/Cls; <method>"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
want_cls, want_m = sys.argv[2], sys.argv[3]
u32 = lambda o: struct.unpack_from("<I", d, o)[0]
u16 = lambda o: struct.unpack_from("<H", d, o)[0]


def uleb(o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if b < 0x80:
            return r, o


def string(i):
    off = u32(u32(0x3C) + 4 * i)
    _, off = uleb(off)
    return d[off:d.index(b"\0", off)].decode("utf-8", "replace")


type_ids = u32(0x44)
typ = lambda i: string(u32(type_ids + 4 * i))
field_ids, method_ids = u32(0x54), u32(0x5C)


def field(i):
    o = field_ids + 8 * i
    return f"{typ(u16(o))}->{string(u32(o + 4))}:{typ(u16(o + 2))}"


def method(i):
    o = method_ids + 8 * i
    return f"{typ(u16(o))}->{string(u32(o + 4))}"


# opcode -> (name, format). Only formats needed for reading.
OPS = {}
def op(code, name, fmt): OPS[code] = (name, fmt)
for c, n in [(0x00, "nop"), (0x0e, "return-void")]: op(c, n, "10x")
for c, n in [(0x01, "move"), (0x04, "move-wide"), (0x07, "move-object"), (0x21, "array-length")]: op(c, n, "12x")
for c, n in [(0x0a, "move-result"), (0x0b, "move-result-wide"), (0x0c, "move-result-object"),
             (0x0d, "move-exception"), (0x0f, "return"), (0x10, "return-wide"), (0x11, "return-object"),
             (0x1d, "monitor-enter"), (0x1e, "monitor-exit"), (0x27, "throw")]: op(c, n, "11x")
op(0x12, "const/4", "11n"); op(0x13, "const/16", "21s"); op(0x14, "const", "31i")
op(0x15, "const/high16", "21h"); op(0x16, "const-wide/16", "21s"); op(0x18, "const-wide", "51l")
op(0x1a, "const-string", "21c:s"); op(0x1b, "const-string/jumbo", "31c")
op(0x1c, "const-class", "21c:t"); op(0x1f, "check-cast", "21c:t"); op(0x22, "new-instance", "21c:t")
op(0x20, "instance-of", "22c:t"); op(0x23, "new-array", "22c:t")
op(0x28, "goto", "10t"); op(0x29, "goto/16", "20t")
for c, n in zip(range(0x32, 0x38), ["if-eq", "if-ne", "if-lt", "if-ge", "if-gt", "if-le"]): op(c, n, "22t")
for c, n in zip(range(0x38, 0x3e), ["if-eqz", "if-nez", "if-ltz", "if-gez", "if-gtz", "if-lez"]): op(c, n, "21t")
for c in range(0x44, 0x52): op(c, ["aget", "aget-wide", "aget-object", "aget-boolean", "aget-byte", "aget-char", "aget-short",
                                    "aput", "aput-wide", "aput-object", "aput-boolean", "aput-byte", "aput-char", "aput-short"][c - 0x44], "23x")
for c in range(0x52, 0x60): op(c, ["iget", "iget-wide", "iget-object", "iget-boolean", "iget-byte", "iget-char", "iget-short",
                                    "iput", "iput-wide", "iput-object", "iput-boolean", "iput-byte", "iput-char", "iput-short"][c - 0x52], "22c:f")
for c in range(0x60, 0x6e): op(c, ["sget", "sget-wide", "sget-object", "sget-boolean", "sget-byte", "sget-char", "sget-short",
                                    "sput", "sput-wide", "sput-object", "sput-boolean", "sput-byte", "sput-char", "sput-short"][c - 0x60], "21c:f")
for c, n in zip(range(0x6e, 0x73), ["invoke-virtual", "invoke-super", "invoke-direct", "invoke-static", "invoke-interface"]): op(c, n, "35c")
for c, n in zip(range(0x74, 0x79), ["invoke-virtual/range", "invoke-super/range", "invoke-direct/range", "invoke-static/range", "invoke-interface/range"]): op(c, n, "3rc")
op(0x24, "filled-new-array", "35c"); op(0x26, "fill-array-data", "31t")
for c in range(0x2d, 0x32): op(c, ["cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double", "cmp-long"][c - 0x2d], "23x")
for c in range(0x90, 0xb0): op(c, f"binop-{c:#x}", "23x")
for c in range(0xb0, 0xd0): op(c, f"binop2addr-{c:#x}", "12x")
for c in range(0xd0, 0xd8): op(c, f"binop/lit16-{c:#x}", "22s")
for c in range(0xd8, 0xe3): op(c, f"binop/lit8-{c:#x}", "22b")
for c in range(0x7b, 0x90): op(c, f"unop-{c:#x}", "12x")
SIZE = {"10x": 1, "12x": 1, "11x": 1, "11n": 1, "10t": 1, "20t": 2, "21s": 2, "21h": 2, "21t": 2, "22t": 2,
        "21c": 2, "22c": 2, "23x": 2, "22s": 2, "22b": 2, "31i": 3, "31c": 3, "31t": 3, "35c": 3, "3rc": 3, "51l": 5}


def disasm(code_off):
    regs, ins, outs, tries, _, n = struct.unpack_from("<HHHHII", d, code_off)
    print(f"  registers={regs} ins={ins} outs={outs} tries={tries} insns={n}")
    base = code_off + 16
    pc = 0
    while pc < n:
        w = u16(base + 2 * pc)
        o = w & 0xff
        if w in (0x100, 0x200, 0x300):  # payloads
            print(f"  {pc:04x}: <payload {w:#x}>"); break
        name, fmt = OPS.get(o, (f"op{o:#04x}", "10x"))
        kind = fmt.split(":")[1] if ":" in fmt else None
        f = fmt.split(":")[0]
        a = w >> 8
        txt = ""
        if f == "21c":
            idx = u16(base + 2 * pc + 2)
            ref = {"s": lambda i: repr(string(i)), "t": typ, "f": field}[kind](idx)
            txt = f"v{a}, {ref}"
        elif f == "22c":
            idx = u16(base + 2 * pc + 2)
            ref = {"t": typ, "f": field}[kind](idx)
            txt = f"v{a & 0xf}, v{a >> 4}, {ref}"
        elif f == "35c":
            idx = u16(base + 2 * pc + 2); g = u16(base + 2 * pc + 4)
            cnt = a >> 4
            rs = [g & 0xf, (g >> 4) & 0xf, (g >> 8) & 0xf, g >> 12, a & 0xf][:cnt]
            txt = "{" + ", ".join(f"v{r}" for r in rs) + "}, " + method(idx)
        elif f == "3rc":
            idx = u16(base + 2 * pc + 2); c = u16(base + 2 * pc + 4)
            txt = f"{{v{c}..v{c + a - 1}}}, " + method(idx)
        elif f in ("21s", "21t", "21h"):
            txt = f"v{a}, {struct.unpack_from('<h', d, base + 2 * pc + 2)[0]}"
        elif f == "31i":
            txt = f"v{a}, {struct.unpack_from('<i', d, base + 2 * pc + 2)[0]:#x}"
        elif f == "11n":
            txt = f"v{a & 0xf}, {((a >> 4) ^ 8) - 8}"
        elif f in ("11x",):
            txt = f"v{a}"
        elif f in ("12x",):
            txt = f"v{a & 0xf}, v{a >> 4}"
        elif f == "23x":
            b = u16(base + 2 * pc + 2); txt = f"v{a}, v{b & 0xff}, v{b >> 8}"
        print(f"  {pc:04x}: {name} {txt}")
        pc += SIZE.get(f, 1)


for ci in range(u32(0x60)):
    cd = u32(0x64) + 32 * ci
    if typ(u32(cd)) != want_cls:
        continue
    data = u32(cd + 24)
    sf, o = uleb(data); inf, o = uleb(o); dm, o = uleb(o); vm, o = uleb(o)
    for _ in range(sf + inf):
        _, o = uleb(o); _, o = uleb(o)
    midx = 0
    for k in range(dm + vm):
        if k == dm:
            midx = 0
        diff, o = uleb(o); _, o = uleb(o); code, o = uleb(o)
        midx += diff
        mo = method_ids + 8 * midx
        if string(u32(mo + 4)) == want_m and code:
            print(method(midx))
            disasm(code)
