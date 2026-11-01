#!/usr/bin/env python3
"""simd_coverage.py — sweep the AArch64 FP/SIMD data-processing space.

Every word with bits 27:25 = 111 (the "Scalar Floating-Point and Advanced
SIMD" data-processing group: no memory access or branches) is run once as an
`insn; ret` stub with Rd = 2 and Rn = 0 pinned. That leaves 2^19 words; Rm,
Ra and every opcode/size/Q field vary. All 32 vector registers and x0 start
from fixed patterns (three input sets), and v2, x2 and NZCV are hashed.

The same guest binary runs under qemu-aarch64 (the oracle) and Bifrost with
BIFROST_FALLBACK_PROFILE. Each word lands in one bucket:
  missing     QEMU executes it, Bifrost raises SIGILL
  accepted    QEMU raises SIGILL, Bifrost executes it
  mismatch    both execute, results differ
  interpreted both agree, but the JIT stepped the interpreter for it
Findings are grouped by disassembled mnemonic and arrangement.

Usage:
    python3 Scripts/Testing/simd_coverage.py [--emu PATH] [--out DIR] [--limit N]
Outputs DIR/report.txt plus the raw per-engine result files (default
build/simd-coverage).
"""
import argparse
import collections
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CROSS = ROOT / "Tools/aarch64-linux-musl-cross/bin"

RUNNER = r"""
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern const uint32_t cov_stubs[];
extern const uint32_t cov_count;
static sigjmp_buf trap;
static void on_sigill(int sig) { (void)sig; siglongjmp(trap, 1); }
static uint8_t regs[3][512];
static void init(void) {
    static const uint32_t f32[] = {0, 0x80000000, 0x3fc00000, 0xc0200000, 0x7f800000, 0xff800000,
        0x7fc00001, 0x7f800001, 0x7f7fffff, 0x00000001, 0x4b000001, 0x3f000000};
    static const uint64_t f64[] = {0, 0x8000000000000000ull, 0x3ff8000000000000ull,
        0x7ff0000000000000ull, 0xfff8000000000002ull, 0x7ff0000000000001ull,
        0x0000000000000001ull, 0x433fffffffffffffull, 0xc3e0000000000000ull, 0x3fe0000000000000ull};
    for (int i = 0; i < 512; ++i) regs[0][i] = (uint8_t)(i * 37 + (i >> 3) * 11 + 0x5a);
    for (int i = 0; i < 128; ++i) memcpy(regs[1] + 4 * i, &f32[(i * 7 + i / 5) % 12], 4);
    for (int i = 0; i < 64; ++i) memcpy(regs[2] + 8 * i, &f64[(i * 3 + i / 4) % 10], 8);
}
int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, 0);
    init();
    setvbuf(stdout, 0, _IOLBF, 0);  /* a crashing word stays visible */
    for (uint32_t i = 0; i < cov_count; ++i) {
        const uint32_t* stub = cov_stubs + 2 * i;
        uint32_t h = 2166136261u;
        int ill = 0;
        for (int s = 0; s < 3 && !ill; ++s) {
            uint8_t out[16]; uint64_t x2 = 0, nzcv = 0;
            if (sigsetjmp(trap, 1)) { ill = 1; break; }
            __asm__ volatile(
                "mov x9, %3\n"
                "ldp q0, q1, [x9]\n ldp q2, q3, [x9, 32]\n ldp q4, q5, [x9, 64]\n ldp q6, q7, [x9, 96]\n"
                "ldp q8, q9, [x9, 128]\n ldp q10, q11, [x9, 160]\n ldp q12, q13, [x9, 192]\n"
                "ldp q14, q15, [x9, 224]\n ldp q16, q17, [x9, 256]\n ldp q18, q19, [x9, 288]\n"
                "ldp q20, q21, [x9, 320]\n ldp q22, q23, [x9, 352]\n ldp q24, q25, [x9, 384]\n"
                "ldp q26, q27, [x9, 416]\n ldp q28, q29, [x9, 448]\n ldp q30, q31, [x9, 480]\n"
                "ldr x0, [x9, 8]\n mov x2, xzr\n msr nzcv, xzr\n"
                "blr %4\n"
                "str q2, [%0]\n mov %1, x2\n mrs %2, nzcv\n"
                : : "r"(out), "r"(&x2), "r"(&nzcv), "r"(regs[s]), "r"(stub)
                : "x0", "x2", "x9", "x30", "cc", "memory",
                  "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11",
                  "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21",
                  "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
            for (int b = 0; b < 16; ++b) h = (h ^ out[b]) * 16777619u;
            h = (h ^ (uint32_t)x2) * 16777619u; h = (h ^ (uint32_t)(x2 >> 32)) * 16777619u;
            h = (h ^ (uint32_t)(nzcv >> 28)) * 16777619u;
        }
        printf("%08x %d %08x\n", stub[0], ill, ill ? 0 : h);
    }
    return 0;
}
"""


def words(limit):
    out = []
    for free in range(1 << 19):
        hi, lo = free >> 15, free & 0x7FFF  # bits 31:28 and 24:10
        out.append((hi << 28) | (0b111 << 25) | (lo << 10) | (0 << 5) | 2)
        if limit and len(out) >= limit:
            break
    return out


def build(out_dir, ws):
    asm = out_dir / "stubs.S"
    with open(asm, "w") as fh:
        fh.write(".text\n.p2align 3\n.globl cov_stubs\ncov_stubs:\n")
        for w in ws:
            fh.write(".inst 0x%08x\nret\n" % w)
        fh.write(".section .rodata\n.globl cov_count\n.p2align 2\ncov_count: .word %d\n" % len(ws))
    (out_dir / "runner.c").write_text(RUNNER)
    exe = out_dir / "coverage.elf"
    subprocess.run([str(CROSS / "aarch64-linux-musl-gcc"), "-static", "-O2", "-o", str(exe),
                    str(out_dir / "runner.c"), str(asm)], check=True)
    return exe


def run(cmd, path, env=None):
    with open(path, "w") as fh:
        subprocess.run(cmd, stdout=fh, check=True, env=env)
    result = {}
    for line in open(path):
        w, ill, h = line.split()
        result[int(w, 16)] = (ill == "1", h)
    return result


def disassemble(exe, wanted):
    text = subprocess.run([str(CROSS / "aarch64-linux-musl-objdump"), "-d", "--no-show-raw-insn",
                           "-j", ".text", str(exe)], capture_output=True, text=True, check=True).stdout
    raw = subprocess.run([str(CROSS / "aarch64-linux-musl-objdump"), "-d", "-j", ".text", str(exe)],
                         capture_output=True, text=True, check=True).stdout
    names = {}
    for line in raw.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)", line)
        if m:
            w = int(m.group(1), 16)
            if w in wanted and w not in names:
                names[w] = m.group(2).split("//")[0].strip()
    return names


def shape(insn, word):
    """Mnemonic plus operand arrangement, without register numbers.
    Undefined words are grouped by bits 31:24, 23:21 and 15:10."""
    if insn == "?" or "undefined" in insn:
        return "undefined %02x/%x/%02x" % (word >> 24, (word >> 21) & 7, (word >> 10) & 63)
    insn = re.sub(r"\b[vqdshbxw](\d+)\b", lambda m: m.group(0)[0], insn)
    insn = re.sub(r"#0x[0-9a-f]+|#-?[0-9.e+]+", "#imm", insn)
    return re.sub(r"\[\d+\]", "[i]", insn)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emu", type=Path, default=ROOT / "build/release-sdl1-gl1/bifrost-emu")
    ap.add_argument("--out", type=Path, default=ROOT / "build/simd-coverage")
    ap.add_argument("--limit", type=int, default=0, help="only the first N words (quick check)")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    ws = words(args.limit)
    exe = build(args.out, ws)
    qemu = run(["qemu-aarch64", str(exe)], args.out / "qemu.txt")
    profile = args.out / "fallback.txt"
    if profile.exists():
        profile.unlink()
    env = dict(os.environ, BIFROST_FALLBACK_PROFILE=str(profile))
    bif = run([str(args.emu), str(exe)], args.out / "bifrost.txt", env)
    interpreted = set()
    if profile.exists():
        for line in open(profile):
            parts = line.split()
            if len(parts) >= 4 and re.fullmatch(r"[0-9a-f]{8}", parts[3]):
                interpreted.add(int(parts[3], 16))
    buckets = collections.defaultdict(list)
    for w in ws:
        (q_ill, q_hash), (b_ill, b_hash) = qemu[w], bif[w]
        if not q_ill and b_ill:
            buckets["missing"].append(w)
        elif q_ill and not b_ill:
            buckets["accepted"].append(w)
        elif not q_ill and q_hash != b_hash:
            buckets["mismatch"].append(w)
        elif not q_ill and w in interpreted:
            buckets["interpreted"].append(w)
    valid = sum(1 for w in ws if not qemu[w][0])
    flagged = set(w for b in buckets.values() for w in b)
    names = disassemble(exe, flagged)
    lines = ["SIMD/FP coverage: %d words, %d valid under QEMU" % (len(ws), valid)]
    for bucket in ("missing", "accepted", "mismatch", "interpreted"):
        found = buckets[bucket]
        lines.append("")
        lines.append("== %s: %d words" % (bucket, len(found)))
        groups = collections.defaultdict(list)
        for w in found:
            groups[shape(names.get(w, "?"), w)].append(w)
        for key, members in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            lines.append("%6d  %-48s e.g. %08x" % (len(members), key, members[0]))
    report = "\n".join(lines) + "\n"
    (args.out / "report.txt").write_text(report)
    print(report if len(report) < 20000 else report[:20000] + "\n... (see %s)" % (args.out / "report.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
