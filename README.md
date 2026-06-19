# bifrost-emu

**A bridge between worlds** — a fast, simple ARM64 (AArch64) Linux user-mode
emulator written in C++17. Runs static AArch64 ELF binaries on any x86_64
Linux host without needing qemu or a cross-compiler.

```
  ____ _____ ______ _____   ____   _____ _______ 
 |  _ \_   _|  ____|  __ \ / __ \ / ____|__   __|
 | |_) || | | |__  | |__) | |  | | (___    | |   
 |  _ < | | |  __| |  _  /| |  | |\___ \   | |   
 | |_) || |_| |    | | \ \| |__| |____) |  | |   
 |____/_____|_|    |_|  \_\\____/|_____/   |_|   

  bifrost-emu  v1.4.0-alpha
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.4.0-alpha](https://img.shields.io/badge/version-1.4.0--alpha-orange.svg)](CHANGELOG.md)

## Quick Start

```bash
# Build (requires only g++ and the standard library)
make

# Run a static ARM64 binary — silent by default, just shows program output
./bifrost-emu hello.elf

# Debug mode — trace every instruction to stderr
./bifrost-emu -d hello.elf

# Verbose — show execution stats on exit
./bifrost-emu -v hello.elf

# Interactive apps work too (raw TTY mode is auto-enabled)
./bifrost-emu echo.elf

# Pass arguments to the emulated program
./bifrost-emu cat.elf /etc/hostname

# Show version
./bifrost-emu --version
```

No args? You get the banner. Try `--bifrost` for a hidden easter egg.

## Architecture

```
┌──────────────────────────┐         ┌───────────────────────┐
│    decoder.cpp           │         │  interpreter.cpp      │
│                          │         │                       │
│  Hierarchical switch:    │         │  switch(d.cls) {      │
│   outer: bits[28:24]     │ ──────► │    case ADD_IMM:      │
│   inner: group-specific  │         │      res = a + b;     │
│  ──────────────────────  │         │      break;           │
│  - field extraction      │         │    case LDR_IMM:      │
│  - InstClass             │         │      ...              │
│  - SP/XZR disambig       │         │    ...                │
│  - shift calc            │         │  }                    │
│  - addressing            │         │                       │
│                          │         │  EXECUTE ONLY         │
│  NO execution            │         │  - reads d.* fields   │
│                          │         │  - no bit extraction  │
└──────────────────────────┘         └───────────────────────┘
         ▲
         │
┌────────┴─────────────┐
│  decode_cache_       │
│  PC → DecodedInst    │
│  (avoids re-decode)  │
└──────────────────────┘
```

The **decoder** (`decoder.cpp`) is the single source of truth for
instruction decode. It is a true two-level hierarchical switch:

- **Outer switch** on bits `[28:24]` — the 5-bit "major encoding group"
  selector from the ARM ARM top-level encoding table. This routes each
  instruction to one of 32 cases (most reserved), preventing the
  encoding collisions that plague flat mask-and-compare decoders.
- **Inner switch** on the group-specific discriminator — typically
  bits `[31:29]` (opc/sf), bit 26 (V), bit 23, bit 22, the addressing-
  mode bits, or a sub-opcode field, depending on the group.

The decoder extracts all fields into a `DecodedInst` struct and
classifies the instruction into an `InstClass` enum value. It does NO
execution — only bit extraction and classification. B/BL are pulled
out before the outer switch because their discriminator is bits
`[30:26]`, not bits `[28:24]` (imm26 leaks into bits[28:24]).

The **interpreter** (`interpreter.cpp`) dispatches on `d.cls` via a
flat `switch` statement. Each case reads `d.*` fields and executes.
The interpreter never does bit extraction (`op >> 22`, `(op & mask)`,
etc.) — that's the decoder's job. The legacy if-chain was deleted in
v1.3.0-beta.3; v1.4.0-alpha restructured the decoder itself from a
flat if-chain into the hierarchical switch described above.

A **decode cache** (`PC → DecodedInst`) avoids re-decoding the same
instruction on repeated execution (tight loops). Since guest code is
not self-modifying (static binaries only), each PC always decodes to
the same instruction.

### Hierarchical Decoder — Structural Properties

The hierarchical structure prevents several long-standing encoding
collisions by construction (no test ordering required):

- **STP/LDP pre-index vs ORR** — STP/LDP pre-index V=0 lives in outer
  case `0x09`; logical shifted register lives in outer case `0x0A`.
  They cannot collide regardless of test order. (v0 had this collision
  fixed only by careful if-chain ordering; the fix was fragile.)
- **LSE atomics vs LDUR/STUR** — both live in outer case `0x18`, but
  the inner switch dispatches on bit 21 + bits[11:10] mode, with an
  explicit V (bit 26) check for LSE atomics (LSE requires V=0).
- **EXTR vs SBFM/BFM/UBFM** — both live in outer case `0x13`, but the
  inner check on bit 23 routes EXTR (bit 23 = 1) before the bitfield
  opc switch. (v0 had a bug where the bitfield check ignored bit 23
  and came first, making EXTR unreachable. v1.4.0-alpha fixes this.)

### Decoder Switch — Complete Coverage

All instruction groups are now classified by the decoder and dispatched
by the interpreter's switch. There is no fallback if-chain.

**Branches** — B, BL, Bcond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET

**System** — SVC, BRK, HLT, MRS, MSR, CLREX, HINT (NOP/WFE/WFI/SEV/YIELD/DSB/DMB/ISB)

**Data processing — immediate** — ADR, ADRP, MOVN/MOVZ/MOVK, ADD/SUB/ADDS/SUBS
immediate, SBFM/BFM/UBFM, EXTR, AND/ORR/EOR/ANDS immediate

**Data processing — register** — ADD/SUB/ADDS/SUBS (shifted + extended register),
ADC/ADCS/SBC/SBCS, AND/ORR/EOR/ANDS (shifted register), CSEL/CSINC/CSINV/CSNEG,
CCMP/CCMN, RBIT/REV16/REV32/REV/CLZ/CLS, UDIV/SDIV/LSL/LSR/ASR/ROR,
MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH

**Load/Store** — LDR/STR (unsigned immediate, unscaled LDUR/STUR, post-index,
pre-index, register-offset), LDP/STP (post/offset/pre), LDRSB/LDRSH/LDRSW
(sign-extend forms), SIMD LDR/STR (B/H/S/D/Q forms, 128-bit Q form)

**Atomics** — LDXR/STXR/LDAXR/STLXR (exclusive monitor), LDAR/STLR
(acquire/release), LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/
SWP/CAS — sub-dispatched by `atom_op` field). LSE atomics honor the ELF's
`GNU_PROPERTY_AARCH64_FEATURE_1_LSE` flag: if the binary doesn't declare LSE,
the bit-21=1 encoding is treated as LDUR/STUR (matching real hardware).

**SIMD/FP** — FMOV (general/scalar/immediate/Vd.D[1]), FADD/FSUB/FMUL/FDIV/
FMAX/FMIN/FNMUL, FABS/FNEG/FSQRT/FRINTN/P/M/Z/A/X/I, FCVT (S↔D), FCMP/FCMPE,
FCVTZS/FCVTZU, SCVTF/UCVTF, FCSEL, FMADD/FMSUB, SIMD LD1/ST1, DUP, INS,
ORR/AND/EOR/BIC (vector), EXT, REV16/REV32/REV64, CNT, UADDLV, CMEQ,
MOVI, SHL/USHR/SHRN (vector immediate), UMAXP/UMINP/SMAXP/SMINP, CMHS, TBL/TBX

## Performance

~140 MIPS on a typical desktop (compute-heavy workload), a 3.8x
speedup over v1.3.0-beta.3 (~37 MIPS). The improvement comes from
two optimizations:

1. **Direct-mapped decode cache** — replaced `std::unordered_map` (hash
   + 88-byte struct copy per instruction) with a flat 4096-entry array
   (index + tag compare + const reference). 100% hit rate for tight
   loops.

2. **Memory page cache** — added single-entry last-page caches for read
   and write, avoiding mutex lock + hash-map lookup on every memory
   access to the same page (stack, heap, code — the common case).

The `fib(30)` test runs 258 instructions in under 0.01ms; musl static
hello world runs 1,760 instructions in under 0.1ms. A 10M-iteration
compute loop runs 90M instructions in 0.64s. An experimental JIT
(frostJIT) landed in v1.4.0-alpha — enable with `--jit`; target for
the production JIT is 500+ MIPS.

Run `bifrost-emu -v <elf>` to see MIPS, memory page count, and decode
cache hit rate for any program.

## Design Philosophy

- **Silent by default.** Only the emulated program's output appears.
  No stats, no exit codes, no noise.
- **Debug when you need it.** `-d` traces every instruction. `-v`
  prints stats.
- **No binary-specific hacks.** Zero hardcoded addresses. Any static
  AArch64 ELF should work (modulo the known issues below).
- **Decoder is the single source of truth.** All decode logic lives in
  `decoder.cpp`. The interpreter only reads `d.*` fields and executes.
  The future JIT will share the same decoder.
- **Hang watchdog.** A safety net in the run loop catches infinite
  loops (e.g. `b .` self-branches) and aborts with a diagnostic instead
  of spinning forever. With the UBFM rotation fix in beta.3, mallocng
  no longer hangs — but the watchdog remains as a safety net for any
  future regressions.

## Usage

```
bifrost-emu [options] <elf-file> [args...]

Options:
  -d, --debug     trace every instruction to stderr
  -v, --verbose   print execution stats on exit
  -V, --version   show version and exit
  -h, --help      show help
  --fb-dump PATH  dump the /dev/fb0 framebuffer to PATH on exit (PPM format)
  -q, --quiet     suppress BRK warnings (even with -d)

Examples:
  bifrost-emu hello.elf
  bifrost-emu -d hello.elf
  bifrost-emu cat.elf /etc/hosts
  bifrost-emu -v echo.elf hello
  bifrost-emu -v --fb-dump out.ppm ctest/test_fb.elf

  # Real-world Unix utilities (built with musl-static):
  bifrost-emu ctest_real/cat.elf README.md | head -5
  bifrost-emu ctest_real/wc.elf README.md
  bifrost-emu ctest_real/head.elf -n 10 README.md
  echo "hello" | bifrost-emu ctest_real/rev.elf
  echo "abcabc" | bifrost-emu ctest_real/tr.elf abc ABC
  printf 'banana\napple\ncherry\n' | bifrost-emu ctest_real/sort.elf -r
  bifrost-emu ctest_real/fib.elf 40
  printf 'help\necho hello\neval 6*7\nexit\n' | bifrost-emu ctest_real/sh.elf

  # Pipelines between emulated programs:
  bifrost-emu ctest_real/cat.elf README.md | bifrost-emu ctest_real/wc.elf
  printf 'hello\nworld\n' | bifrost-emu ctest_real/rev.elf \
                          | bifrost-emu ctest_real/tr.elf ol LO \
                          | bifrost-emu ctest_real/head.elf -n 2
```

The `--fb-dump PATH` option syncs the guest's `/dev/fb0` writes back
to the host and writes a PPM image to `PATH` on exit. Useful for
headless debugging of programs that draw to the framebuffer. If the
guest never opened `/dev/fb0`, no PPM is created (no error). View the
PPM with any image viewer, or convert with
`convert out.ppm out.png` (ImageMagick).

## Build

```bash
make          # release build with -O3
make debug    # debug build with ASan + UBSan
make test     # run the test suite
make install  # install to /usr/local/bin/
```

No external libraries required. Only standard C++ and POSIX.

## File Structure

```
bifrost-emu/
├── include/
│   ├── arm64_emu.hpp     Emulator class (CPU, Memory, ELF loader, threads)
│   ├── decoder.hpp       DecodedInst struct, InstClass enum, decode() decl
│   ├── graphics.hpp      GraphicsBackend (framebuffer + optional SDL2)
│   ├── signal.hpp        Signal frame / rt_sigaction plumbing
│   └── frostjit.hpp      frostJIT block translator (experimental, v1.4.0-alpha)
├── src/
│   ├── decoder.cpp       Pure instruction decoder (single source of truth)
│   ├── interpreter.cpp   Instruction execution (pure switch on d.cls)
│   ├── syscalls.cpp      Linux AArch64 syscall layer (~88 syscalls)
│   ├── graphics.cpp      GraphicsBackend implementation (headless + SDL2)
│   ├── signal.cpp        Signal frame save/restore
│   ├── frostjit.cpp      frostJIT x86_64 emitter (experimental)
│   ├── jit_glue.cpp      Emulator↔FrostJIT glue (enable_jit, jit_step)
│   └── main.cpp          CLI entry point
├── api/bifrost.h         Public C API for libbifrost
├── test/                 Sample ARM64 programs (.s sources + assembled .elf)
├── ctest/                C test programs (musl-static)
├── ctest_real/           Real-world Unix utilities (musl-static): cat, wc,
│                         head, tr, rev, sort, sh, fib, yes
├── Makefile              Build, test, install targets
├── CHANGELOG.md          Release history
├── README.md             This file
└── LICENSE               Public domain (Unlicense)
```

Note: `mini_arm64_asm.py` was removed during the v1.4.0-alpha source-tree
restructure — test `.elf` files in `test/` are now checked in directly.

## Test Programs

| Program | Description |
|---------|-------------|
| `hello.elf` | Prints "Hello, ARM64!" |
| `count.elf` | Prints numbers 1-6 using a loop |
| `echo.elf` | Interactive char-by-char echo (exits on 'q') |
| `repl.elf` | Line-buffered REPL ("got: \<line\>") |
| `cat.elf` | Reads a file path from argv[1] and prints it |
| `fib.elf` | Computes fib(30) and prints it in decimal |
| `extr.elf` | Verifies EXTR instruction decode and execute |
| `ctest/test_fb.elf` | Virtual `/dev/fb0` framebuffer test (musl static) |
| `ctest_real/cat.elf` | Unix `cat` clone — concatenates files (musl static) |
| `ctest_real/wc.elf` | Unix `wc` clone — counts lines/words/bytes (musl static) |
| `ctest_real/head.elf` | Unix `head` clone — first N lines (musl static) |
| `ctest_real/tr.elf` | Unix `tr` clone — character translator (musl static) |
| `ctest_real/rev.elf` | Unix `rev` clone — reverses lines (musl static) |
| `ctest_real/sort.elf` | Unix `sort` clone — line sort with `-r` (musl static) |
| `ctest_real/sh.elf` | Interactive REPL shell: `help`/`echo`/`eval`/`exit` (musl static) |
| `ctest_real/fib.elf` | Fibonacci benchmark, accepts N on command line (musl static) |
| `ctest_real/yes.elf` | Unix `yes` clone — emit a string forever (musl static) |

For C programs, compile with a musl cross-compiler:
```bash
aarch64-linux-musl-gcc -static -O2 -o prog.elf prog.c
```

The `ctest_real/` programs are real-world Unix-style utilities built
with musl-static. They exercise the emulator's `readv`/`writev` paths,
`fgets` line buffering, `qsort`, dynamic memory (`malloc`/`realloc`/
`free`), and command-line argument handling. Pipelines between them
also work (e.g. `cat foo | wc`, `rev | tr | head`).

## Compatibility

Every binary shipped in the repo has been verified to run correctly on
the current build. The table below lists exactly what was tested; any
binary not listed here was not tested (previous versions of this table
carried stale ✅/⚠️ entries for binaries like `test_fnptr.elf`,
`test_sdl2.elf`, `hello_arm64_static`, and `toybox-aarch64` that are
not in the tree — those entries have been removed).

### `test/` — assembled AArch64 sources

| Binary | Status | Verified output |
|--------|--------|-----------------|
| `test/hello.elf` | ✅ Works | `Hello, ARM64!` |
| `test/count.elf` | ✅ Works | `1` (start of 1–6 count loop) |
| `test/fib.elf` | ✅ Works | `832040` (= fib(30)) |
| `test/cat.elf` | ✅ Works | Concatenates `argv[1]` to stdout |
| `test/echo.elf` | ✅ Works | Interactive char-by-char echo (exits on `q`) |
| `test/repl.elf` | ✅ Works | Line-buffered REPL (`got: <line>`) |
| `test/extr.elf` | ✅ Works | Prints `OK` (verifies EXTR instruction end-to-end) |

### `ctest/` — musl-static C test programs

| Binary | Status | Verified output |
|--------|--------|-----------------|
| `ctest/hello.elf` | ✅ Works | `Hello, ARM64!` |
| `ctest/loop.elf` | ✅ Works | `Loop value is: 55` (for-loop + `printf("%d")`) |
| `ctest/test_float.elf` | ✅ Works | `3.140000` — `printf("%f")` works as of v1.4.0-alpha (was hanging in beta.4) |
| `ctest/test_malloc.elf` | ✅ Works | `malloc test start` / `first=1 last=100` / `malloc test done` — `malloc`/`free`/`qsort` all succeed (was crashing in beta.4) |
| `ctest/test_fb.elf` | ✅ Works | Queries `FBIOGET_VSCREENINFO`/`FSCREENINFO`, mmaps `/dev/fb0`, draws a gradient, exits cleanly. ⚠️ `--fb-dump PATH` is currently a no-op for this binary — the guest never triggers `GraphicsBackend::ready()`, so the PPM-write path doesn't fire. Tracked in the roadmap. |

### `ctest_real/` — real-world Unix utilities (musl-static, `-O2`)

| Binary | Status | Verified output |
|--------|--------|-----------------|
| `ctest_real/cat.elf` | ✅ Works | Unix `cat` — concatenates files, exercises `readv`/`writev` |
| `ctest_real/wc.elf` | ✅ Works | Unix `wc` — line/word/byte counters |
| `ctest_real/head.elf` | ✅ Works | Unix `head` — `-n N` flag, multi-file |
| `ctest_real/tr.elf` | ✅ Works | Unix `tr` — translate / `-d` delete |
| `ctest_real/rev.elf` | ✅ Works | Unix `rev` — line reversal |
| `ctest_real/sort.elf` | ✅ Works | Unix `sort` — `qsort` (works for `n=1..20`), `realloc`, `-r`. Was crashing in beta.4 due to the SBFIZ bug. |
| `ctest_real/sh.elf` | ✅ Works | Interactive REPL shell (`help`/`echo`/`eval`/`exit`). Uses a manual tokenizer to work around the NEON bug below. |
| `ctest_real/fib.elf` | ✅ Works | `fib(30) = 832040`, `fib(40) = 102334155`. ~140 MIPS on the interpreter. |
| `ctest_real/yes.elf` | ✅ Works | Unix `yes` — ~150M lines/sec through the emulator |
| `ctest_real/fgets_test.elf` | ✅ Works | Line-buffered stdin via `fgets` (exercises the `readv` fix) |

### Not in the tree (historical notes)

These binaries were referenced in older versions of this table but are
not checked into the repo, so they cannot be re-verified. Carry-over
status from when they were last tested:

| Binary | Last-known status | Notes |
|--------|-------------------|-------|
| `test_fnptr.elf` (musl static) | ⚠️ Decode error | Function-pointer table relocation issue in static-PIE. Tracked in the roadmap. |
| `test_sdl2.elf` (musl+SDL2 static) | ⚠️ Watchdog abort | Got past atomics + mallocng init; hung later in SDL2 setup. May behave differently now that mallocng and SBFIZ are fixed — re-test before relying on this. |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | Unhandled instruction after mallocng. glibc static binaries are not a target; musl-static is. |
| `toybox-aarch64` | ⚠️ Exit 1 | PC=0 (STP/LDP mode calculation bug). Tracked in the roadmap. |

### frostJIT (`--jit`) compatibility

frostJIT is **experimental and known to crash**. The default interpreter
path is stable and passes every test above. Under `--jit`:

| Binary | Status | Notes |
|--------|--------|-------|
| `test/hello.elf` | ✅ Works | Small enough to stay within the JIT's supported subset |
| `ctest_real/fib.elf` | ❌ Segfault | Hits an unsupported instruction or NZCV-flag-emission TODO |
| `ctest_real/sort.elf` | ❌ Segfault | Same — falls back through paths frostJIT doesn't handle yet |

See the Limitations section and the roadmap for what's needed to make
`--jit` production-ready.

## What's Implemented

**Instructions** — ~140 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH,
UDIV/SDIV, RBIT/REV/CLZ/CLS, ADC/SBC with carry), branches
(B/BL/BR/BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store (immediate,
register, pair, sign-extended, unscaled, pre/post-index), LSE atomics
(LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS), acquire/release
(STLR/LDAR), exclusive monitor (LDXR/STXR/CLREX — monitor is no longer
cleared on branches, matching real hardware), FP arithmetic
(FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FCVT/SCVTF/FCVTZS/FMADD/FMSUB/
FCSEL, both S and D registers), FMOV Vd.D[1] (128-bit vector high half),
a subset of SIMD/NEON (DUP, MOVI, LD1/ST1, CNT, CMEQ, UMAXP, SHL, USHR,
EOR, ORR, AND, BIC, REV16/32/64, STP/LDP pairs, EXT, INS, TBL/TBX), and
system (SVC, MRS/MSR, BRK, HLT, CLREX, HINT, barriers).

**Decoder** — `decoder.cpp` is the single source of truth for instruction
decode. It extracts all fields into `DecodedInst` and classifies into
`InstClass`. The interpreter dispatches on `d.cls` via `switch`. **There
is no legacy if-chain anymore** (deleted in v1.3.0-beta.3). A decode
cache (`PC → DecodedInst`) avoids re-decoding on repeated execution.
PT_NOTE parsing detects `GNU_PROPERTY_AARCH64_FEATURE_1_LSE` to
disambiguate LDUR vs LSE atomics (matching real hardware behavior).

**Syscalls** — ~88 Linux AArch64 syscalls including the basics
(read/write/openat/close/exit/exit_group/brk/mmap/mprotect/mremap),
file I/O (lseek/fstat/statx/fstatat/readlinkat/statfs/fstatfs/readv/
writev/dup/dup2/pipe2/mkdirat/unlinkat/renameat/utimensat), process
info (getpid/gettid/getuid/geteuid/getgid/getegid/uname/prlimit64/
getppid), timing (clock_gettime/gettimeofday/nanosleep/clock_nanosleep),
threading (clone, futex with WAIT/WAKE/REQUEUE, set_tid_address,
set_robust_list), event loops (eventfd2, epoll_create1/epoll_ctl/
epoll_wait, timerfd_create/settime/gettime, ppoll, pselect6),
networking stubs (socketpair, listen, accept), and misc (getrandom,
ioctl, getcwd, prctl, rt_sigaction, rt_sigprocmask). Unsupported
syscalls return `-ENOSYS` silently unless `-v` is set.

**VFS** — `/proc/self/{exe,cmdline,maps,status,auxv,environ}`,
`/proc/{meminfo,cpuinfo,version}`, `/proc/sys/kernel/osrelease`,
`/dev/{null,zero,urandom,random}`. Uses `memfd_create` for seekable
virtual file descriptors.

**TLS** — TPIDR_EL0 / TPIDRRO_EL0 via MRS/MSR; 64KB TLS scratch area
pre-allocated; per-thread TLS via `clone(CLONE_SETTLS, ...)`. Zero
page mapped so NULL dereferences return 0.

**ELF** — Static ELF64 AArch64 (ET_EXEC and ET_DYN, including
static-PIE); PT_LOAD with BSS zero-fill; RELA relocations (JUMP_SLOT,
GLOB_DAT, RELATIVE, ABS64); PT_NOTE parsing for GNU property features
(LSE detection); full initial stack with argc/argv/envp/auxv (AT_PHDR,
AT_ENTRY, AT_RANDOM, AT_HWCAP, etc.).

**Graphics** — Virtual `/dev/fb0` framebuffer (memfd-backed, mmap-able
by the guest). `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO` ioctls
supported (any real fb program can query the mode). Default mode
640x480@32bpp BGRA. Headless: `--fb-dump PATH` syncs the guest's
framebuffer pages back to the host on exit and writes a PPM image
suitable for viewing in any image viewer. An optional SDL2 window
backend is available at build time via `make USE_SDL2=1` (see
`graphics.hpp` / `graphics.cpp`).

## Release History

This section summarizes what each tagged release actually delivered,
based on the commit history. Full per-commit detail lives in
[CHANGELOG.md](CHANGELOG.md).

### v1.4.0-alpha

The "printf %f, malloc, qsort, signal delivery, and an experimental
JIT" release. Re-versioned from `1.4.0` to `1.4.0-alpha` to reflect
that this is a preview of the 1.4 feature set, not a stable release.
The non-JIT path remains fully functional and passes all existing
tests; the JIT is opt-in and known to crash on some programs.

**Critical correctness fixes (the headline wins)**

- **`printf("%f", ...)` finally works.** Three root causes fixed in
  this release:

  1. **UBFM/SBFM/LSL/SBFIZ/UBFIZ mask bug.** The bitfield handler was
     using the wrong mask in the rotate case. For `LSL Xd, Xn, #shift`
     (encoded as `UBFM Xd, Xn, #(-shift MOD 64), #(63-shift)`), the
     result field lives in the HIGH bits, not the low bits. The
     previous code used the low-bits `wmask` and extracted bits
     unrelated to the shift result. This was the root cause of
     `printf("%f")` hanging forever: musl's `__extenddftf2` uses
     `lsl x0, x0, #60` to left-align the IEEE-754 mantissa, and the
     buggy handler produced `0x1` instead of `0xF000000000000000`,
     corrupting the long-double value. The downstream `__fixunstfsi`
     then looped forever. Verified: `ctest/test_float.elf` now prints
     `3.140000` and exits 0.

  2. **FMOV (scalar, immediate) decoding.** The 8-bit FP immediate was
     decoded with a wrong sign/exp4/mant3 layout. Replaced with the
     ARM ARM `VFPExpandImm` algorithm. Example: `fmov d0, #2.5` used
     to produce `0x4078…` (= 384.0) instead of `0x4004…` (= 2.5),
     corrupting every `printf("%f", float_var)`.

  3. **Interactive shell no longer hangs (termios).** The emulator was
     unconditionally enabling raw TTY mode whenever stdin was a TTY.
     Raw mode turns off `ICANON` and `ECHO`, so the host kernel
     delivered each keystroke as a 1-byte `read()`. That broke every
     guest program using line-oriented stdio: musl's `fgets()` in
     `sh.elf` received one byte per `read()` and never saw the
     trailing newline. Default is now to leave the host TTY alone;
     raw mode is opt-in via `--raw-tty`.

- **malloc / free crash and qsort n≥8 bug — root cause found.** Both
  were the same bug: `SBFM`/`UBFM` with `imms < immr` (the
  `SBFIZ`/`UBFIZ`/`BFI`/`LSL` alias group) used the wrong mask. The
  previous code masked the ROR'd value with `high_mask` (bits at the
  TOP of the register), but `SBFIZ`/`UBFIZ` need the field at bits
  `[lsb+width-1:lsb]` which is NOT necessarily at the top. This broke
  `SBFIZ` specifically: `sbfiz x3, x19, #3, #32` (which computes
  `pshift*8` for `lp[]` indexing) returned 0 instead of the correct
  value (e.g., 24 for `pshift=3`). This caused musl's smoothsort to
  read the wrong `lp[]` element, corrupting the heap structure and
  producing wrong sort results for `n >= 8`. The same bug also caused
  the malloc/free crash: musl's mallocng uses `SBFIZ` internally, and
  the wrong result corrupted heap metadata, triggering the
  `BRK #1000` assertion in `get_meta()`. Rewrote the `BFI`/rotate
  case to directly extract low `(imms+1)` bits, sign-extend for
  `SBFM`, and shift left by `(datasize - immr)`. Verified: `qsort`
  works for n=1..20, `test_malloc` runs to completion, and `sort.elf`
  no longer crashes.

- **LSR #0 / LSL #0 — shift by 64.** On AArch64, `LSR Xd, Xn, #0` and
  `LSL Xd, Xn, #0` both encode as `UBFM Xd, Xn, #0, #63`, which
  shifts by 64 (result = 0), NOT a no-op. The `UBFM` extract case
  with `imms=datasize-1` and `immr=0` was returning the source
  unchanged. This corrupted musl's smoothsort `shr()` function.

**New subsystems**

- **Signal delivery (`signal.hpp` / `signal.cpp`).** `rt_sigaction`
  (syscall 134) now actually installs handlers instead of being a
  silent no-op. `rt_sigreturn` (syscall 133) pops a `SignalFrame` and
  restores CPU state. `kill` (129), `tkill` (130), `tgkill` (131)
  deliver signals to the current thread. A `sigreturn` trampoline
  (`mov x8, #139; svc #0`) is mapped at a fixed guest address
  (`0x7000000000`) so signal handlers can return into it. The run
  loop catches `UnmappedMemory` and delivers `SIGSEGV` if a handler
  is installed (otherwise terminates with exit `128+11`).
  Limitations (documented in `signal.hpp`): no `siginfo_t`/
  `ucontext_t` contents, no `SA_RESTART`, no signal masks, no
  `sigaltstack`, no real-time signals 32+.

- **frostJIT — experimental block-translation JIT
  (`frostjit.hpp` / `frostjit.cpp`).** Translates AArch64 basic blocks
  into x86_64 machine code in a 16 MB `mmap`'d RWX code cache, sharing
  the existing decoder with the interpreter. Uses a register-bank-in-
  memory model (ARM64 `X0`-`X30`/`SP` live in the `CPU` struct; the
  JIT emits load/op/store sequences with no regalloc). Supports a
  limited subset: `ADD`/`SUB`/`AND`/`ORR`/`EOR` (reg + imm),
  `MOVN`/`MOVZ`/`MOVK`, `CSEL`, `MADD`/`MSUB`, `LSL`/`LSR`/`ASR`/
  `ROR`, `LDR`/`STR` (imm/unscaled/reg), `ADR`/`ADRP`, `B`/`BL`/
  `BR`/`BLR`/`RET`, `Bcond`/`CBZ`/`CBNZ`/`TBZ`/`TBNZ` (simplified —
  always branches), `NOP`. Falls back to the interpreter for `SVC`,
  FP/SIMD, atomics, `MSR`/`MRS`, `BRK`/`HLT`, and any unsupported op
  (block ends, single-step). Block cache keyed by guest PC; cache
  hits skip translation. Statistics (blocks translated/executed,
  cache hit rate, interpreter fallbacks, code cache usage) printed
  with `-v`. Enabled via `--jit`; default is interpreter-only.
  frostJIT is **experimental** and known to crash on some programs
  (`fib`, `sort` segfault under `--jit`). Tracked in the roadmap.

- **SDL2 window backend for `/dev/fb0` (`graphics.hpp` /
  `graphics.cpp`).** Build with `make USE_SDL2=1`. Opens a real SDL2
  window and pushes the framebuffer on every `refresh()`. The run
  loop now periodically syncs the guest framebuffer back to the host
  and calls `graphics_.refresh()` every ~1M instructions (~6 fps at
  6 MIPS — enough for interactive graphics without killing
  performance). `poll_events()` is also called so closing the SDL2
  window terminates the guest cleanly. Headless mode (default) skips
  this; the `--fb-dump PATH` PPM dump on exit still works.

- **VFS improvements (`syscalls.cpp`).** Added `BIFROST_ROOT`
  environment variable for guest path sandboxing: set
  `BIFROST_ROOT=/tmp/guest-root` to redirect all guest absolute
  paths (except `/proc` and `/dev`) to that directory on the host.
  Added `/dev/tty` (opens host controlling terminal), `/dev/stdin`,
  `/dev/stdout`, `/dev/stderr` (dup host fds 0/1/2). `fstatat`
  (syscall 79) now does a real host `stat` instead of returning a
  fake "regular file, 0 bytes" result. All path-based syscalls now
  go through `map_guest_path()` for consistent `BIFROST_ROOT`
  remapping.

- **FCVT rounding modes (`interpreter.cpp`).** Added
  `FCVTNS`/`FCVTNM`/`FCVTPS`/`FCVTPM`/`FCVTZS`/`FCVTZU` (and
  unsigned variants) with explicit rounding mode field. Previously
  only `FCVTZS`/`FCVTZU` (round-toward-zero) was handled; the
  others fell through to the default case. Added
  `FRINTN`/`FRINTP`/`FRINTM`/`FRINTZ`/`FRINTA`/`FRINTX`/`FRINTI`
  variants in the FP 1-source group. Added `FCVT H` half-precision
  (FP16) conversions (`D`↔`H`, `S`↔`H`) with manual IEEE 754
  binary16 ↔ binary32 conversion helpers. Required for musl's
  hex-float printf path.

- **Source-tree restructure.** All `.cpp` moved to `src/`, all `.hpp`
  moved to `include/`, `mini_arm64_asm.py` removed (dead code; not
  referenced by the build since test `.elf` files are pre-assembled).
  Makefile updated with `-Iinclude` and `src/` paths.

**Stability verification**

All `test/` and `ctest/` tests pass under the default interpreter
path: `hello`, `loop`, `test_float`, `test_fb`, `sh`, `cat`, `wc`,
`head`, `rev`, `fib`, `yes`, `fgets_test`, `extr`, `count`, `echo`,
`repl`. `qsort` works for `n = 1..20`. `printf("%f")` works. The
frostJIT path is the only known source of crashes; the interpreter
path is stable.

### v1.3.0-beta.4

The "real hierarchical decoder + 3.8x performance + softfloat fixes"
release. Three major areas of improvement:

1. **Performance**: 3.78x speedup (37 → 140 MIPS) via direct-mapped
   decode cache and memory page cache.
2. **Hierarchical decoder**: flat if-chains replaced with a true
   two-level switch on bits[28:24].
3. **Softfloat fixes**: four critical bugs (CCMP, CSEL/CSNEG, SIMD
   Q-form, BFM BFI) that blocked musl's 128-bit long double routines,
   partially unblocking `printf("%f")`.

**Performance: 3.8x speedup (37 → 140 MIPS)**

Two optimizations:

1. **Direct-mapped decode cache.** Replaced
   `std::unordered_map<uint64_t, DecodedInst>` (hash + 88-byte
   struct copy per instruction) with a flat 4096-entry direct-mapped
   array (384 KB). The hot path is now: hash PC to 12-bit index,
   compare tag, use `const` reference. No hash, no copy.
   `__builtin_expect` for branch prediction. Alone gives 2.17x
   (37 → 80 MIPS).

2. **Memory page cache.** Added single-entry last-page caches for
   read and write. The old code locked a mutex + did an
   `unordered_map` lookup on every memory access. The new code checks
   the last-page cache first (no lock, no hash — just a tag compare +
   `memcpy`). For tight loops accessing the same page, eliminates all
   mutex/hash overhead. Gives an additional 1.75x (80 → 140 MIPS).

**Hierarchical decoder**

The decoder is now a true two-level hierarchical switch. v1.3.0-beta.3
had a stub `switch (bits[28:24])` at the top of `decode()` that did
nothing (`default: break;`) and fell through to ~500 lines of flat
`if ((inst & MASK) == VAL)` chains. v1.3.0-beta.4 replaces that with a
real hierarchical switch: outer switch on bits `[28:24]` (the ARM ARM
major encoding group), inner switch on the group-specific
discriminator. Every flat `if` chain is now a `case` with early
`return`. See the Architecture section above for details.

**Critical decoder/interpreter bug fixes (unblocks `printf("%f")`)**

Four bugs that together blocked musl's 128-bit long double softfloat
path (used by `printf("%f")`):

1. **CCMP register vs immediate form.** The register/immediate
   distinction is at **bit 11** (0=register, 1=immediate), not bit 21
   (always 0). The previous code always treated CCMP as immediate, so
   `ccmp x6, x7, #0, eq` compared `x6` with `#7` instead of `x7`.
   Broke `__eqtf2` (long double equality), making `y == 0.0` always
   false — printf's do/while digit extraction loop never exited.

2. **CSEL/CSINC/CSINV/CSNEG decode.** The variant is selected by BOTH
   `bits[30:29]` (opc) AND `bits[11:10]` (op2), not just
   `bits[11:10]`. The previous code confused CSINC with CSNEG. Broke
   CNEG (alias for CSNEG), used by `__gttf2`/`__lttf2` to negate
   return values — producing `-1` instead of `1`, breaking all long
   double ordering comparisons.

3. **SIMD DP Q-form bugs.** The SIMD_DP if-chains had masks that
   included bit 30 (Q), so `Q=1` (128-bit) forms of `DUP`, `INS`,
   `ORR` (MOV alias), and `EXT` were silently NOP'd. This broke
   musl's 128-bit softfloat, which uses `mov v1.16b, v0.16b` to copy
   128-bit values. Converted the SIMD_DP handler from flat if-chains
   to a proper switch with Q-stripped sub-discriminator.

4. **BFM BFI field mask.** The previous code computed
   `field_mask = mask | hi_mask = ~0`, replacing ALL of `Rd` instead
   of just the target field. Fixed to `mask << lsb`. Broke
   `__floatsitf`'s BFI, corrupting 128-bit long double values.

**EXTR instruction fixed**

Three bugs around EXTR:

1. The bitfield check (mask `0x1F000000`, ignoring bit 23) came
   BEFORE the EXTR check (mask `0x1F800000`, requiring bit 23 = 1).
   Every EXTR was silently misdecoded as SBFM/BFM/UBFM. v1.3.0-beta.4
   routes on bit 23 first.
2. The interpreter's EXTR handler had the operand order backwards
   (`Rm:Rn` instead of `Rn:Rm`).
3. The interpreter used `(rn << width)` with `width == 64`, which is
   undefined behavior in C++. Fixed with `__uint128_t`.

**Graphics backend wired up**

`GraphicsBackend` is no longer a stub. `Emulator` owns an instance.
`openat("/dev/fb0")` returns a memfd-backed fd that the guest can
`mmap` and write pixels to. `FBIOGET_VSCREENINFO` and
`FBIOGET_FSCREENINFO` ioctls supported. `--fb-dump PATH` syncs the
guest's framebuffer pages back to the host on exit and writes a PPM
file. Verified pixel-by-pixel. New `ctest/test_fb.c` verifies the
full pipeline. (SDL2 window support came in v1.4.0-alpha.)

**Additional decoder correctness fixes**

- **64-bit CBZ/CBNZ/TBZ/TBNZ** (`sf=1`, bits[31:29] = 101) now
  decode correctly. The previous flat masks caught only the 32-bit
  form.
- **BRK and HLT** now enforce `bits[4:0] == 0` per the ARM ARM.
- **Add/subtract extended register** now enforces `bits[23:22] == 00`.
- **STP/LDP pre-index collision** is now structural (outer case 0x09
  vs 0x0A, not if-chain ordering).
- **INS (general)** case label fixed from unreachable `0x4E000C00`
  to correct `0x0E001C00`.

**Code-review audit (post-release)**

A review pass after the beta.4 release landed an additional batch of
correctness and hygiene fixes:

- **Page-cache sentinel.** `Memory::PageCache` defaulted
  `read_page = 0`, which matched any real access to page 0 (e.g. a
  null-deref at offset `0x480`), causing `read_ptr = nullptr` to be
  dereferenced — `hello.elf` was crashing deterministically. Default
  is now `UINT64_MAX`.
- **Per-vCPU decode cache.** Moved the decode cache from the shared
  `Emulator` into each `CPU`, eliminating a data race between
  concurrently-running guest threads (previously two vCPUs could
  race on the same cache slot).
- **Correct AArch64 syscall numbers.** Verified every `case N` in
  `syscalls.cpp` against `asm-generic/unistd.h` and renumbered 15
  mislabeled slots: `nanosleep` (100→101), `clock_nanosleep`
  (206→115), `rt_sigreturn` (133→139), `mremap` (227→216), `ppoll`
  (168→73), `getcwd` (165→17), `sendfile` (40→71), `epoll_pwait`
  (22, was wrongly pipe2), `mincore` (232, was wrongly epoll_wait),
  `getrlimit` (163, was wrongly acct), `getrusage` (165, was wrongly
  getcwd), `getcpu` (168, was wrongly ppoll), `msync` (227, was
  wrongly mremap), `mount` (40, was wrongly sendfile), and
  `process_vm_readv` (270, was wrongly an eventfd2 alt entry).
- **ELF loader bounds checks.** Program-header table is now
  validated against `data.size()` before indexing — prevents OOB
  reads on truncated or hostile ELF files.
- **Futex liveness fix.** The `*uaddr == val` check moved inside the
  slot lock, eliminating a "wait forever" race where a concurrent
  waker could slip in between check and waiter increment.
- **`fb_fix_screeninfo` size fix.** Hardcoded `out_sz = 72`
  corrected to `80` — guest was reading a truncated struct missing
  `capabilities` and `reserved[2]`.
- **`getrandom` hardened.** Removed the unseeded non-thread-safe
  `rand()` fallback path; now returns `-ENOSYS` if `/dev/urandom`
  is unavailable.
- **Decoder UB fix.** `decode_bitfield_imm`'s rotation step
  `elem << (esize - R)` was UB when `R == 0` and `esize == 64`
  (shift by 64). Now guarded with `if (R != 0)`.
- **Zero compiler warnings.** `make` now builds clean under
  `-Wall -Wextra` (was 24+ warnings).
- **ASan/UBSan clean.** Debug build (`make debug`) runs all tests
  under AddressSanitizer + UndefinedBehaviorSanitizer with no
  violations.
- **Dead code removed.** `Emulator::exiting_` (write-only),
  `GuestThread::done` (write-only),
  `GuestThread::set_tid_address_ptr` (duplicated by
  `CPU::set_tid_address_ptr`), duplicate pipe2 handler at case 22,
  and an empty `CLONE_CHILD_SETTID` if-block.

**Real-world testing (post-release)**

The emulator was tested against 9 real-world Unix-style utility
programs built with `aarch64-linux-musl-gcc -O2 -static`. All
programs in `ctest_real/` (`cat`, `wc`, `head`, `tr`, `rev`, `sort`,
`sh`, `fib`, `yes`) work correctly end-to-end.

- **`readv` syscall number fix.** `readv` was at the wrong syscall
  number (case 67 = `preadv64`, missing case 65 = `readv`). musl's
  `fgets` uses `readv` with 2 iovecs for buffered stdin, so any
  program using `fgets()` on a non-tty stdin silently broke. Added
  both `readv` (65) and `preadv64` (67) handlers correctly.
- **`isatty()` fixed.** The ioctl handler previously returned
  success for every unknown ioctl (including `TIOCGWINSZ`), which
  made `isatty()` always return `true` — even for pipes and regular
  files. This broke musl's stdio buffering decisions on non-tty
  stdin. The handler now forwards `TIOCGWINSZ`,
  `TCGETS`/`TCSETS`/etc., and `FIONREAD` to the host, and returns
  `-ENOTTY` for everything else.
- **Pipelines verified.** `cat foo | wc`, `rev | tr | head`, `sort`
  with stdin all produce expected output. Throughput:
  `fib(40)` = 102334155 in ~23ms (~140 MIPS); `yes` emits ~150M
  lines/sec through the emulator.
- **Interactive shell verified.** `ctest_real/sh.elf` is a tiny REPL
  that supports `help`, `echo ARGS`, `eval EXPR` (arithmetic), and
  `exit [N]`. It uses a manual tokenizer instead of `strtok` (see
  Limitations below). Interactive keyboard input works end-to-end
  via a real PTY.
- **Latent NEON bug discovered.** musl's `strtok`/`strtok_r` expose a
  NEON/SIMD corruption bug (see Limitations). The shell works around
  this with a manual tokenizer. The specific NEON instruction that
  misbehaves is the next thing to track down — likely related to the
  ORR (vector) handler's byte order vs `STR Qn`/`LDR Qn`.

### v1.3.0-beta.3

The "mallocng hang finally fixed + the legacy if-chain is deleted"
release. The single biggest blocker since v1.1.5-alpha.1 was squashed,
and the interpreter was migrated to a pure `switch(d.cls)` dispatch.

- **The mallocng hang is fixed.** Root cause: a 32-bit rotation bug in
  the UBFM/SBFM/BFM instruction handler. When executing
  `lsl w24, w26, #4` (which the compiler encodes as
  `ubfm w24, w26, #28, #27`), the emulator used `ror64` followed by
  a `uint32_t` cast, which lost the wrapped bits —
  `ror64(0x2, 28)` puts the wrapped bits at position 36, and
  `(uint32_t)0x2000000000 = 0x0` instead of the correct `0x20` (= 32).
  This made musl's mallocng stride 0, which caused `alloc_slot` to
  infinitely recurse. `malloc`/`free`/`qsort` all work after this
  fix.
- **BIC/ORN/EON/BICS fix (the printf/fclose root cause).** The
  logical shifted register group has an N bit (bit 21) that inverts
  the second operand: AND→BIC, ORR→ORN, EOR→EON, ANDS→BICS. The
  interpreter was ignoring N entirely — BIC was treated as AND. This
  broke musl's `strlen` zero-byte detection, causing strlen to scan
  past NUL terminators, which corrupted stdio buffer management,
  producing garbled printf output and the exit-133 fclose crash.
- **Bitmask immediate decode fix.** The decoder's
  `decode_bitmask_imm` used `~0` for `esize==64` regardless of `S`,
  producing all-ones instead of the correct mask. The interpreter's
  inline bitmask decode had a separate bug (shifted ones to
  `esize-1-S` instead of bit 0). Both fixed.
- **Hierarchical STP/LDP decoder fix.** The decoder now checks
  Load/Store pair (STP/LDP) before logical shifted register,
  preventing the pre-index STP/LDP vs ORR encoding collision. The
  STP/LDP mask was expanded to catch all three addressing modes
  (post-index, signed offset, pre-index) for both GP and SIMD
  registers, with a mode validation check to reject LDUR/STUR that
  shares the same top bits.
- **The decoder switch is complete, the legacy if-chain is deleted.**
  Every instruction handler — branches, system, data processing
  (immediate and register), load/store, atomics, SIMD data-
  processing, and FP scalar — now lives in the `switch(d.cls)` block
  in `interpreter.cpp`. The ~500-line legacy if-chain that lived
  below the switch since alpha.1 is gone.
- **FP scalar decoder fix.** The FP scalar decoder only matched
  `0x1E200000` (32-bit single-precision). It missed all 64-bit
  (`sf=1`, top byte `0x9E`) and double-precision (`ftype=01`) FP
  instructions, causing decode errors on any binary using D
  registers. Fixed by adding the broad `0x1E000000` and `0x9E000000`
  top-byte masks.
- **mallocng MAP_FIXED overlap handling.** When musl's mallocng calls
  `mmap(MAP_FIXED, addr, ...)` inside the brk region, the brk is now
  pushed forward past the mmap'd region. This prevents a subsequent
  `brk(new)` extension from re-mapping the same pages via
  `map_range` and corrupting musl's metadata.
- **Hang watchdog.** The run loop tracks the last PC and counts how
  many times it's been executed consecutively. If the same PC is hit
  more than 50 million times in a row (which only happens for
  `b .` self-branches or genuinely stuck atomic-CAS loops), the
  emulator aborts with a diagnostic message instead of spinning
  forever. Legitimate tight loops (`fib`, `count`, etc.) cycle
  through multiple PCs and never trip the watchdog.
- **MADD family decoder fix.** The old decoder classified `SMULH` as
  `sub_op=7`, but per the ARM ARM pseudocode `SMULH` is `sub_op=2`.
  The old `case 7` was unreachable; `SMULH` instructions would have
  fallen through to UNKNOWN and thrown a `DecodeError`.
- **LSE atomics decoder fix.** The old decoder treated `SWP` as a
  distinct encoding (bit 21=1) and `LDADD` family as bit 21=0. Per
  the ARM ARM, **all** LSE atomics have bit 21=1 — they're
  distinguished by the `opc` field at bits 15:12. The new decoder
  classifies any bit-21=1 encoding in the 111000 group as
  `LSE_ATOMIC` and sub-dispatches on `atom_op` in the interpreter.
  The `has_lse_` gate is now checked in the interpreter's
  `LSE_ATOMIC` case: if the binary doesn't declare LSE, the encoding
  is executed as LDUR/STUR (matching real hardware).

### Earlier releases

- **v1.3.0-beta.2** — malloc fixes, ARM ISA research, STP/LDP
  decoder correction, SWP instruction fix.
- **v1.3.0-beta.1** — exclusive monitor fix, fclose crash fix, SDL2
  cross-build.
- **v1.3.0-alpha.1..3** — wired the decoder up as the entry point,
  `switch(d.cls)` dispatch, full decoder rewrite, decode cache,
  MOVI fix, immediate group migrated to the decoder switch.
- **v1.1.5-alpha.1** — SIMD LDR/STR Q-form, FMOV Vd.D[1], BFM fix,
  ADC/SBC, more syscalls.
- **v1.1.1-alpha.1** — PT_NOTE-based LDUR/LSE disambiguation.
- **v1.1.0-rc.1..2** — VFS support, SIMD STP/LDP, real FP/SIMD
  arithmetic (FADD/FSUB/FMUL/FDIV/FSQRT/FCMP/FCVT/SCVTF/FCVTZS/
  FMADD), file split, decoder.hpp.
- **v1.1.0-beta.1** — LSE atomics encoding fix (toybox gets past
  mallocng init).
- **v1.1.0-alpha.1** — exclusive monitor, LSE atomics fix, mmap
  MAP_FIXED, getppid, mremap in-place.
- **v1.0.0-beta.1** — initial commit.

## Limitations

This is alpha-quality software. Known issues:

- **frostJIT (`--jit`) is experimental and known to crash.** `fib`,
  `sort`, and other programs that hit the JIT's unsupported
  instruction paths or NZCV flag-emission TODOs can segfault. The
  default interpreter path is stable and passes all tests; do not
  rely on `--jit` for production use yet.

- **Signal delivery is partial.** `rt_sigaction` installs handlers,
  `rt_sigreturn` restores CPU state, and `kill`/`tkill`/`tgkill`
  deliver signals to the current thread. However: no `siginfo_t`/
  `ucontext_t` contents are passed to handlers, no `SA_RESTART`,
  no signal masks, no `sigaltstack`, no real-time signals 32+, and
  no cross-thread delivery. See `signal.hpp` for the full list.

- **NEON bug triggered by `strtok`/`strtok_r`.** musl's `strtok` and
  `strtok_r` call `strspn`/`strcspn`, which build a 256-bit bitset
  using NEON/SIMD instructions. After a successful `fgets` of
  `"hi\n"`, calling `strtok_r` corrupts registers `x21`/`x22` with
  garbage values like `0xffff98f000000108` (top 16 bits set —
  invalid user-space addresses on AArch64). The specific NEON
  instruction that corrupts state has not yet been identified. The
  `ctest_real/sh.elf` REPL shell works around this by using a manual
  tokenizer. Programs that avoid `strtok` family functions work
  correctly. Likely root cause: an ORR (vector) handler that writes
  `v_lo`/`v_hi` in a different byte order than `STR Qn`/`LDR Qn`
  reads them, or a 128-bit shift/extract instruction whose
  high-half handling is wrong.

- **Function pointer tables in static-PIE binaries** may not relocate
  correctly (`test_fnptr` hits a decode error).
- **No dynamic linking** — static binaries only.
- **No ASLR** — binaries load at their preferred vaddr.
- **`toybox-aarch64` crashes at PC=0** — STP/LDP mode calculation
  bug. Fix requires further decoder work.
- **glibc 2.36+ static binaries** hit a decode error on an unhandled
  instruction.
- **`test_sdl2.elf`** gets past atomics and mallocng init but hangs
  later in SDL2 setup. The hang watchdog catches it as a fast-fail.

## Roadmap

**v1.4.0 (stabilize the alpha)**

1. **Make frostJIT not crash.** Fill in the NZCV flag-emission paths
   (currently `TODO` in `frostjit.cpp` for `ADDS`/`SUBS`/`CMP` and
   shifted-register forms), add SIMD/FP fallbacks that don't abort
   the block, and add a regression test that runs every `ctest_real/`
   binary under `--jit` and compares output against the interpreter.
   Target: `--jit` runs `fib(40)` and `sort` without segfaulting.
2. **Fix the NEON/SIMD bug** that breaks `strtok`/`strtok_r`. Trace
   the `strspn` bitset construction in musl to pinpoint the exact
   instruction. Most likely candidate: an ORR (vector) handler that
   writes `v_lo`/`v_hi` in a different byte order than `STR Qn`/
   `LDR Qn` reads them, or a 128-bit shift/extract instruction whose
   high-half handling is wrong.
3. **Complete signal delivery.** Add `siginfo_t`/`ucontext_t`
   contents, `SA_RESTART`, signal masks, `sigaltstack`, and
   cross-thread delivery. The signal frame plumbing landed in
   v1.4.0-alpha; this is the remaining work to make it useful for
   real signal-heavy programs.
4. **Fix `test_fnptr`** — investigate static-PIE self-relocation.
5. **More test coverage**: threads, signals, real-time signals.

**v1.4.x (feature work)**

1. **SDL2 audio + input** on top of the v1.4.0-alpha SDL2 video
   backend. The video path is wired up and refreshed every ~1M
   instructions; audio and input are the remaining pieces for
   interactive graphical guests.
2. **Sub-decode the SIMD DP and FP scalar catch-all groups.**
   Currently these are routed as generic `SIMD_DP` / `FP_SCALAR` and
   re-dispatched in the interpreter. The hierarchical decoder
   structure makes adding dedicated `InstClass` values for each a
   clean refactor — and would make the NEON bug above easier to
   isolate.
3. **Promote frostJIT from experimental to default.** Once the
   stabilization work above lands, flip the default to JIT-on with
   an interpreter fallback. Target: 500+ MIPS. The outer switch on
   bits[28:24] maps directly to a JIT dispatch table, and the
   direct-mapped decode cache already demonstrates the hot-path
   speedup achievable with flat dispatch.

**v2.0+ (long-term)**

1. **Full game support** — framebuffer/DRM, audio, input. Long-term
   goal: statically-linked ARM64 SDL2 games at playable framerates.
2. **Dynamic linking** — currently static binaries only. Loading and
   resolving a dynamic AArch64 binary would significantly expand the
   set of runnable software.
3. **ASLR** — binaries currently load at their preferred vaddr;
   randomizing load addresses would catch guest programs that
   accidentally depend on absolute addressing.

## Forking

This project does **not** accept pull requests or contributions. If you
want to modify it, fix a bug, or add a feature, just fork it — that's
what the public domain license is for. No attribution required, no
upstreaming expected.

## License

Public domain. Use freely. See [LICENSE](LICENSE) for details.

## Acknowledgments

Inspired by [qemu-user](https://www.qemu.org/docs/master/user/main.html),
[box64](https://github.com/ptitSeb/box64), and
[FEX-Emu](https://github.com/FEX-Emu/FEX). Written from scratch as a
learning project.
