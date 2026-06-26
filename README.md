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

  bifrost-emu  v1.4.0-beta.3
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.4.0-beta.3](https://img.shields.io/badge/version-1.4.0--beta.3-orange.svg)](CHANGELOG.md)

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

# Enable the JIT (39/39 tests pass)
./bifrost-emu --jit ctest_real/fib.elf

# Show version
./bifrost-emu --version
```

No args? You get the banner. Try `--bifrost` for a hidden easter egg.

## Usage

```
bifrost-emu [options] <elf-file> [args...]

Options:
  -d, --debug     trace every instruction to stderr
  -v, --verbose   print execution stats on exit
  -V, --version   show version and exit
  -h, --help      show help
  --jit           enable frostJIT (experimental block-translation JIT)
  --fb-dump PATH  dump the /dev/fb0 framebuffer to PATH on exit (PPM format)
  --audio-dump PATH  dump audio PCM to PATH on exit (WAV format)
  --raw-tty       force raw TTY mode (per-character input, no echo)
  -q, --quiet     suppress BRK warnings (even with -d)
```

The `--fb-dump PATH` option syncs the guest's `/dev/fb0` writes back to
the host and writes a PPM image to `PATH` on exit. Useful for headless
debugging of programs that draw to the framebuffer.

## Build

```bash
make          # release build with -O3
make debug    # debug build with ASan + UBSan
make test     # run the test suite
make lib      # build libbifrost.a (static library for API consumers)
make install  # install to /usr/local/bin/
```

No external libraries required for the default build. Only standard C++
and POSIX. For the SDL2 window backend: `make USE_SDL2=1` (requires
`libsdl2-dev`, or run `./tools/fetch-sdl2-headers.sh` to download
SDL2 headers via `apt-get download` without a system-wide install).

To cross-compile test programs with the bundled musl toolchain:

```bash
make tools/fetch-musl-toolchain.sh   # download toolchain (one-time)
make cross SRC=ctest/hello.c OUT=ctest/hello.elf
```

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

The **decoder** (`src/frontend/decoder.cpp`) is the single source of truth
for instruction decode. It is a true two-level hierarchical switch:

- **Outer switch** on bits `[28:24]` — the 5-bit "major encoding group"
  selector from the ARM ARM top-level encoding table. This routes each
  instruction to one of 32 cases (most reserved), preventing the encoding
  collisions that plague flat mask-and-compare decoders.
- **Inner switch** on the group-specific discriminator — typically
  bits `[31:29]` (opc/sf), bit 26 (V), bit 23, bit 22, the addressing-
  mode bits, or a sub-opcode field, depending on the group.

The decoder extracts all fields into a `DecodedInst` struct and
classifies the instruction into an `InstClass` enum value. It does NO
execution — only bit extraction and classification.

The **interpreter** (`src/interp/interpreter.cpp`) dispatches on `d.cls`
via a flat `switch` statement. Each case reads `d.*` fields and executes.
The interpreter never does bit extraction — that's the decoder's job.

A **decode cache** (`PC → DecodedInst`) avoids re-decoding the same
instruction on repeated execution (tight loops). Since guest code is not
self-modifying (static binaries only), each PC always decodes to the same
instruction.

**frostJIT** (`src/jit/frostjit.cpp`) is an optional block-translation
JIT that translates AArch64 basic blocks into x86_64 machine code in a
64MB `mmap`'d RWX code cache. It shares the decoder with the interpreter
and falls back to single-step interpretation for unsupported instructions.
Enable with `--jit`. As of beta.3 (2026-06-26), all 39 test programs
pass under JIT.

## Performance

The interpreter achieves ~96 MIPS on compute-heavy workloads. The
experimental frostJIT (`--jit`) achieves **573 MIPS** on `bench_mips`
(5.9x faster than the interpreter) via:

1. **Self-loop chaining** — tight loops jump directly back to the block
   body, skipping the epilogue/dispatcher/prologue.
2. **Liveness-based register freeing** — dead vregs' host regs are freed
   immediately after their last use, eliminating eviction spills.
3. **Register-cache-aware ALU codegen** — operands stay in whatever host
   regs they're cached in, instead of being forced into RAX/RCX.
4. **Decode cache** — a flat array indexed by PC avoids re-decoding on
   repeated execution (tight loops get 100% hit rate).
5. **Memory page cache** — single-entry last-page caches for read and
   write, avoiding mutex lock + hash-map lookup on every memory access
   to the same page.

Run `bifrost-emu -v <elf>` to see MIPS, memory page count, and decode
cache hit rate for any program.

## Design Philosophy

- **Silent by default.** Only the emulated program's output appears.
  No stats, no exit codes, no noise.
- **Debug when you need it.** `-d` traces every instruction. `-v`
  prints stats.
- **No binary-specific hacks.** Zero hardcoded addresses. Any static
  AArch64 ELF should work.
- **Decoder is the single source of truth.** All decode logic lives in
  `decoder.cpp`. The interpreter and JIT both share the same decoder.
- **Hang watchdog.** A safety net in the run loop catches infinite loops
  and aborts with a diagnostic instead of spinning forever.

## File Structure

```
bifrost-emu/
├── include/
│   ├── bifrost/          Public API headers (types, version, emulator)
│   ├── core/             CPU, Memory, Emulator, Signal, ThreadMgr
│   ├── jit/              frostjit.hpp — block translator interface
│   ├── ir/               IR block / IR inst definitions
│   └── decoder.hpp       DecodedInst struct, InstClass enum
├── src/
│   ├── core/             Emulator, Memory, CPU, Signal, ThreadMgr
│   ├── frontend/         decoder.cpp + elf_loader.cpp
│   ├── interp/           interpreter.cpp (switch on d.cls)
│   ├── ir/               IR builder, translator, optimizer, lowerer
│   ├── jit/              frostjit.cpp, x86_backend, x86_regalloc, cache
│   ├── syscalls/         Linux AArch64 syscall layer (~88 syscalls)
│   ├── vfs/              Virtual filesystem (VNode + FdTable + procfs + devfs)
│   ├── graphics/         /dev/fb0 backend (headless or SDL2)
│   └── audio/            OSS /dev/dsp passthrough + WAV dump
├── api/bifrost.h         Public C API for libbifrost
├── test/                 Sample ARM64 programs (.s sources + assembled .elf)
├── ctest/                C test programs (musl-static) + jit_*.elf suites
├── ctest_real/           Real-world Unix utilities + toybox binary
├── tools/                musl/glibc toolchain fetch scripts
├── Makefile              Build, test, install, cross-compile targets
├── CHANGELOG.md          Release history (per-commit detail)
├── ROADMAP.md            Planned development trajectory
├── TESTS.md              Test programs and current status
├── README.md             This file
└── LICENSE               Public domain (Unlicense)
```

## What's Implemented

**Instructions** — ~140 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH,
UDIV/SDIV, RBIT/REV/CLZ/CLS, ADC/SBC with carry), branches
(B/BL/BR/BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store (immediate,
register, pair, sign-extended, unscaled, pre/post-index), LSE atomics
(LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS), acquire/release
(STLR/LDAR), exclusive monitor (LDXR/STXR/CLREX), FP arithmetic
(FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FCVT/SCVTF/FCVTZS/FMADD/FMSUB/
FCSEL, both S and D registers), FMOV Vd.D[1], a subset of SIMD/NEON
(DUP, MOVI, LD1/ST1, CNT, CMEQ, UMAXP, SHL, USHR, EOR, ORR, AND, BIC,
REV16/32/64, STP/LDP pairs, EXT, INS, TBL/TBX), and system (SVC, MRS/MSR,
BRK, HLT, CLREX, HINT, barriers).

**Syscalls** — ~88 Linux AArch64 syscalls including file I/O (read/write/
openat/close/readv/writev/statx/fstatat), process info (getpid/gettid/
uname/prlimit64), timing (clock_gettime/nanosleep/clock_nanosleep),
threading (clone, futex, set_tid_address), event loops (eventfd2, epoll,
timerfd, ppoll), and misc (getrandom, ioctl, getcwd, rt_sigaction).
Unsupported syscalls return `-ENOSYS` silently unless `-v` is set.

**VFS** — `/proc/self/{exe,cmdline,maps,status,auxv,environ}`,
`/proc/{meminfo,cpuinfo,version}`, `/dev/{null,zero,urandom,random,tty}`,
`/dev/{fb0,dsp,snd}`. Uses `memfd_create` for seekable virtual file
descriptors.

**TLS** — TPIDR_EL0 / TPIDRRO_EL0 via MRS/MSR; 64KB TLS scratch area
pre-allocated; per-thread TLS via `clone(CLONE_SETTLS, ...)`.

**ELF** — Static ELF64 AArch64 (ET_EXEC and ET_DYN, including static-PIE);
PT_LOAD with BSS zero-fill; RELA relocations; PT_NOTE parsing for GNU
property features (LSE detection); full initial stack with argc/argv/
envp/auxv.

**Graphics** — Virtual `/dev/fb0` framebuffer (memfd-backed, mmap-able).
`FBIOGET_VSCREENINFO`/`FSCREENINFO` ioctls. Default 640x480@32bpp BGRA.
Headless: `--fb-dump PATH` writes a PPM image on exit. Optional SDL2
window backend via `make USE_SDL2=1`.

**Audio** — OSS `/dev/dsp` passthrough with in-memory PCM buffering.
`--audio-dump PATH` writes a WAV file on exit (16-bit, 44100Hz, stereo).

## Test Status

All 35 JIT test programs pass under both the default interpreter path and
`--jit`. See [TESTS.md](TESTS.md) for the full test matrix, including
toybox compatibility (`echo`, `ls /`, `od`, `head`, `sort`, `rev`, `wc`,
`cat`, `printf "%g"` all work; `seq` produces no output — see Limitations).

Run the test suite:

```bash
make test     # run all tests under interpreter + JIT
make verify   # JIT divergence checker (slow, catches codegen bugs)
```

## Limitations

This is beta-quality software. Key limitations:

- **Limited dynamic linking.** The dynamic linker (PT_INTERP) is loaded
  and its entry point is used, allowing simple dynamically-linked musl
  binaries to run. However, full dynamic linking (DT_NEEDED processing,
  runtime relocations, glibc support) is not yet complete. Static
  binaries are recommended. Full dynamic linking and glibc support are
  planned for v2.0 (see [ROADMAP.md](ROADMAP.md)).
- **No ASLR.** Binaries load at their preferred vaddr.
- **`fork()` is stubbed.** `clone()` without `CLONE_VM` returns 0 (vfork
  semantics). Real fork with copy-on-write is planned for v1.4.x.
- **Signal delivery is partial.** `rt_sigaction` installs handlers and
  `kill`/`tgkill` deliver signals, but `siginfo_t`/`ucontext_t` contents,
  `SA_RESTART`, and signal masks are not fully implemented.
- **frostJIT (`--jit`) is experimental.** All 35 tests pass, but the
  JIT has not been exhaustively tested against arbitrary ARM64 binaries.
  The interpreter path is the default for production use.
- **`strtod()` now works** for all decimal and exponential inputs
  (`"0.5"`, `"1.5"`, `"1e1"`, etc.) — a 32-bit ASR sign-extension bug
  was causing it to return `inf` with `ERANGE` for any input containing
  a decimal point or exponent. `strtod("inf")` still returns `-nan`
  (separate inf/nan string-parsing issue).
- **toybox `seq`** now works — `seq 1 5` outputs `1 2 3 4 5`. Two bugs
  were fixed: SCVTF (int→double) was misdecoded as FMOV (raw GPR bit
  copy), and FMADD's operand sources were reading from scratch vregs
  instead of FP register indices. All seq variants work: `-w` (width
  padding), `-s` (separator), `-f` (format), negative steps, float
  steps.
- **`BIFROST_ENABLE_FWD=1`** (arm_reg_cache load-forwarding) is an
  opt-in IR optimization that gives ~1.2x speedup on bench_mips. All
  JIT tests pass with it enabled, but toybox `ls /` still crashes
  (pre-existing, not introduced by the FWD bug fixes).

For the full development roadmap, see [ROADMAP.md](ROADMAP.md).

## Release History

See [CHANGELOG.md](CHANGELOG.md) for the full per-commit history. The
current release is **v1.4.0-beta.3** (2026-06-26), which includes:

- SCVTF/FMOV decode + FMADD operand fix — `toybox seq` now works
  (`seq 1 5` outputs `1 2 3 4 5`). SCVTF (int→double) was misdecoded
  as FMOV (raw bit copy); FMADD's operands were reading scratch vregs
  instead of FP register indices.
- 32-bit ASR sign-extension fix — `strtod()` now works for all decimal
  and exponential inputs (`"0.5"`, `"1.5"`, `"1e1"`, etc.). Previously,
  a 32-bit ASR bug in `neg w0, w0, asr #1` caused musl's `__floatscan`
  to take the overflow path and return `inf` with `ERANGE` for any
  input with a decimal point or exponent.
- JIT `mrs xN, fpsr/fpcr` fix — was reading 8 bytes instead of 4,
  leaking adjacent `TPIDR_EL0` into the result.
- JIT performance overhaul — 573 MIPS on bench_mips (5.9x speedup over
  interpreter) via self-loop chaining, liveness-based register freeing,
  and register-cache-aware ALU codegen
- JIT FP correctness overhaul — all 39 tests now pass under JIT
  (FCMP #0.0 form detection, FP 1-source opcode extraction,
  FMOV imm mask, FMOV imm vs SCVTF collision, missing interp FCMP)
- Shared `fp_decode` helpers in `decoder.hpp` to keep interpreter
  and JIT's IR translator in sync
- JIT critical correctness fixes (FCMP prefix, CSEL/BRCOND flag
  resolution, CF normalization, ADCS/SBCS carry convention)
- Audio backend (OSS `/dev/dsp` passthrough + WAV dump)
- VFS abstraction (VNode + FdTable + procfs + devfs)
- ~16 new syscalls (networking, inotify, statx)
- frostJIT: native UDIV/SDIV, SMADDL/UMADDL, MRS/MSR, 15 new FP instructions

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
