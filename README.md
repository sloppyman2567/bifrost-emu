# bifrost-emu

**A bridge between worlds** — a fast, simple ARM64 (AArch64) Linux user-mode
emulator written in C++17. Runs static AArch64 ELF binaries on any x86_64
Linux host without needing qemu or a cross-compiler.

```
  ██████╗ ██╗██████╗ ███████╗██████╗
  ██╔══██╗██║██╔══██╗██╔════╝██╔══██╗
  ██████╔╝██║██║  ██║█████╗  ██████╔╝
  ██╔══██╗██║██║  ██║██╔══╝  ██╔══██╗
  ██████╔╝██║██████╔╝███████╗██║  ██║
  ╚═════╝ ╚═╝╚═════╝ ╚══════╝╚═╝  ╚═╝

  bifrost-emu  v1.0.0-beta.1
  A bridge between worlds
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: beta](https://img.shields.io/badge/version-1.0.0--beta.1-orange.svg)](CHANGELOG.md)

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

## What's New in 1.0.0-beta.1

- **Threading!** `clone()` and `futex()` are now real implementations
  using OS threads and condition variables. Multi-threaded ARM64
  programs that use `pthread` should work (caveat: no signal delivery
  yet, so `pthread_kill` won't).
- **Event-loop syscalls**: `epoll`, `timerfd`, `eventfd`, `ppoll`,
  `pselect6`, `socketpair`, and friends — all delegate to the host
  kernel.
- **Thread-safe memory**: per-page mutex on every access.
- **BRK is fatal**: matches real Linux `SIGTRAP` semantics.
- **Versioning**: `1.0.0-beta.1` semver string, `--version` flag.
- **GitHub-ready**: `LICENSE`, `.gitignore`, `CHANGELOG.md`,
  `CONTRIBUTING.md`, `Makefile`.

See [CHANGELOG.md](CHANGELOG.md) for the full history and known issues.

## Design Philosophy

- **Silent by default.** Only the emulated program's output appears.
  No stats, no exit codes, no noise.
- **Debug when you need it.** `-d` traces every instruction. `-v`
  prints stats.
- **No binary-specific hacks.** Zero hardcoded addresses. Any static
  AArch64 ELF should work (modulo the known issues below).
- **Readable over fast.** The interpreter is a flat switch — easy to
  extend, easy to debug. A JIT is planned for v2.0.

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

## What's Included

| File | Description |
|------|-------------|
| `bifrost-emu` | Compiled binary (build with `make`) |
| `arm64_emu.hpp` | Header: Memory, CPU, ELF loader, Emulator class |
| `arm64_emu.cpp` | Instruction decoder + syscall layer |
| `main.cpp` | CLI entry point with easter egg |
| `mini_arm64_asm.py` | Built-in ARM64 assembler (produces static ELF binaries) |
| `test/` | Sample ARM64 programs |
| `Makefile` | Build, test, install targets |
| `CHANGELOG.md` | Versioned release history |
| `CONTRIBUTING.md` | How to contribute |
| `LICENSE` | Public domain / Unlicense |

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

## Implemented Instructions

**Data processing:**
- MOVZ / MOVK / MOVN (wide immediate)
- ADD / SUB / ADDS / SUBS / CMP / CMN (immediate, shifted register, extended register)
- AND / ORR / EOR / ANDS / TST (immediate, shifted register)
- SBFM / BFM / UBFM / EXTR (bitfield)
- CSEL / CSINC / CSINV / CSNEG (conditional select)
- CCMP / CCMN (conditional compare)
- MUL / MADD / MSUB / UMADDL / SMADDL / UMSUBL / SMSUBL
- UMULH / SMULH (high-half multiply)
- UDIV / SDIV / LSL / LSR / ASR / ROR
- RBIT / REV16 / REV32 / REV / CLZ / CLS

**Branches:**
- B / BL / BR / BLR / RET
- B.cond (all 14 condition codes)
- CBZ / CBNZ / TBZ / TBNZ

**Memory:**
- Load/store (immediate: unsigned offset, unscaled, pre/post-indexed)
- Load/store (register offset)
- Load/store pair (offset, pre/post-indexed)
- LDRSW / LDRS[BH] (sign-extended loads)

**Atomics & exclusives:**
- STXR / LDXR / STLXR / LDAXR
- STLR / LDAR (acquire/release)
- LSE atomics: LDADD / LDCLR / LDEOR / LDSET / CAS / SWP

**SIMD/NEON (subset for glibc/musl optimized routines):**
- DUP, INS, MOVI
- LD1 / ST1 (vector load/store)
- CNT, UADDLV (for strlen)
- CMEQ, CMHS (vector compare)
- UMAXP / UMINP (pairwise max/min)
- SHL, USHR, SHRN (vector shifts)
- EOR, AND, ORR, BIC (vector logical)
- REV16, REV32, REV64 (vector byte reversal)
- FMOV (general ↔ FP, scalar)
- TBL / TBX (table lookup, stubbed)

**System:**
- SVC #0 (syscall)
- MRS / MSR (TPIDR_EL0, TPIDRRO_EL0, NZCV, FPCR, FPSR, CTR_EL0, DCZID_EL0, MIDR_EL1)
- CLREX, DSB, DMB, ISB, NOP, YIELD
- BRK (fatal — raises SIGTRAP, exits with status 133)

## Implemented Linux Syscalls (AArch64 numbers)

| # | Name | Notes |
|---|------|-------|
| 19 | eventfd2 | delegates to host |
| 20 | epoll_create1 | delegates to host |
| 21 | epoll_ctl | delegates to host |
| 22 | pipe2 | (also aarch64 epoll_pwait — see Known Issues) |
| 24 | dup3 | |
| 25 | fcntl | stub |
| 29 | ioctl | TIOCGWINSZ handled |
| 40 | sendfile | (real aarch64 # is 71 — see Known Issues) |
| 43 | statfs | |
| 44 | fstatfs | |
| 56 | openat | |
| 57 | close | |
| 62 | lseek | |
| 63 | read | |
| 64 | write | |
| 66 | writev | |
| 72 | pselect6 | delegates to host |
| 78 | readlinkat | |
| 79 | fstatat | |
| 80 | fstat | |
| 84 | semget | stub (returns -ENOSYS) |
| 85 | timerfd_create | delegates to host |
| 86 | timerfd_settime | delegates to host |
| 87 | timerfd_gettime | delegates to host |
| 93 | exit | |
| 94 | exit_group | |
| 96 | set_tid_address | per-thread state |
| 98 | futex | real impl (WAIT/WAKE/REQUEUE) |
| 99 | set_robust_list | no-op |
| 100 | nanosleep | |
| 113 | clock_gettime | |
| 130 | tkill | no-op (no signal delivery) |
| 131 | tgkill | no-op |
| 133 | rt_sigreturn | stub |
| 134 | rt_sigaction | no-op |
| 135 | rt_sigprocmask | no-op |
| 160 | uname | advertises Linux 6.5.0 |
| 163 | acct | -EPERM |
| 165 | getcwd | |
| 167 | prctl | no-op |
| 168 | ppoll | delegates to host |
| 169 | gettimeofday | |
| 172 | getpid | |
| 174 | getuid | |
| 175 | geteuid | |
| 176 | getgid | |
| 177 | getegid | |
| 178 | gettid | returns guest TID |
| 198 | socket | -ENOSYS |
| 199 | socketpair | delegates to host |
| 200 | bind | -ENOSYS (sockaddr marshalling) |
| 201 | listen | delegates to host |
| 202 | accept | delegates to host |
| 203 | connect | -ENOSYS (sockaddr marshalling) |
| 206 | clock_nanosleep | |
| 214 | brk | thread-safe |
| 215 | munmap | no-op |
| 220 | clone | real impl (threads only, no fork) |
| 221 | clone3 | -ENOSYS |
| 222 | mmap | anonymous + file-backed |
| 226 | mprotect | no-op |
| 227 | mremap | |
| 233 | madvise | no-op |
| 261 | prlimit64 | returns RLIM_INFINITY |
| 278 | getrandom | reads /dev/urandom |
| 291 | statx | |
| 293 | rseq | -ENOSYS |
| — | others | -ENOSYS (silent unless -v) |

## TLS Support

- TPIDR_EL0 / TPIDRRO_EL0 are properly read/written via MRS/MSR
- A 64KB TLS scratch area is pre-allocated before program start
- Per-thread TLS via `clone(CLONE_SETTLS, ...)` is supported
- The zero page (0x0-0xFFF) is mapped so NULL dereferences return 0

## ELF Loading

- Static ELF64 AArch64 (ET_EXEC and ET_DYN)
- PT_LOAD segments with BSS zero-fill
- RELA relocations (R_AARCH64_JUMP_SLOT, GLOB_DAT, RELATIVE, ABS64)
- Initial stack with argc, argv, envp, and auxv (AT_PHDR, AT_ENTRY,
  AT_RANDOM, AT_HWCAP, etc.)

## Performance

Roughly 20-30 MIPS on a typical desktop. The fib(30) test runs 258
instructions in under 1ms. musl static hello world runs 1,760
instructions in under 0.1ms.

A JIT is planned for v2.0 — see [CONTRIBUTING.md](CONTRIBUTING.md)
for the roadmap.

## Compatibility

| Binary | Status | Notes |
|--------|--------|-------|
| `hello.elf` (assembled) | ✅ Works | |
| `count.elf` (assembled) | ✅ Works | |
| `fib.elf` (assembled) | ✅ Works | |
| `cat.elf` (assembled) | ✅ Works | |
| `echo.elf` (assembled) | ✅ Works | Interactive, raw TTY |
| `repl.elf` (assembled) | ✅ Works | Line-buffered |
| `hello_arm64_musi` | ✅ Works | Full musl static |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | getrandom vDSO assertion |
| `toybox-aarch64` | ❌ Hangs | musl malloc loop (atomics) |

## Limitations

- No FP/SIMD arithmetic (loads/stores work, but FADD/FMUL etc. are stubbed)
- No signal delivery (rt_sigaction is a no-op)
- No exclusive monitor (STXR always succeeds — see Known Issues)
- No dynamic linking (static binaries only)
- No ASLR (binaries load at their preferred vaddr)
- glibc 2.36+ static binaries hit a getrandom vDSO assertion (musl works)

See [CHANGELOG.md](CHANGELOG.md) for the full list of known issues
and the v2.0 roadmap.

## Build

```bash
make          # release build with -O3
make debug    # debug build with ASan + UBSan
make test     # run the test suite
make install  # install to /usr/local/bin/
```

No external libraries required. Only standard C++ and POSIX.

## Roadmap (v2.0+)

1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS.
2. **FP/SIMD arithmetic** — `FADD`, `FMUL`, `FMLA`, `FCVT`, etc.
3. **Exclusive monitor** — proper LL/SC semantics for `LDXR`/`STXR`.
4. **Signal delivery** — real `rt_sigaction` + `rt_sigreturn`.
5. **Game support** — framebuffer/DRM, audio, input. Long-term goal:
   statically-linked ARM64 SDL2 games at playable framerates.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full roadmap.

## License

Public domain. Use freely. See [LICENSE](LICENSE) for details.

## Acknowledgments

Inspired by [qemu-user](https://www.qemu.org/docs/master/user/main.html),
[box64](https://github.com/ptitSeb/box64), and
[FEX-Emu](https://github.com/FEX-Emu/FEX). Written from scratch as a
learning project; the goal is readability, not performance (yet).
