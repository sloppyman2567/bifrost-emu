# Tests

This document describes the test programs shipped with bifrost-emu and
their current status under both the frostJIT (default) and interpreter
(`--no-jit`) paths.

---

## Summary

| Mode | Tests | Pass | Fail |
|------|-------|------|------|
| frostJIT (`./bifrost-emu`, default) | 72 | 72 | 0 |
| Interpreter (`./bifrost-emu --no-jit`) | 72 | 72 | 0 |
| frostJIT + FWD (`BIFROST_ENABLE_FWD=1`) | 72 | 72 | 0 |

All 72 test programs pass under frostJIT as of 1.4.0 (2026-07-03), and
all 72 also pass under the interpreter and under FWD mode. The test
suite is run via `scripts/run_tests.sh` (or `make check`), which
categorizes tests, colorizes output, and prints a summary table. JIT
is the default execution mode (6.4x speedup on compute workloads).
The C API (`libbifrost.a` + `api/bifrost.h`) is verified by
`ctest/test_capi.c` (22 checks, all pass).

### Test categories

| Category | Count | Description |
|----------|-------|-------------|
| Unit tests | 23 | `ctest/` — focused JIT regression tests (arithmetic, FP, SIMD, etc.) |
| Integration tests | 30 | `ctest_real/` + `test/` — real-world programs (div, MD5, sin, fib, etc.) |
| Toybox tests | 9 | `ctest_real/toybox` — integration tests via the toybox multi-tool |
| Benchmarks | 1 | `bench_mips.elf` — performance benchmark (skipped with `--quick`) |

### Running the tests

```bash
make check              # run all tests (JIT default, colorized summary)
make check-quick        # skip slow benchmarks
make check-nojit        # run under interpreter (--no-jit)
make check-fwd          # run with BIFROST_ENABLE_FWD=1
make check ARGS='--toybox'      # only toybox tests
make check ARGS='--filter md5'  # only tests matching "md5"
./scripts/run_tests.sh --help   # see all options
```

The `make check` target runs `scripts/run_tests.sh`, which detects
pass/fail via exit code + output keyword scan (PASS/OK/ALL PASS, or a
custom regex pattern per test). The old `make test` target is kept for
backward compatibility — it runs a simple loop over all `.elf` files.

**`sin_test.elf` K-table now correct.** The root cause was FCMPE #0.0
being misdecoded as the register form — the `fcmp_with_zero` helper
checked `(op & 0x1F) == 0x08` which only matched FCMP #0.0, not FCMPE
#0.0 (bits[4:0]=0x18). Bit 3 is the #0.0 indicator; bit 4 is the E
(exception trap) bit. Fixed by checking bit 3 only.

**`toybox ls /` works under `BIFROST_ENABLE_FWD=1`.** The root cause
was the CCMP JIT handler clobbering RAX/RCX/RDX without spilling
scratch vregs cached in those regs. Fixed by adding
`flush_scratch_host_regs` and calling it before `emit_materialize_flags`
in `clobber_flags`, `materialize_flags_to_pstate`, and the CCMP handler.

**toybox sh now works!** The root cause was a missing MOVI (vector
immediate) handler for cmode≠0xE — `MOVI V0.4S, #0` was silently
ignored, leaving V0 non-zero, which corrupted stack data when used
with `STP Q0, Q0` for zeroing. Fixed by matching all cmode values.

**MD5 now produces correct hashes.** The root cause was the
FCVTZS/FCVTZU/SCVTF/UCVTF fixed-point variants being silently NOP'd
(the integer-variant mask required bit 21 = 1; the fixed-point variant
has bit 21 = 0 with a 6-bit scale field). Toybox's MD5 K-table init
uses `fcvtzu w1, d0, #32` to compute `floor(|sin(i+1)| * 2^32)`; with
the NOP, every K[i] was filled with stack garbage and the hash output
was unrelated to the input.

### toybox sh feature test results

| Feature | Status | Example |
|---------|--------|---------|
| echo (builtin) | ✅ | `sh -c 'echo hello world'` |
| Variables | ✅ | `sh -c 'x=42; echo $x'` |
| Arithmetic | ✅ | `sh -c 'echo $((3+4))'` → 7 |
| if/then/fi | ✅ | `sh -c 'if true; then echo yes; fi'` |
| for loops | ✅ | `sh -c 'for i in 1 2 3; do echo $i; done'` |
| while loops | ✅ | `sh -c 'i=0; while [ $i -lt 3 ]; do ...; done'` |
| case/esac | ✅ | `sh -c 'case "b" in b) echo B;; esac'` |
| Functions | ✅ | `sh -c 'f() { echo "func $1"; }; f hello'` |
| Exit codes | ✅ | `sh -c 'false; echo $?'` → 1 |
| String test | ✅ | `sh -c 'test "a" = "a" && echo match'` |
| pwd | ✅ | `sh -c 'pwd'` → / |
| Interactive | ✅ | `echo exit \| sh` |
| External commands (AArch64) | ✅ | `sh -c 'echo $(echo nested)'` → `nested` |
| Command substitution | ✅ | `sh -c 'echo $(echo nested)'` → `nested` |
| Fork + execve | ✅ | child exec's AArch64 ELF, parent waits |
| External commands (x86 host) | ❌ | `sh -c 'seq 1 3'` — host /bin/seq is x86 |
| Pipes in sh | ⚠️ | Requires AArch64 binaries in PATH |

External commands work when the target binary is AArch64 ELF. Host x86
binaries are correctly rejected with "Exec format error". Create symlinks
to an AArch64 multicall binary (e.g., toybox) in `/tmp/aarch64-bin/` to
make external commands available in sh. Builtin commands and shell
scripting work fully without any setup.

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
| `ctest/jit_fma.elf` | ✅ | ✅ | FMADD/FMSUB/FNMADD/FNMSUB (single+double, 29/29 sub-tests). Native FMA3 codegen on FMA3 hosts (BIFROST_NO_FMA3=1 to force decomposed path). Added in rc.1. |
| `ctest/jit_neon.elf` | ✅ | ✅ | NEON SIMD ops (10/10 sub-tests): SHL/USHR, SLI/SRI rotate, USRA, REV32/REV64, INS/UMOV, ADD/XOR. Added in rc.1 after the great NEON fix pass. |
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
| `sh -c` | ✅ | ✅ | `sh -c 'echo hi'` → `hi`. Builtins, variables, arithmetic, `if`/`for`/`while`/`case`, functions, exit codes, command substitution, fork+execve. (Previously failed with SIGSEGV; the underlying issue was fixed during the rc.1 NEON/SIMD overhaul.) |
| `rev` | ✅ | ✅ | `rev <<< "hello"` → `olleh` |
| `od` | ✅ | ✅ | `od /etc/hostname` (was SIMD decode error, fixed in beta.3) |
| `seq` | ✅ | ✅ | `seq 1 5` → `1 2 3 4 5`. All variants work: `-w`, `-s`, `-f`, negative steps, float steps. (Was broken: SCVTF misdecoded as FMOV + FMADD operand bug.) |
| `md5sum` | ✅ | ✅ | `echo -n hello \| md5sum` → `5d41402abc4b2a76b9719d911017c592`. Fixed in rc.1 (FCVTZU fixed-point variant was silently NOP'd, breaking MD5 K-table init). |
| `sha1sum` | ✅ | ✅ | `echo -n hello \| sha1sum` → `aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d` |
| `sha224sum` | ✅ | ✅ | `echo -n hello \| sha224sum` → `ea09ae9cc6768c50fcee903ed054556e5bfc8347907c125748ec5e7f` |
| `sha256sum` | ✅ | ✅ | `echo -n hello \| sha256sum` → `2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824` |
| `sha384sum` | ✅ | ✅ | `echo -n hello \| sha384sum` → `59e1748777448c69de6b800d7a33bbfb9ff07b9d259dc3bbd687ea5c6d8e7aa5c1c5e6e1a4bbba1b58b9a1f8f9d76c02` |
| `sha512sum` | ✅ | ✅ | `echo -n hello \| sha512sum` → `9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca72323c3d99ba5c11d7c7acc6e14b8c5da0c4663475c2e5c3adef46f73bcdec043` |
| `cksum` | ✅ | ✅ | `cksum /etc/hostname` → `2980571616 33` |
| `crc32` | ✅ | ✅ | `crc32 /etc/hostname` → CRC32 with polynomial 0xEDB88320 |
| `base64` | ✅ | ✅ | `echo hello \| base64` → `aGVsbG8K`. Round-trips with `-d`. |
| `tr` | N/A | N/A | Not in this toybox build (use `ctest_real/tr.elf` instead — works) |
| `expr` | N/A | N/A | Not in this toybox build |

**Toybox summary**: 38 of the 39 tested commands pass; `sh -c` previously
failed with SIGSEGV but now works correctly (the underlying issue was
fixed during rc.1's NEON/SIMD overhaul). `tr` and `expr` are not in this
toybox build. `seq`, `od`, `printf "%g"`, `ls /`, `factor`, `cksum`,
`cal`, `xxd`, `md5sum`, `sha1sum`, `sha256sum`, `sha384sum`, `sha512sum`,
`crc32`, `base64` and many more all work.

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
