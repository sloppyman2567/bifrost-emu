#!/usr/bin/env python3
"""sequence_fuzz.py — differential fuzzing of random AArch64 instruction sequences.

The JIT and the interpreter share one decoder and some semantic helpers, so `make verify` (JIT against interpreter)
cannot see a bug they share. This compares both engines with an independent reference, qemu-aarch64, on random
straight-line sequences, which is also where the JIT's register allocation, SSA, peepholes and flag elision act.

Instructions are random words from the three A64 encoding groups that neither access memory nor branch:
data processing (immediate), data processing (register) and scalar FP / Advanced SIMD. Register fields are biased
towards a few registers so instructions depend on each other; x28 is never named (it is the harness's base).
Every word is first probed on its own, and only words that QEMU and Bifrost both execute enter the pool (words only
QEMU executes are listed as missing, not as mismatches).

A case is a sequence of 1..--max-length pool words plus a random machine state: x0..x30, SP, NZCV, FPSR, FPCR (with
--fpcr: rounding mode, flush-to-zero, default NaN) and v0..v31 (integer patterns, FP16/32/64 special values).
A batch of cases is one static guest ELF (each case: load the state, run the sequence, store the state). It runs
under QEMU, the Bifrost JIT and the Bifrost interpreter, and every register is compared. A mismatch is classified
  jit       the JIT differs from QEMU, the interpreter agrees with QEMU
  shared    both engines agree with each other and differ from QEMU
  split     JIT, interpreter and QEMU all differ
  signal    an engine raised SIGILL/SIGSEGV for a sequence the others ran
then shrunk to the fewest instructions that still mismatch, written to OUT/failures/*.json and summarized in
OUT/report.txt, grouped by the minimized sequence's shape. `--replay FILE` reruns one case and prints its
differences. Exit status: 0 when nothing mismatched, 1 otherwise. QEMU is a reference, not the Arm specification:
check a mismatch against the Arm pseudocode before calling it Bifrost's.

Usage:
    python3 Scripts/Testing/sequence_fuzz.py [--emu PATH] [--seed N] [--batches N] [--cases N] [--max-length N]
                                             [--groups imm,reg,simd] [--fpcr] [--ignore-fpsr MASK] [--jobs N]
    python3 Scripts/Testing/sequence_fuzz.py --replay build/sequence-fuzz/failures/0001.json
"""
import argparse
import collections
import concurrent.futures
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CROSS = ROOT / "Tools/aarch64-linux-musl-cross/bin"

# A64 top-level encoding groups (Arm ARM, "A64 instruction set encoding", op0 = bits 28:25).
GROUPS = {
    "imm": (0x1C000000, 0x10000000),   # 100x: data processing, immediate
    "reg": (0x0E000000, 0x0A000000),   # x101: data processing, register
    "simd": (0x0E000000, 0x0E000000),  # x111: scalar FP and Advanced SIMD data processing
}
# Rd, Rn, Ra/Rt2 and Rm: the only register fields in these groups.
REGISTER_SHIFTS = (0, 5, 10, 16)
BASE = 28  # holds the case's state block while the sequence runs

# struct fz_state in the guest: v[32][16], x[31], sp, nzcv, fpcr, fpsr, padding to 16.
V_OFF, X_OFF = 0, 512
SP_OFF, NZCV_OFF, FPCR_OFF, FPSR_OFF = 760, 768, 776, 784
STATE_SIZE = 800
FPSR_CUMULATIVE = 0x0800009F  # QC, IDC, IXC, UFC, OFC, DZC, IOC
IEEE_FLAGS = 0x9F  # IDC, IXC, UFC, OFC, DZC, IOC
FPCR_CHOICES = (0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, 1 << 25, (1 << 24) | (1 << 25), (3 << 22) | (1 << 24))

RUNNER = r"""
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fz_state { uint8_t v[32][16]; uint64_t x[31], sp, nzcv, fpcr, fpsr, pad; };
struct fz_case { struct fz_state in, out; };
extern struct fz_case fz_cases[];
extern void (*const fz_entries[])(struct fz_case *);
extern const uint32_t fz_count;
static sigjmp_buf trap;
static void on_signal(int sig) { siglongjmp(trap, sig); }
static uint32_t fnv(uint32_t h, const void *data, size_t size) {
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
int main(int argc, char **argv) {
    static uint8_t alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0};
    sigaltstack(&ss, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    setvbuf(stdout, 0, _IOLBF, 0);  /* a case that kills the process stays visible */
    int dump = argc > 1 ? atoi(argv[1]) : -1;
    for (uint32_t i = 0; i < fz_count; ++i) {
        if (dump >= 0 && i != (uint32_t)dump) continue;
        struct fz_case *c = &fz_cases[i];
        int sig = sigsetjmp(trap, 1);
        if (sig) {
            __asm__ volatile("msr fpcr, xzr");
            printf("%u %d 0 0 0 0\n", i, sig);
            continue;
        }
        fz_entries[i](c);
        struct fz_state *o = &c->out;
        if (dump >= 0) {
            for (int r = 0; r < 31; ++r) printf("x%d %016llx\n", r, (unsigned long long)o->x[r]);
            printf("sp %016llx\nnzcv %016llx\nfpcr %016llx\nfpsr %016llx\n", (unsigned long long)o->sp,
                   (unsigned long long)o->nzcv, (unsigned long long)o->fpcr, (unsigned long long)o->fpsr);
            for (int r = 0; r < 32; ++r) {
                printf("v%d ", r);
                for (int b = 15; b >= 0; --b) printf("%02x", o->v[r][b]);
                printf("\n");
            }
            continue;
        }
        uint32_t hx = fnv(2166136261u, o->x, sizeof(o->x));
        hx = fnv(hx, &o->sp, sizeof(o->sp));
        uint32_t hv = fnv(2166136261u, o->v, sizeof(o->v));
        printf("%u 0 %08x %08x %llx %llx\n", i, hx, hv, (unsigned long long)o->nzcv, (unsigned long long)o->fpsr);
    }
    return 0;
}
"""

# FP values that sit on the edges of the conversions, comparisons and NaN rules.
F16 = (0x0000, 0x8000, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xfe01, 0x0001, 0x03ff, 0x7bff, 0x3555, 0x5640)
F32 = (0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fa00001, 0xffc00123,
       0x00000001, 0x007fffff, 0x7f7fffff, 0x4b000001, 0x3effffff, 0x4f000000, 0xcf000000, 0x5f000000, 0x3f000000)
F64 = (0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x7ff0000000000000,
       0xfff0000000000000, 0x7ff8000000000000, 0x7ff4000000000001, 0xfff8000000000123, 0x0000000000000001,
       0x000fffffffffffff, 0x7fefffffffffffff, 0x4330000000000001, 0x43e0000000000000, 0xc3e0000000000000,
       0x41e0000000000000, 0x3fe0000000000000, 0x3fdfffffffffffff)
INT_EDGES = (0, 1, 0xffffffffffffffff, 0x7fffffffffffffff, 0x8000000000000000, 0x7fffffff, 0x80000000, 0xffffffff,
             0xffffffff80000000, 0x7fff, 0x8000, 0xffff, 0x7f, 0x80, 0xff, 0x100000000)


def random_x(rng, values):
    roll = rng.random()
    if roll < 0.30:
        return rng.getrandbits(64)
    if roll < 0.45:
        return rng.randrange(-256, 256) & (2 ** 64 - 1)
    if roll < 0.62:
        return rng.choice(INT_EDGES)
    if roll < 0.72:
        return 1 << rng.randrange(64)
    if roll < 0.85 and values:
        return rng.choice(values)  # the same value in two registers: equal operands, aliasing in folds
    value = rng.getrandbits(32)
    return value | (0xffffffff00000000 if value >> 31 and rng.random() < 0.5 else 0)


def random_v(rng):
    kind = rng.randrange(6)
    if kind == 0:
        return rng.getrandbits(128).to_bytes(16, "little")
    if kind == 1:
        return b"".join(struct.pack("<H", rng.choice(F16)) for _ in range(8))
    if kind == 2:
        return b"".join(struct.pack("<I", rng.choice(F32) if rng.random() < 0.8 else rng.getrandbits(32))
                        for _ in range(4))
    if kind == 3:
        return b"".join(struct.pack("<Q", rng.choice(F64) if rng.random() < 0.8 else rng.getrandbits(64))
                        for _ in range(2))
    if kind == 4:
        return b"".join(struct.pack("<Q", rng.choice(INT_EDGES)) for _ in range(2))
    return b"".join(struct.pack("<B", rng.choice((0, 1, 0x7f, 0x80, 0xff))) for _ in range(16))


def random_state(rng, fpcr):
    xs = []
    for _ in range(31):
        xs.append(random_x(rng, xs))
    return {
        "x": xs,
        "sp": random_x(rng, xs),
        "nzcv": rng.randrange(16) << 28,
        "fpcr": rng.choice(FPCR_CHOICES) if fpcr else 0,
        "fpsr": rng.getrandbits(32) & FPSR_CUMULATIVE if rng.random() < 0.3 else 0,
        "v": [random_v(rng).hex() for _ in range(32)],
    }


def pick_register(rng):
    roll = rng.random()
    if roll < 0.6:
        return rng.randrange(8)
    if roll < 0.85:
        return rng.choice([r for r in range(8, 31) if r != BASE])
    return 31


def random_word(rng, group):
    mask, value = GROUPS[group]
    word = (rng.getrandbits(32) & ~mask) | value
    for shift in REGISTER_SHIFTS:
        field = (word >> shift) & 31
        if rng.random() < 0.85 or field == BASE:
            word = (word & ~(31 << shift)) | (pick_register(rng) << shift)
    return word


def state_bytes(state):
    data = bytearray(STATE_SIZE)
    for r, hexed in enumerate(state["v"]):
        data[V_OFF + 16 * r:V_OFF + 16 * r + 16] = bytes.fromhex(hexed)
    for r, value in enumerate(state["x"]):
        struct.pack_into("<Q", data, X_OFF + 8 * r, value)
    for offset, key in ((SP_OFF, "sp"), (NZCV_OFF, "nzcv"), (FPCR_OFF, "fpcr"), (FPSR_OFF, "fpsr")):
        struct.pack_into("<Q", data, offset, state[key])
    return bytes(data)


def case_asm(index, words):
    """One case: save the C caller's registers, load the state from x28 = the case, run the words, store the state."""
    lines = [".p2align 2", "fz_case_%d:" % index,
             "adrp x16, fz_save", "add x16, x16, :lo12:fz_save"]
    for i, (a, b) in enumerate(((19, 20), (21, 22), (23, 24), (25, 26), (27, 28), (29, 30))):
        lines.append("stp x%d, x%d, [x16, #%d]" % (a, b, 16 * i))
    lines += ["mov x17, sp", "str x17, [x16, #96]"]
    for i, (a, b) in enumerate(((8, 9), (10, 11), (12, 13), (14, 15))):
        lines.append("stp d%d, d%d, [x16, #%d]" % (a, b, 112 + 16 * i))
    lines.append("mov x%d, x0" % BASE)
    lines += ["ldr x0, [x28, #%d]" % FPCR_OFF, "msr fpcr, x0", "ldr x0, [x28, #%d]" % FPSR_OFF, "msr fpsr, x0",
              "ldr x0, [x28, #%d]" % NZCV_OFF, "msr nzcv, x0", "ldr x0, [x28, #%d]" % SP_OFF, "mov sp, x0"]
    lines += ["ldr q%d, [x28, #%d]" % (r, V_OFF + 16 * r) for r in range(32)]
    lines += ["ldr x%d, [x28, #%d]" % (r, X_OFF + 8 * r) for r in range(31) if r != BASE]
    lines += [".inst 0x%08x" % w for w in words]
    lines.append("add x28, x28, #%d" % STATE_SIZE)
    lines += ["str x%d, [x28, #%d]" % (r, X_OFF + 8 * r) for r in range(31) if r != BASE]
    lines += ["mov x0, sp", "str x0, [x28, #%d]" % SP_OFF, "mrs x0, nzcv", "str x0, [x28, #%d]" % NZCV_OFF,
              "mrs x0, fpcr", "str x0, [x28, #%d]" % FPCR_OFF, "mrs x0, fpsr", "str x0, [x28, #%d]" % FPSR_OFF]
    lines += ["str q%d, [x28, #%d]" % (r, V_OFF + 16 * r) for r in range(32)]
    lines += ["str xzr, [x28, #%d]" % (X_OFF + 8 * BASE), "b fz_epilogue"]
    return lines


EPILOGUE = ["fz_epilogue:", "adrp x16, fz_save", "add x16, x16, :lo12:fz_save", "ldr x17, [x16, #96]", "mov sp, x17"] + \
    ["ldp x%d, x%d, [x16, #%d]" % (a, b, 16 * i)
     for i, (a, b) in enumerate(((19, 20), (21, 22), (23, 24), (25, 26), (27, 28), (29, 30)))] + \
    ["ldp d%d, d%d, [x16, #%d]" % (a, b, 112 + 16 * i) for i, (a, b) in enumerate(((8, 9), (10, 11), (12, 13), (14, 15)))] + \
    ["msr fpcr, xzr", "ret"]


def build(directory, cases):
    """cases: [(words, state)]. Returns the guest ELF."""
    directory.mkdir(parents=True, exist_ok=True)
    asm = [".text"]
    for i, (words, _) in enumerate(cases):
        asm += case_asm(i, words)
    asm += EPILOGUE
    asm += [".data", ".p2align 3", ".globl fz_entries", "fz_entries:"]
    asm += [".quad fz_case_%d" % i for i in range(len(cases))]
    asm += [".globl fz_count", ".p2align 2", "fz_count: .word %d" % len(cases)]
    (directory / "cases.bin").write_bytes(b"".join(state_bytes(state) + bytes(STATE_SIZE) for _, state in cases))
    asm += [".data", ".p2align 4", ".globl fz_cases", "fz_cases:", '.incbin "%s"' % (directory / "cases.bin")]
    asm += [".bss", ".p2align 4", "fz_save: .space 192"]
    (directory / "cases.S").write_text("\n".join(asm) + "\n")
    (directory / "runner.c").write_text(RUNNER)
    exe = directory / "cases.elf"
    # Not position-independent: every engine then runs the code at the same addresses (ADR, ADRP results).
    subprocess.run([str(CROSS / "aarch64-linux-musl-gcc"), "-static", "-no-pie", "-O2", "-o", str(exe),
                    str(directory / "runner.c"), str(directory / "cases.S")], check=True)
    return exe


def engines(emu):
    # Pointer authentication off: QEMU picks random keys per process, so its results would never repeat.
    return {"qemu": ["qemu-aarch64", "-cpu", "max,pauth=off"], "jit": [str(emu)], "interp": [str(emu), "--no-jit"]}


def run(command, exe, timeout, arg=None):
    """Per-case results {index: (signal, hx, hv, nzcv, fpsr)}; a case missing from the map killed the process."""
    try:
        done = subprocess.run(command + [str(exe)] + ([str(arg)] if arg is not None else []), capture_output=True,
                              text=True, timeout=timeout)
        out = done.stdout
    except subprocess.TimeoutExpired as expired:
        out = expired.stdout.decode(errors="replace") if isinstance(expired.stdout, bytes) else (expired.stdout or "")
    if arg is not None:
        return out
    results = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 6 and parts[0].isdigit():
            results[int(parts[0])] = (int(parts[1]), parts[2], parts[3], int(parts[4], 16), int(parts[5], 16))
    return results


def outcome(result, ignore_fpsr):
    if result is None:
        return ("died",)
    sig, hx, hv, nzcv, fpsr = result
    if sig:
        return ("signal", sig)
    return (hx, hv, nzcv, fpsr & ~ignore_fpsr)


def classify(results, index, ignore_fpsr):
    q, j, i = (outcome(results[e].get(index), ignore_fpsr) for e in ("qemu", "jit", "interp"))
    if q == j == i:
        return None
    if any(o[0] in ("signal", "died") for o in (q, j, i)):
        return "signal"
    if i == q:
        return "jit"
    if j == i:
        return "shared"
    return "split"


def run_all(emu, exe, timeout, names=("qemu", "jit", "interp")):
    commands = engines(emu)
    with concurrent.futures.ThreadPoolExecutor(len(names)) as pool:
        futures = {name: pool.submit(run, commands[name], exe, timeout) for name in names}
        return {name: future.result() for name, future in futures.items()}


def survivors(results, count):
    """Cases before the first one an engine's process did not get past (a crash or hang ends the run there)."""
    first = count
    for result in results.values():
        missing = [i for i in range(count) if i not in result]
        if missing:
            first = min(first, missing[0])
    return first


def probe(emu, directory, candidates, rng, timeout):
    """Words QEMU and Bifrost both execute, and words only QEMU executes."""
    cases = [([w], random_state(rng, False)) for w in candidates]
    exe = build(directory, cases)
    results = run_all(emu, exe, timeout, ("qemu", "jit"))
    valid, missing = [], []
    for index, word in enumerate(candidates):
        q, b = results["qemu"].get(index), results["jit"].get(index)
        if q is None or b is None or q[0]:
            continue
        (missing if b[0] else valid).append(word)
    return valid, missing


def build_pool(emu, out, groups, size, rng, timeout):
    pool, missing = {}, []
    for group in groups:
        found = []
        attempt = 0
        while len(found) < size and attempt < 40:
            candidates = [random_word(rng, group) for _ in range(4000)]
            valid, lost = probe(emu, out / "probe" / group, candidates, rng, timeout)
            found += valid
            missing += lost
            attempt += 1
        pool[group] = found[:size]
    return pool, missing


def make_case(rng, pool, groups, max_length, fpcr):
    words = [rng.choice(pool[rng.choice(groups)]) for _ in range(rng.randint(1, max_length))]
    return words, random_state(rng, fpcr)


def differences(emu, directory, words, state, timeout):
    """Per-register values that differ between the engines for one case."""
    exe = build(directory, [(words, state)])
    dumps = {}
    for name, command in engines(emu).items():
        dumps[name] = dict(line.split() for line in run(command, exe, timeout, arg=0).splitlines() if len(line.split()) == 2)
    keys = sorted(set().union(*dumps.values()), key=lambda k: (k[0], int(k[1:]) if k[1:].isdigit() else 99, k))
    return [(k, {name: dumps[name].get(k, "-") for name in dumps}) for k in keys
            if len({dumps[name].get(k) for name in dumps}) > 1]


def shrink(emu, directory, words, state, kind, timeout, ignore_fpsr):
    """Remove instructions while the same kind of mismatch remains (one build per round: every one-shorter variant)."""
    while len(words) > 1:
        variants = [words[:i] + words[i + 1:] for i in range(len(words))]
        exe = build(directory, [(v, state) for v in variants])
        results = run_all(emu, exe, timeout)
        for index, variant in enumerate(variants):
            if classify(results, index, ignore_fpsr) == kind:
                words = variant
                break
        else:
            break
    return words


def disassemble(words):
    with tempfile.TemporaryDirectory() as temporary:
        source, obj = Path(temporary) / "w.S", Path(temporary) / "w.o"
        source.write_text(".text\n" + "".join(".inst 0x%08x\n" % w for w in words))
        subprocess.run([str(CROSS / "aarch64-linux-musl-as"), "-o", str(obj), str(source)], check=True)
        text = subprocess.run([str(CROSS / "aarch64-linux-musl-objdump"), "-d", str(obj)], capture_output=True,
                              text=True, check=True).stdout
    names = []
    for line in text.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+(.*)", line)
        if m:
            names.append(m.group(1).split("//")[0].strip())
    return names


def shape(name):
    """Mnemonic and operand arrangement, without register numbers or immediates."""
    name = re.sub(r"\b[vqdshbxw](\d+|zr)\b", lambda m: m.group(0)[0], name)
    name = re.sub(r"#0x[0-9a-f]+|#-?[0-9.e+]+", "#imm", name)
    return re.sub(r"\[\d+\]", "[i]", name)


def report_failure(emu, out, number, kind, words, state, timeout, origin):
    record = {"kind": kind, "origin": origin, "words": ["%08x" % w for w in words], "state": state}
    failures = out / "failures"
    failures.mkdir(parents=True, exist_ok=True)
    path = failures / ("%04d.json" % number)
    path.write_text(json.dumps(record, indent=1) + "\n")
    names = disassemble(words)
    lines = ["== %s #%d (%s) %s" % (kind, number, origin, path.relative_to(ROOT) if path.is_relative_to(ROOT) else path)]
    lines += ["   %08x  %s" % (w, n) for w, n in zip(words, names)]
    lines.append("   fpcr=%x nzcv=%x fpsr=%x" % (state["fpcr"], state["nzcv"], state["fpsr"]))
    for key, values in differences(emu, out / "diff", words, state, timeout):
        lines.append("   %-5s " % key + "  ".join("%s=%s" % (name, values[name]) for name in ("qemu", "jit", "interp")))
    return lines, " ; ".join(shape(n) for n in names)


def replay(args):
    record = json.loads(Path(args.replay).read_text())
    words = [int(w, 16) for w in record["words"]]
    out = args.out / "replay"
    for w, name in zip(words, disassemble(words)):
        print("%08x  %s" % (w, name))
    found = differences(args.emu, out, words, record["state"], args.timeout)
    for key, values in found:
        print("%-5s " % key + "  ".join("%s=%s" % (name, values[name]) for name in ("qemu", "jit", "interp")))
    print("no differences" if not found else "%d registers differ" % len(found))
    return 1 if found else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emu", type=Path, default=ROOT / "bifrost-emu")
    ap.add_argument("--out", type=Path, default=ROOT / "build/sequence-fuzz")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--batches", type=int, default=10)
    ap.add_argument("--cases", type=int, default=400, help="cases per batch (one guest ELF)")
    ap.add_argument("--max-length", type=int, default=12)
    ap.add_argument("--groups", default="imm,reg,simd")
    ap.add_argument("--pool", type=int, default=3000, help="probed words per group")
    ap.add_argument("--fpcr", action="store_true", help="randomize FPCR rounding mode, FZ and DN")
    # Bifrost keeps FPSR.QC but does not raise the IEEE cumulative exception flags (IOC, DZC, OFC, UFC, IXC, IDC), so by
    # default they are left out; --ignore-fpsr 0 compares them too.
    ap.add_argument("--ignore-fpsr", type=lambda s: int(s, 0), default=IEEE_FLAGS,
                    help="FPSR bits left out of the comparison (default: the IEEE exception flags)")
    ap.add_argument("--max-reports", type=int, default=3, help="reported failures per minimized shape")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 4))
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--replay", type=Path)
    args = ap.parse_args()
    if args.replay:
        return replay(args)
    groups = [g for g in args.groups.split(",") if g]
    rng = random.Random(args.seed)
    args.out.mkdir(parents=True, exist_ok=True)
    pool, missing = build_pool(args.emu, args.out, groups, args.pool, rng, args.timeout)
    print("pool: " + ", ".join("%s %d words" % (g, len(pool[g])) for g in groups) + "; %d probed words only QEMU runs"
          % len(missing), flush=True)
    groups = [g for g in groups if pool[g]]

    def batch(number):
        local = random.Random("%d/%d" % (args.seed, number))
        cases = [make_case(local, pool, groups, args.max_length, args.fpcr) for _ in range(args.cases)]
        exe = build(args.out / ("batch-%d" % number), cases)
        results = run_all(args.emu, exe, args.timeout)
        last = survivors(results, len(cases))
        found = []
        for index, (words, state) in enumerate(cases[:last + 1] if last < len(cases) else cases):
            kind = "signal" if index == last else classify(results, index, args.ignore_fpsr)
            if kind:
                found.append((kind, words, state, "seed %d batch %d case %d" % (args.seed, number, index)))
        return number, found, len(cases) - min(len(cases), last + 1)

    counts = collections.Counter()
    shapes = collections.defaultdict(list)
    report = []
    total = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
        for number, found, skipped in executor.map(batch, range(args.batches)):
            total += args.cases - skipped
            print("batch %d: %d of %d cases mismatched%s" % (number, len(found), args.cases,
                  ", %d not run after a process died" % skipped if skipped else ""), flush=True)
            for kind, words, state, origin in found:
                counts[kind] += 1
                words = shrink(args.emu, args.out / "shrink", words, state, kind, args.timeout, args.ignore_fpsr)
                key = (kind, " ; ".join(shape(n) for n in disassemble(words)))
                shapes[key].append(origin)
                if len(shapes[key]) > args.max_reports:
                    continue
                lines, _ = report_failure(args.emu, args.out, sum(len(v) for v in shapes.values()), kind, words, state,
                                          args.timeout, origin)
                report += lines
    head = ["sequence fuzz: seed %d, %d cases, max length %d, groups %s%s" %
            (args.seed, total, args.max_length, ",".join(groups), ", random FPCR" if args.fpcr else ""),
            "pool: " + ", ".join("%s %d" % (g, len(pool[g])) for g in groups),
            "mismatches: " + (", ".join("%s %d" % kv for kv in sorted(counts.items())) or "none"), ""]
    head += ["%5d  %-6s %s" % (len(v), k[0], k[1]) for k, v in sorted(shapes.items(), key=lambda kv: -len(kv[1]))]
    if missing:
        names = collections.Counter(shape(n) for n in disassemble(sorted(set(missing))[:4000]))
        head += ["", "probed words QEMU runs and Bifrost refuses (SIGILL), by shape:"]
        head += ["%5d  %s" % (n, s) for s, n in names.most_common(40)]
    text = "\n".join(head + [""] + report) + "\n"
    (args.out / "report.txt").write_text(text)
    print(text if len(text) < 12000 else text[:12000] + "\n... (see %s)" % (args.out / "report.txt"))
    return 1 if counts else 0


if __name__ == "__main__":
    sys.exit(main())
