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

  bifrost-emu  v1.3.0-alpha.1
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.3.0-alpha.1](https://img.shields.io/badge/version-1.3.0--alpha.1-orange.svg)](CHANGELOG.md)

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
| `test_fileio.elf` (musl static) | ⚠️ Partial | File prints, then crash on `fclose` |
| `test_float.elf` (musl static) | ❌ Hangs | `printf("%f")` → `__multf3` recursion (partial fix) |
| `test_malloc.elf` (musl static) | ❌ Hangs | mallocng init recursion (partial `MAP_FIXED` fix) |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | Unhandled instruction after mallocng |
| `toybox-aarch64` | ⚠️ Exit 1 | PC=0 (STP/LDP mode bug, planned for v2.0) |

## What's Implemented

**Instructions** — ~120 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB, UDIV/SDIV, RBIT/REV/CLZ), branches (B/BL/BR/
BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store (immediate, register,
pair, sign-extended), LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/
UMAX/UMIN/SWP/CAS), acquire/release (STLR/LDAR), exclusive monitor
(LDXR/STXR/CLREX), **full FP arithmetic** (FADD/FSUB/FMUL/FDIV/FSQRT/
FABS/FNEG/FCMP/FCVT/SCVTF/FCVTZS/FMADD/FMSUB/FCSEL, both S and D
registers with IEEE 754 semantics), a subset of SIMD/NEON (DUP, LD1/ST1,
CNT, CMEQ, UMAXP, SHL, USHR, EOR, REV16/32/64, STP/LDP pairs), and
system (SVC, MRS/MSR, BRK, barriers, CLREX).

**Syscalls** — ~60 Linux AArch64 syscalls including the basics
(read/write/openat/close/exit/exit_group/brk/mmap/mprotect/mremap),
file I/O (lseek/fstat/statx/fstatat/readlinkat/statfs/fstatfs/readv/writev),
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

## What's New in 1.3.0-alpha.1

Significant alpha release with multiple correctness fixes and syscall
expansions. The headline fix is the SIMD load/store bug that broke
128-bit (`str q0`/`ldr q0`) operations — this was silently corrupting
softfloat values on the stack and broke musl's `printf("%f")` path.

- **SIMD LDR/STR Q-form (128-bit) fix** — `str q0`/`ldr q0` were
  incorrectly decoded as 1-byte (B-form) transfers because the LSE
  handler's `opc` field interpretation was wrong. The correct encoding:
  `opc=10` → STR Q (128-bit), `opc=11` → LDR Q (128-bit), `opc=00`/`01`
  with size → B/H/S/D forms. Fixed in all three load/store handlers
  (unsigned-offset, pre/post-indexed, register-offset).
- **`FMOV Vd.D[1], Rn` / `FMOV Rn, Vm.D[1]`** — these move a 64-bit GPR
  to/from the HIGH 64 bits of a vector register. Used heavily by musl's
  128-bit softfloat routines (`__multf3`, `__addtf3`, `__eqtf2`, etc.)
  to construct long doubles from two GPRs. Previously unimplemented.
- **`BFM` (bitfield move) fix** — the destination field position was
  wrong: BFM was inserting source bits at position 0 instead of at the
  `[immr..imms]` field position. This broke `bfi` (bitfield insert),
  which musl uses to assemble FP exponent/mantissa fields.
- **`ADC`/`ADCS`/`SBC`/`SBCS`** — add/subtract with carry. Previously
  unimplemented; caused decode errors in softfloat routines that use
  multi-precision arithmetic.
- **More syscalls** — added `dup` (23), `dup2` (33), `pipe2` (59),
  `mkdirat` (34), `unlinkat` (35), `renameat` (38), `utimensat` (88),
  `fstatat` (79). Total syscall count now ~88.
- **`MAP_FIXED` overlap handling** — when musl's mallocng uses `MAP_FIXED`
  on an address inside the brk region, the brk is now pushed past the
  mmap'd area to avoid corrupting heap metadata. (Partial fix — see
  Known Limitations.)

Also includes all fixes from 1.1.1-alpha.1 (PT_NOTE-based LDUR/LSE
disambiguation), 1.1.0-rc.2 (FP/SIMD arithmetic, VFS, file split,
readv fix, SIMD STP/LDP) and earlier releases.

Full release notes in [CHANGELOG.md](CHANGELOG.md).

## What's New in 1.1.0-rc.2

Major refactor: split the monolithic `arm64_emu.cpp` into separate files,
added real FP/SIMD arithmetic, VFS, and a public API header.

- **FP/SIMD arithmetic** — FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FCVT/
  SCVTF/FCVTZS/FMADD/FMSUB/FCSEL, all with IEEE 754 semantics (both S and D
  registers). Previously all FP was stubbed as NOP.
- **VFS** — `/proc/self/{exe,cmdline,maps,status,auxv,environ}`,
  `/proc/{meminfo,cpuinfo,version}`, `/dev/{null,zero,urandom,random}`.
- **File split** — `decoder.hpp/cpp`, `interpreter.cpp`, `syscalls.cpp`,
  `graphics.hpp/cpp`, `api/bifrost.h`. Decoder is shared with future JIT.
- **readv fix** — was case 73 (pselect6!), now case 67 (readv).
- **SIMD STP/LDP** — 32/64/128-bit pair store/load (was silently dropped).

Also includes all fixes from 1.1.0-beta.1 (LSE atomics, CAS argument order,
exclusive monitor, mremap in-place, mmap MAP_FIXED, getppid, stack 64MB).

Full release notes in [CHANGELOG.md](CHANGELOG.md).

## Limitations

This is alpha-quality software. Known issues:

- **`printf("%f", ...)` still hangs in some cases.** The SIMD LDR/STR
  fix resolved the stack corruption that caused the original infinite
  recursion in `__multf3`. However, musl's `__fmt_fp` (float formatter)
  now enters a different loop involving `__fixunstfsi`. Investigating.
  Integer printf formats (`%d`, `%x`, `%c`, `%s`, `%ld`, `%llx`) all work.
- **`malloc`/`free` hangs in musl's mallocng init.** The `MAP_FIXED`
  overlap fix helps, but the brk/mmap interaction still confuses
  musl's metadata tracking. This also blocks toybox and any binary
  that does nontrivial heap allocation.
- **Function pointer tables in static-PIE binaries** may not relocate
  correctly. Test `test_fnptr` hits a decode error because the function
  pointer ends up pointing at `.rodata` instead of the function.
- **`fclose` crash** in `test_fileio` — file contents print correctly
  but `__stdio_exit` calls `memchr` on a garbage pointer.
- **No signal delivery** — `rt_sigaction` is a no-op. Planned for 1.2.0.
- **No dynamic linking** — static binaries only.
- **No ASLR** — binaries load at their preferred vaddr.
- **`toybox-aarch64` crashes at PC=0** — STP/LDP mode calculation bug.
  Fix requires hierarchical decoder restructure, planned for v2.0.
- **glibc 2.36+ static binaries** hit a decode error on an unhandled
  instruction.

## Roadmap

**Short-term (1.3.0-beta.1 / 1.3.0):**
1. Fix `printf("%f")` — audit `__fixunstfsi` and FP value propagation.
2. Fix `malloc`/`free` — rewrite brk/mmap interaction.
3. Fix `test_fnptr` — investigate static-PIE self-relocation conflict.
4. Fix `test_fileio` `fclose` crash — stdio cleanup bug.
5. Performance: decoded instruction cache.
6. More test coverage: threads, signals.

**Medium-term (1.2.0):**
1. Signal delivery (`rt_sigaction` + `rt_sigreturn` + trampoline page).
2. SDL2 rendering for the graphics backend (1.3.0).

**Long-term (2.0+):**
1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS.
2. **Hierarchical decoder restructure** — fixes the STP/LDP mode bug
   that breaks toybox.
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
