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

  bifrost-emu  v1.0.0-beta.1
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

## Performance

Roughly 20-30 MIPS on a typical desktop. The `fib(30)` test runs 258
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
- **Readable over fast.** The interpreter is a flat switch — easy to
  extend, easy to debug. The v2.0 JIT will share the same decoder
  tables.

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

## What's Implemented

**Instructions** — ~100 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB, UDIV/SDIV, RBIT/REV/CLZ), branches (B/BL/BR/
BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store (immediate, register,
pair, sign-extended), LSE atomics (LDADD/LDCLR/LDEOR/LDSET/CAS/SWP),
acquire/release (STLR/LDAR), a subset of SIMD/NEON (DUP, LD1/ST1, CNT,
CMEQ, UMAXP, SHL, USHR, EOR, REV16/32/64, FMOV), and system (SVC, MRS/
MSR, BRK, barriers).

**Syscalls** — ~50 Linux AArch64 syscalls including the basics
(read/write/openat/close/exit/exit_group/brk/mmap/mprotect/mremap),
file I/O (lseek/fstat/statx/fstatat/readlinkat/statfs/fstatfs),
process info (getpid/gettid/getuid/geteuid/getgid/getegid/uname/
prlimit64), timing (clock_gettime/gettimeofday/nanosleep/
clock_nanosleep), threading (clone, futex with WAIT/WAKE/REQUEUE,
set_tid_address, set_robust_list), event loops (eventfd2, epoll_create1
/epoll_ctl/epoll_wait, timerfd_create/settime/gettime, ppoll, pselect6),
networking stubs (socketpair, listen, accept), and misc (getrandom,
ioctl, getcwd, prctl, rt_sigaction, rt_sigprocmask). Unsupported
syscalls return `-ENOSYS` silently unless `-v` is set.

**TLS** — TPIDR_EL0 / TPIDRRO_EL0 via MRS/MSR; 64KB TLS scratch area
pre-allocated; per-thread TLS via `clone(CLONE_SETTLS, ...)`. Zero
page mapped so NULL dereferences return 0.

**ELF** — Static ELF64 AArch64 (ET_EXEC and ET_DYN); PT_LOAD with
BSS zero-fill; RELA relocations (JUMP_SLOT, GLOB_DAT, RELATIVE, ABS64);
full initial stack with argc/argv/envp/auxv (AT_PHDR, AT_ENTRY,
AT_RANDOM, AT_HWCAP, etc.).

See [CHANGELOG.md](CHANGELOG.md) for the complete list with notes
on each syscall and known issues.

## What's New in 1.0.0-beta.1

- **Threading** — `clone()` and `futex()` are now real implementations
  using OS threads and condition variables. `pthread`-based code should
  work (caveat: no signal delivery yet, so `pthread_kill` won't).
- **Event-loop syscalls** — epoll, timerfd, eventfd, ppoll, pselect6,
  socketpair all delegate to the host kernel.
- **Thread-safe memory** — per-page mutex on every access.
- **BRK is fatal** — matches real Linux `SIGTRAP` semantics (exit 133).
- **Versioning** — `1.0.0-beta.1` semver string, `--version` flag.

Full release notes and known issues in [CHANGELOG.md](CHANGELOG.md).

## Limitations

- No FP/SIMD arithmetic (loads/stores work, but FADD/FMUL etc. are stubbed)
- No signal delivery (`rt_sigaction` is a no-op)
- No exclusive monitor (`STXR` always succeeds — see CHANGELOG)
- No dynamic linking (static binaries only)
- No ASLR (binaries load at their preferred vaddr)
- glibc 2.36+ static binaries hit a getrandom vDSO assertion (musl works)

## Roadmap (v2.0+)

1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS.
2. **FP/SIMD arithmetic** — `FADD`, `FMUL`, `FMLA`, `FCVT`, etc.
3. **Exclusive monitor** — proper LL/SC semantics for `LDXR`/`STXR`.
4. **Signal delivery** — real `rt_sigaction` + `rt_sigreturn`.
5. **Game support** — framebuffer/DRM, audio, input. Long-term goal:
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
