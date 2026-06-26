# Tests

This document describes the test programs shipped with bifrost-emu and
their current status under both the frostJIT (default) and interpreter
(`--no-jit`) paths.

---

## Summary

| Mode | Tests | Pass | Fail |
|------|-------|------|------|
| frostJIT (`./bifrost-emu`, default) | 36 | 36 | 0 |
| Interpreter (`./bifrost-emu --no-jit`) | 36 | 36 | 0 |

All 36 JIT test programs pass under frostJIT as of beta.3 (2026-06-26),
and the `ctest/jit_*.elf` regression suite also passes under the
interpreter to catch decoder drift. JIT is the default execution mode
(6.4x speedup on compute workloads). This release fixed 9 critical
int↔FP conversion bugs (SCVTF/UCVTF/FCVTZS/FCVTZU) that were the root
cause of `strtod("-inf")` returning `-nan`. A new 36-case ctest
(`ctest/jit_int_fp_conv.c`) covers all 8 variants of int↔FP conversion
to prevent regression. See CHANGELOG.md for details.

---

## `test/` — assembled AArch64 sources

Small hand-written assembly programs that exercise specific instruction
paths. These are the original smoke tests from the v1.0 release.

| Binary | Status | Verified output |
|--------|--------|-----------------|
| `test/hello.elf` | ✅ Works | `Hello, ARM64!` |
| `test/count.elf` | ✅ Works | `1` (start of 1–6 count loop) |
| `test/fib.elf` | ✅ Works | `832040` (= fib(30)) |
| `test/cat.elf` | ✅ Works | Concatenates `argv[1]` to stdout |
| `test/echo.elf` | ✅ Works | Interactive char-by-char echo (exits on `q`) |
| `test/repl.elf` | ✅ Works | Line-buffered REPL (`got: <line>`) |
| `test/extr.elf` | ✅ Works | Prints `OK` (verifies EXTR instruction end-to-end) |

---

## `ctest/` — musl-static C test programs

C programs cross-compiled with the bundled musl toolchain. These exercise
musl libc code paths (printf, malloc, qsort, FP arithmetic, framebuffer).

| Binary | Status | Verified output |
|--------|--------|-----------------|
| `ctest/hello.elf` | ✅ Works | `Hello, ARM64!` |
| `ctest/loop.elf` | ✅ Works | `Loop value is: 55` (for-loop + `printf("%d")`) |
| `ctest/test_float.elf` | ✅ Works | `3.140000` — `printf("%f")` works as of v1.4.0-alpha |
| `ctest/test_malloc.elf` | ✅ Works | `malloc test start` / `first=1 last=100` / `malloc test done` |
| `ctest/test_fb.elf` | ✅ Works | Queries `FBIOGET_VSCREENINFO`/`FSCREENINFO`, mmaps `/dev/fb0`, draws a gradient |

### `ctest/jit_*.elf` — JIT-specific test suites

These programs exercise instruction patterns that the JIT handles
natively. They are the regression suite for frostJIT codegen changes.

| Binary | Interp | JIT | Notes |
|--------|--------|-----|-------|
| `ctest/jit_addsub_imm.elf` | ✅ | ✅ | ADD/SUB immediate forms |
| `ctest/jit_bitfield.elf` | ✅ | ✅ | SBFM/UBFM/BFM bitfield ops |
| `ctest/jit_block_split.elf` | ✅ | ✅ | Block splitting at CALL_INTERP boundaries (fixed in beta.2) |
| `ctest/jit_carry.elf` | ✅ | ✅ | ADCS/SBCS carry chain |
| `ctest/jit_cls.elf` | ✅ | ✅ | CLS (count leading sign bits) |
| `ctest/jit_csel.elf` | ✅ | ✅ | CSEL/CSINC/CSINV/CSNEG |
| `ctest/jit_extend.elf` | ✅ | ✅ | SXTB/SXTH/SXTW/UXTB/UXTH/UXTW |
| `ctest/jit_fp_scalar.elf` | ✅ | ✅ | FP scalar ops (25/25 sub-tests pass after FCMP/FABS/FNEG/FSQRT/FMOV-imm decode fixes) |
| `ctest/jit_int_fp_conv.elf` | ✅ | ✅ | int↔FP conversions: SCVTF/UCVTF/FCVTZS/FCVTZU × 32/64-bit GPR × single/double FP (36/36 sub-tests, added beta.3) |
| `ctest/jit_ldp_stp.elf` | ✅ | ✅ | LDP/STP pair load/store |
| `ctest/jit_madd.elf` | ✅ | ✅ | MADD/MSUB/SMADDL/UMADDL/SMULH/UMULH |
| `ctest/jit_rev.elf` | ✅ | ✅ | REV/REV16/REV32/RBIT |
| `ctest/jit_simd.elf` | ✅ | ✅ | SIMD logical/DUP/MOVI/LD1/ST1 |

---

## `ctest_real/` — real-world Unix utilities

Real Unix utilities cross-compiled with musl-static at `-O2`. These are
the integration tests — they exercise the full syscall surface, musl
libc, and complex control flow.

| Binary | Interp | JIT | Verified output |
|--------|--------|-----|-----------------|
| `ctest_real/cat.elf` | ✅ | ✅ | Unix `cat` — concatenates files, exercises `readv`/`writev` |
| `ctest_real/wc.elf` | ✅ | ✅ | Unix `wc` — line/word/byte counters |
| `ctest_real/head.elf` | ✅ | ✅ | Unix `head` — `-n N` flag, multi-file |
| `ctest_real/tr.elf` | ✅ | ✅ | Unix `tr` — translate / `-d` delete |
| `ctest_real/rev.elf` | ✅ | ✅ | Unix `rev` — line reversal |
| `ctest_real/sort.elf` | ✅ | ✅ | Unix `sort` — `qsort`, `realloc`, `-r` |
| `ctest_real/sh.elf` | ✅ | ✅ | Interactive REPL shell (`help`/`echo`/`eval`/`exit`) |
| `ctest_real/fib.elf` | ✅ | ✅ | `fib(30) = 832040`, `fib(40) = 102334155`. ~140 MIPS on the interpreter |
| `ctest_real/yes.elf` | ✅ | ✅ | Unix `yes` — ~150M lines/sec through the emulator |
| `ctest_real/fgets_test.elf` | ✅ | ✅ | Line-buffered stdin via `fgets` (exercises the `readv` fix) |
| `ctest_real/audio_test.elf` | ✅ | ✅ | Writes 1 second of 440Hz sine wave to `/dev/dsp` |
| `ctest_real/bench_mips.elf` | ✅ | ✅ | MIPS benchmark loop |
| `ctest_real/jit_new_ops.elf` | ✅ | ✅ | UDIV/SDIV/SMULH/UMULH/SMADDL/FABS/FNEG/FSQRT/FCVT |

---

## `ctest_real/toybox` — toybox multi-call binary

The full toybox-aarch64 static binary (841KB). Tests real-world binary
compatibility. Run commands via `./bifrost-emu ctest_real/toybox <cmd>`.

### Tested commands (37 total)

| Command | Interp | JIT | Notes |
|---------|--------|-----|-------|
| `echo` | ✅ | ✅ | `echo hello world` |
| `true` | ✅ | ✅ | Exit 0 |
| `false` | ✅ | ✅ | Exit 1 |
| `hostname` | ✅ | ✅ | `arm64-emu` |
| `uname` | ✅ | ✅ | `Linux` |
| `whoami` | ✅ | ✅ | Current user |
| `arch` | ✅ | ✅ | `aarch64` |
| `basename` | ✅ | ✅ | `basename /a/b/c` → `c` |
| `dirname` | ✅ | ✅ | `dirname /a/b/c` → `/a/b` |
| `pwd` | ✅ | ✅ | `/` |
| `env` | ✅ | ✅ | Prints `PATH=...` |
| `date` | ✅ | ✅ | `Thu Jun 25 05:45:28 UTC 2026` |
| `printf` | ✅ | ✅ | `printf '%d %s\n' 42 hello`, `printf '%g\n' 3.14` (3.14) |
| `head` | ✅ | ✅ | `head -n 3 /etc/hostname` |
| `tail` | ✅ | ✅ | `tail -n 1 /etc/hostname` |
| `cat` | ✅ | ✅ | `cat /etc/hostname` |
| `wc` | ✅ | ✅ | `wc /etc/hostname` → `1 1 33` |
| `sort` | ✅ | ✅ | `sort /etc/hostname` |
| `cut` | ✅ | ✅ | `cut -d: -f1 /etc/hostname` |
| `cksum` | ✅ | ✅ | `cksum /etc/hostname` → `2980571616 33` |
| `factor` | ✅ | ✅ | `factor 60` → `60: 2 2 3 5` |
| `ls` | ✅ | ✅ | `ls /` |
| `cal` | ✅ | ✅ | June 2026 calendar |
| `xxd` | ✅ | ✅ | Hex dump of `/etc/hostname` |
| `sleep` | ✅ | ✅ | `sleep 0.1` |
| `sh -c` | ❌ | ❌ | `sh -c 'echo hi'` — currently aborts with `UnmappedMemory` (regression). The standalone `sh.elf` (ctest_real/sh.elf) works fine. |
| `rev` | ✅ | ✅ | `rev <<< "hello"` → `olleh` |
| `od` | ✅ | ✅ | `od /etc/hostname` (was SIMD decode error, fixed in beta.3) |
| `seq` | ✅ | ✅ | `seq 1 5` → `1 2 3 4 5`. All variants work: `-w`, `-s`, `-f`, negative steps, float steps. (Was broken: SCVTF misdecoded as FMOV + FMADD operand bug.) |
| `tr` | N/A | N/A | Not in this toybox build (use `ctest_real/tr.elf` instead — works) |
| `expr` | N/A | N/A | Not in this toybox build |

**Toybox summary**: 28 of the 29 tested commands pass; `sh -c` is the
only regression (UnmappedMemory abort — likely a stack-pointer or
signal-frame issue specific to toybox's `sh` binary). `tr` and `expr`
are not in this toybox build. `seq`, `od`, `printf "%g"`, `ls /`,
`factor`, `cksum`, `cal`, `xxd` and many more all work.

---

## Test runner

Run the full test suite with:

```bash
make test
```

This runs every `.elf` file under `test/`, `ctest/`, and `ctest_real/`
under the JIT (the default mode), with stdin redirected from `/dev/null`
so interactive programs don't block. Interactive programs (`echo.elf`,
`repl.elf`, `sh.elf`, `fgets_test.elf`, `cat.elf`) and the infinite
`yes.elf` are skipped — run them by hand. The `ctest/jit_*.elf`
regression suite is then re-run under the interpreter (`--no-jit`) to
catch decoder/interpreter drift. Each test has a 10-second timeout.

For JIT divergence checking (slow, but catches codegen bugs):

```bash
make verify
```

This runs each `ctest/jit_*.elf` under `BIFROST_JIT_VERIFY=1`, which
runs each block through both the JIT and the interpreter and compares
CPU state. Any divergence is logged.

---

## Adding a new test

1. Write a C program (e.g. `ctest/my_test.c`) or assembly source
   (`test/my_test.s`).
2. Cross-compile with the musl toolchain:
   ```bash
   make cross SRC=ctest/my_test.c OUT=ctest/my_test.elf
   ```
3. Run it (JIT is the default; `--no-jit` opts out):
   ```bash
   ./bifrost-emu ctest/my_test.elf
   ./bifrost-emu --no-jit ctest/my_test.elf
   ```
4. Add an entry to the appropriate table above.

For JIT-specific tests, prefix the name with `jit_` so `make verify`
picks it up automatically.
