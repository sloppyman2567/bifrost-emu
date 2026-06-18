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

  bifrost-emu  v1.3.0-beta.2
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.3.0-beta.2](https://img.shields.io/badge/version-1.3.0--beta.1-orange.svg)](CHANGELOG.md)

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
┌──────────────────────┐         ┌───────────────────────┐
│    decoder.cpp       │         │  interpreter.cpp      │
│                      │         │                       │
│  decode(d, inst)     │ ──────► │  switch(d.cls) {      │
│                      │         │    case ADD_IMM:      │
│  - bit patterns      │         │      res = a + b;     │
│  - field extraction  │         │      break;           │
│  - InstClass         │         │    case LDR_IMM:      │
│  - SP/XZR disambig   │         │      ...              │
│  - shift calc        │         │    ...                │
│  - addressing        │         │  }                    │
│                      │         │                       │
│  NO execution        │         │  EXECUTE ONLY         │
│                      │         │  - reads d.* fields   │
│                      │         │  - no bit extraction  │
└──────────────────────┘         └───────────────────────┘
         ▲
         │
┌────────┴─────────────┐
│  decode_cache_       │
│  PC → DecodedInst    │
│  (avoids re-decode)  │
└──────────────────────┘
```

The **decoder** (`decoder.cpp`) is the single source of truth for
instruction decode. It extracts all fields into a `DecodedInst` struct
and classifies the instruction into an `InstClass` enum value. The
decoder does NO execution — only bit extraction and classification.

The **interpreter** (`interpreter.cpp`) dispatches on `d.cls` via a
`switch` statement. Each case reads `d.*` fields and executes. The
interpreter never does bit extraction (`op >> 22`, `(op & mask)`, etc.)
— that's the decoder's job.

A **decode cache** (`PC → DecodedInst`) avoids re-decoding the same
instruction on repeated execution (tight loops). Since guest code is
not self-modifying (static binaries only), each PC always decodes to
the same instruction.

### Current Migration Status

The interpreter uses a **hybrid dispatch**: migrated instruction
classes are handled in the `switch`, the rest fall through to a legacy
`if`-chain (transitional, will be deleted in v2.0).

**Migrated to switch (v1.3.0-beta.2):**
- B, BL, Bcond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET (branches)
- ADC/ADCS/SBC/SBCS (add/subtract with carry)
- FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] (FP move with index)
- ADR/ADRP (PC-relative address)
- MOVN/MOVZ/MOVK (move immediate)
- ADD/SUB/ADDS/SUBS immediate
- SBFM/BFM/UBFM (bitfield)
- EXTR (extract)
- AND/ORR/EOR/ANDS immediate (logical immediate)

**Still in if-chain (pending migration):**
- ADD/SUB register (shifted/extended)
- AND/ORR/EOR/ANDS register (logical shifted register)
- CSEL/CSINC/CSINV/CSNEG, CCMP/CCMN
- MADD/MSUB, UDIV/SDIV, LSL/LSR/ASR/ROR
- RBIT/REV/REV16/REV32/CLZ/CLS
- Load/store (all forms: immediate, unscaled, register, pair)
- LSE atomics, exclusives (STXR/LDXR/STLR/LDAR)
- SIMD data processing (DUP, MOVI, SHL, USHR, CNT, CMEQ, etc.)
- FP scalar (FMOV, FADD, FSUB, FMUL, FDIV, FCMP, FCVT, etc.)
- System (SVC, BRK, HLT, MSR/MRS, barriers, CLREX)

## Performance

~23 MIPS on a typical desktop, with the decode cache providing a
significant speedup for tight loops. The `fib(30)` test runs 258
instructions in under 1ms; musl static hello world runs 1,760
instructions in under 0.1ms. A JIT is planned for v2.0 (target:
100-500 MIPS).

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

## Usage

```
bifrost-emu [options] <elf-file> [args...]

Options:
  -d, --debug     trace every instruction to stderr
  -v, --verbose   print execution stats on exit
  -V, --version   show version and exit
  -h, --help      show help

Examples:
  bifrost-emu hello.elf
  bifrost-emu -d hello.elf
  bifrost-emu cat.elf /etc/hosts
  bifrost-emu -v echo.elf hello
```

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
├── arm64_emu.hpp         Emulator class (CPU, Memory, ELF loader, threads)
├── decoder.hpp           DecodedInst struct, InstClass enum, decode() decl
├── decoder.cpp           Pure instruction decoder (single source of truth)
├── interpreter.cpp       Instruction execution (switch on d.cls + legacy if-chain)
├── syscalls.cpp          Linux AArch64 syscall layer (~88 syscalls)
├── graphics.hpp/cpp      GraphicsBackend (framebuffer stub for 1.3.0)
├── api/bifrost.h         Public C API for libbifrost
├── main.cpp              CLI entry point
├── mini_arm64_asm.py     Built-in ARM64 assembler (for test programs)
├── test/                 Sample ARM64 programs (.s sources)
├── Makefile              Build, test, install targets
├── CHANGELOG.md          Release history
├── README.md             This file
└── LICENSE               Public domain (Unlicense)
```

## Test Programs

| Program | Description |
|---------|-------------|
| `hello.elf` | Prints "Hello, ARM64!" |
| `count.elf` | Prints numbers 1-6 using a loop |
| `echo.elf` | Interactive char-by-char echo (exits on 'q') |
| `repl.elf` | Line-buffered REPL ("got: \<line\>") |
| `cat.elf` | Reads a file path from argv[1] and prints it |
| `fib.elf` | Computes fib(30) and prints it in decimal |

Assemble new test programs with:
```bash
python3 mini_arm64_asm.py prog.s -o prog.elf
```

For C programs, compile with a musl cross-compiler:
```bash
aarch64-linux-musl-gcc -static -O2 -o prog.elf prog.c
```

## Compatibility

| Binary | Status | Notes |
|--------|--------|-------|
| `hello.elf` (assembled) | ✅ Works | |
| `count.elf` (assembled) | ✅ Works | |
| `fib.elf` (assembled) | ✅ Works | |
| `cat.elf` (assembled) | ✅ Works | |
| `echo.elf` (assembled) | ✅ Works | Interactive, raw TTY |
| `repl.elf` (assembled) | ✅ Works | Line-buffered |
| `hello_arm64_musl` (static) | ✅ Works | Full musl static |
| `loop.elf` (musl static-PIE, `-O2`) | ✅ Works | `for` loop + `printf("%d")` |
| `test_recursion.elf` (musl static) | ✅ Works | Recursive `fib(20)` |
| `test_structs.elf` (musl static) | ✅ Works | Structs, pointers, `strcat`/`strlen` |
| `test_bitops.elf` (musl static) | ✅ Works | 64-bit arithmetic, `%016llx` |
| `test_switch.elf` (musl static) | ✅ Works | Switch/jump-table, 2D arrays, `goto` |
| `test_advanced.elf` (musl static) | ✅ Works | Ackermann recursion |
| `test_argv.elf` (musl static) | ✅ Works | `argc`/`argv` with extra args |
| `test_args_math.elf` (musl static) | ✅ Works | `strtol`, sum/product of args |
| `test_strings.elf` (musl static) | ✅ Works | `strcmp`/`strchr`/`strrchr`/`memset` |
| `test_math.elf` (musl static) | ✅ Works | 64-bit mul/div, shifts, ternary |
| `test_fnptr.elf` (musl static) | ⚠️ Decode error | Function pointer table relocation issue |
| `test_fileio.elf` (musl static) | ✅ Works | File I/O + `fclose` cleanup (fixed in beta.1) |
| `test_float.elf` (musl static) | ❌ Hangs | `printf("%f")` → softfloat recursion (partial fix) |
| `test_malloc.elf` (musl static) | ❌ Hangs | mallocng init recursion (brk/mmap interaction) |
| `test_sdl2.elf` (musl+SDL2 static) | ❌ Hangs | Gets past atomics, hangs in mallocng init |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | Unhandled instruction after mallocng |
| `toybox-aarch64` | ⚠️ Exit 1 | PC=0 (STP/LDP mode bug, planned for v2.0) |

## What's Implemented

**Instructions** — ~120 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB, UDIV/SDIV, RBIT/REV/CLZ, ADC/SBC with carry),
branches (B/BL/BR/BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store
(immediate, register, pair, sign-extended, unscaled), LSE atomics
(LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS), acquire/release
(STLR/LDAR), exclusive monitor (LDXR/STXR/CLREX — monitor is no longer
cleared on branches, matching real hardware), FP arithmetic
(FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FCVT/SCVTF/FCVTZS/FMADD/FMSUB/
FCSEL, both S and D registers), FMOV Vd.D[1] (128-bit vector high half),
a subset of SIMD/NEON (DUP, MOVI, LD1/ST1, CNT, CMEQ, UMAXP, SHL, USHR,
EOR, ORR, REV16/32/64, STP/LDP pairs), and system (SVC, MRS/MSR, BRK,
barriers, CLREX).

**Decoder** — `decoder.cpp` is the single source of truth for instruction
decode. It extracts all fields into `DecodedInst` and classifies into
`InstClass`. The interpreter dispatches on `d.cls` via `switch`. A decode
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

See [CHANGELOG.md](CHANGELOG.md) for the complete release history.

## What's New in 1.3.0-beta.2

**Exclusive monitor fix (the big one).** Branches were incorrectly
clearing the exclusive monitor, causing `STXR` to always fail after
any branch between `LDXR` and `STXR`. This broke all atomic operations
that used compare-and-swap loops (spinlocks, refcounting, SDL2 init).
Now only `STXR` and `CLREX` clear the monitor, matching real hardware.

**LDXR decode fix.** `LDXR` with `low6=0x3F` was misclassified as
`LDAR` (which doesn't mark the exclusive monitor). The `o0` bit
distinguishes `LDXR` (o0=0, marks monitor) from `LDAR` (o0=1, no
monitor). Now correctly handled.

**fclose crash fix.** `__stdio_exit` (called during `exit()`) uses
stale buffer pointers that point to freed stack memory. The run loop
now catches `UnmappedMemory` exceptions and breaks gracefully. File
I/O works clean (exit 0).

**SDL2 cross-compiled.** SDL2 2.30.0 built for AArch64 musl static
(minimal config: timers, file, cpuinfo, filesystem). Test program
gets past atomic operations (thanks to the exclusive monitor fix)
but still hangs in musl's mallocng init (known brk/mmap issue).

**Decoder-centric architecture** (from earlier alphas). The decoder
is the single source of truth for instruction decode. `execute()`
calls `decode()` once per instruction, then dispatches via
`switch(d.cls)`. A decode cache (`PC → DecodedInst`) provides ~2x
performance for tight loops.

**Migrated to switch:** ADR/ADRP, MOVN/MOVZ/MOVK, ADD/SUB immediate,
SBFM/BFM/UBFM, EXTR, AND/ORR/EOR/ANDS immediate, plus all branches,
ADC/SBC, and FMOV Vd.D[1].

**Fixes from 1.1.x carried forward:**
- SIMD LDR/STR Q-form (128-bit) — was transferring only 1 byte
- FMOV Vd.D[1], Rn — was unimplemented (broke 128-bit softfloat)
- BFM destination field position — was inserting at bit 0
- ADC/ADCS/SBC/SBCS — were unimplemented
- MOVI Vd.2D, #0 — was only handling byte broadcast form
- PT_NOTE-based LDUR/LSE disambiguation — matches real hardware
- MAP_FIXED overlap handling for mallocng

Full release notes in [CHANGELOG.md](CHANGELOG.md).

## Limitations

This is beta-quality software. Known issues:

- **`malloc`/`free` hangs in mallocng init.** The brk/mmap interaction
  confuses musl's metadata tracking. This also blocks SDL2, toybox, and
  any binary that does nontrivial heap allocation. **This is the #1
  blocker for real applications.**
- **`printf("%f", ...)` hangs.** musl's float formatter enters an
  infinite loop in `__multf3`/`__fixunstfsi`. Integer printf formats
  (`%d`, `%x`, `%c`, `%s`, `%ld`, `%llx`) all work.
- **Function pointer tables in static-PIE binaries** may not relocate
  correctly (`test_fnptr` hits a decode error).
- **No signal delivery** — `rt_sigaction` is a no-op.
- **No dynamic linking** — static binaries only.
- **No ASLR** — binaries load at their preferred vaddr.
- **`toybox-aarch64` crashes at PC=0** — STP/LDP mode calculation bug.
  Fix requires hierarchical decoder restructure, planned for v2.0.
- **glibc 2.36+ static binaries** hit a decode error on an unhandled
  instruction.
- **Legacy if-chain still present** — not all handlers have been
  migrated to the decoder switch (~17 of ~50 classes migrated). The
  if-chain is transitional and will be deleted in v2.0.

## Roadmap

**Short-term (1.3.0):**
1. **Fix `malloc`/`free`** — rewrite brk/mmap interaction (the #1 blocker)
2. Fix `printf("%f")` — audit FP value propagation
3. Migrate remaining if-chain handlers to the switch
4. Fix `test_fnptr` — investigate static-PIE self-relocation
5. More test coverage: threads, signals

**Medium-term (1.4.0):**
1. Signal delivery (`rt_sigaction` + `rt_sigreturn` + trampoline page)
2. SDL2 rendering for the graphics backend (video/audio/input)

**Long-term (2.0+):**
1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS.
2. **Delete the legacy if-chain** — all handlers in the switch.
3. **Game support** — framebuffer/DRM, audio, input. Long-term goal:
   statically-linked ARM64 SDL2 games at playable framerates.

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
