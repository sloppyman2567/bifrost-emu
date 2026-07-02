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

  bifrost-emu  v1.4.0
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.4.0](https://img.shields.io/badge/version-1.4.0--rc.1-orange.svg)](CHANGELOG.md)

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

# JIT is ON by default (41/41 tests pass, 6.4x speedup on compute)
./bifrost-emu ctest_real/fib.elf

# Use --no-jit to force the interpreter (fallback / debugging)
./bifrost-emu --no-jit ctest_real/fib.elf

# Run toybox — a real-world AArch64 multicall binary
./bifrost-emu toybox echo hello world
./bifrost-emu toybox ls /
./bifrost-emu toybox seq 1 10
./bifrost-emu toybox sh -c 'echo $((3+4))'

# Fork + execve works — run external AArch64 commands from sh
mkdir -p /tmp/aarch64-bin
ln -sf /path/to/toybox-aarch64 /tmp/aarch64-bin/cat
ln -sf /path/to/toybox-aarch64 /tmp/aarch64-bin/seq
./bifrost-emu toybox sh -c 'seq 1 5'

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
  --jit           enable frostJIT (now the default; kept for compatibility)
  --no-jit        disable JIT and use the interpreter (fallback / debugging)
  --jit-threshold N  use interpreter for first N instructions, then switch
                     to JIT (avoids compilation overhead for short programs;
                     default 0 = use JIT from start)
  --              end of options; next arg is the ELF file (POSIX convention)
  --fb-dump PATH  dump the /dev/fb0 framebuffer to PATH on exit (PPM format)
  --audio-dump PATH  dump audio PCM to PATH on exit (WAV format)
  --raw-tty       force raw TTY mode (per-character input, no echo)
  -q, --quiet     suppress BRK warnings (even with -d)
```

Environment variables:
- `BIFROST_NATIVE_DYNLINK=1` — use the in-emulator dynamic linker instead
  of loading the guest-side ld.so. Processes DT_NEEDED, applies
  relocations, resolves symbols, and allocates TLS blocks natively.
- `BIFROST_JIT_VERIFY=1` — run the JIT divergence checker (compares JIT
  results against the interpreter for every block). Verify mode now
  un-patches both the regular chain slot and the self-loop slot before
  running each block, eliminating the self-loop chaining false positives
  that dominated the output in earlier releases. A `verified_once` flag
  on each block makes verify mode skip the divergence check on second
  and subsequent dispatches — first-dispatch verify still catches real
  codegen bugs, but the per-iteration overhead is gone (~9× speedup).
- `BIFROST_ENABLE_FWD=1` — enable experimental load-forwarding in the IR
  optimizer (~5.6% speedup, has known correctness bugs with some toybox
  commands).
- `BIFROST_SYSCALL_TRACE=1` — trace syscall invocations to stderr.
- `BIFROST_NO_CHAIN=1` — disable lazy block chaining (for debugging).
- `BIFROST_NO_SELFLOOP=1` — disable self-loop chaining (for debugging).
- `BIFROST_NO_FMA3=1` — force the JIT to use the decomposed
  `mulsd`+`addsd` codegen for FMADD/FMSUB/FNMADD/FNMSUB even on host
  CPUs that support FMA3. Useful for A/B-testing the FMA3 codegen
  against the decomposed path on the same machine, or as a workaround
  if an FMA3 codegen bug is suspected. By default, the JIT detects
  FMA3+AVX support at startup (via CPUID+XGETBV) and emits native
  `vfmadd231ss/sd`, `vfnmadd231ss/sd`, `vfnmsub231ss/sd` — giving
  both IEEE 754-correct single-rounded fused mul-add (addressing the
  long-standing "FMADD not truly fused" limitation) and ~1 cycle per
  FMADD savings.

The `--fb-dump PATH` option syncs the guest's `/dev/fb0` writes back to
the host and writes a PPM image to `PATH` on exit. Useful for headless
debugging of programs that draw to the framebuffer.

## Build

```bash
make          # release build with -O3
make debug    # debug build with ASan + UBSan
make test     # run the test suite (basic loop over all .elf files)
make check    # run the categorized test runner (colorized, summary table)
make lib      # build libbifrost.a (static library for API consumers)
make install  # install to /usr/local/bin/
```

The `make check` target runs `scripts/run_tests.sh`, which provides:
- Categorized tests (unit, integration, toybox, bench)
- Colorized pass/fail output with timing
- Pattern-based pass detection (checks output for expected keywords)
- Summary table with counts

```bash
make check              # run all tests (JIT, default)
make check-quick        # skip slow benchmarks
make check-nojit        # run under interpreter (--no-jit)
make check-fwd          # run with BIFROST_ENABLE_FWD=1
make check ARGS="--toybox"      # only toybox tests
make check ARGS="--filter md5"  # only tests matching "md5"
./scripts/run_tests.sh --help   # see all options
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

**frostJIT** (`src/jit/frostjit.cpp`) is the default block-translation
JIT that translates AArch64 basic blocks into x86_64 machine code in a
64MB `mmap`'d RWX code cache. It shares the decoder with the interpreter
and falls back to single-step interpretation for unsupported instructions.
JIT is ON by default; use `--no-jit` to opt out. As of rc.1 (2026-06-27),
all 41 test programs pass under JIT, including the
`ctest/jit_int_fp_conv.elf` covering all 8 variants of int↔FP conversion,
the new `ctest/jit_fma.elf` covering FMADD/FMSUB/FNMADD/FNMSUB in both
single and double precision, and 19 real-world C programs in `ctest_real/`.

## Performance

Measured on x86_64 Linux (Debian 14, g++ -O3), 10-run averages:

| Workload | Interpreter | frostJIT | JIT + FWD |
|----------|------------|----------|-----------|
| bench_mips (compute) | 89 MIPS | **571 MIPS** (6.4x) | **604 MIPS** (6.8x) |
| toybox seq 1 10000 (I/O) | 28 MIPS | 26 MIPS (0.9x) | — |

The JIT excels at long-running compute-intensive workloads (loops, math,
crypto) where its 6.4x throughput advantage amortizes the one-time block
compilation cost. For short or I/O-bound programs (cat, ls, seq), the
interpreter is faster because it has zero compilation overhead. Break-even
is approximately 1–2 million instructions.

The JIT achieves this via:

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
│   ├── jit/              frostjit.hpp — block translator interface
│   ├── ir/               IR block / IR inst definitions
│   ├── arm64_emu.hpp     Legacy umbrella header (redirects to bifrost/)
│   ├── graphics.hpp      Framebuffer / SDL2 interface
│   └── decoder.hpp       DecodedInst struct, InstClass enum, fp_decode helpers
├── src/
│   ├── core/             Emulator, Memory, CPU, Signal, ThreadMgr (headers + .cpp)
│   ├── frontend/         decoder.cpp + elf_loader.cpp
│   ├── interp/           interpreter.cpp (switch on d.cls)
│   ├── ir/               IR builder, translator, optimizer, lowerer, executor
│   ├── jit/              frostjit.cpp, x86_backend, x86_regalloc, cache, profiler
│   ├── syscalls/         Linux AArch64 syscall layer (~88 syscalls, split by concern)
│   ├── vfs/              Virtual filesystem (VNode + FdTable + procfs + devfs)
│   ├── graphics/         /dev/fb0 backend (headless or SDL2)
│   └── audio/            OSS /dev/dsp passthrough + WAV dump
├── api/bifrost.h         Public C API for libbifrost
├── test/                 Sample ARM64 programs (.s sources + assembled .elf)
├── ctest/                C test programs (musl-static) + jit_*.elf regression suites
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

**Instructions** — ~150 ARM64 instructions covering data processing
(MOVZ/K/N, ADD/SUB/CMP family, AND/ORR/EOR, bitfield, conditional
select, MUL/MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH,
UDIV/SDIV, RBIT/REV/CLZ/CLS, ADC/SBC with carry), branches
(B/BL/BR/BLR/RET, B.cond, CBZ/CBNZ, TBZ/TBNZ), load/store (immediate,
register, pair, sign-extended, unscaled, pre/post-index), LSE atomics
(LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS), acquire/release
(STLR/LDAR), exclusive monitor (LDXR/STXR/CLREX), FP arithmetic
(FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FCVT/SCVTF/FCVTZS/FMADD/FMSUB/
FCSEL, both S and D registers, including the **fixed-point FCVTZS/FCVTZU/
SCVTF/UCVTF variants** that broke MD5 in rc.1), FMOV Vd.D[1], SIMD/NEON
(DUP, MOVI all cmode values, LD1/ST1, CNT, CMEQ, UMAXP, SHL, USHR, SSHR,
EOR, ORR, AND, BIC, ORN, EON, NOT, NEG, ADD/SUB/MUL vector, REV16/32/64,
STP/LDP pairs including Q registers, EXT, INS, TBL/TBX), and system (SVC,
MRS/MSR, BRK, HLT, CLREX, HINT, barriers).

**JIT (frostJIT)** — Native x86-64 code generation for most instructions.
6.4x speedup over interpreter on compute workloads (571 MIPS). Features:
self-loop chaining, lazy block chaining, IR optimization (DCE, const
folding, copy propagation, store-load forwarding), W^X code buffer,
--jit-threshold for hybrid mode. Native SIMD codegen via SSE2/SSE4.1
(paddb/w/d/q, psubb/w/d/q, pmullw, pmulld, pcmpeqb/w/d/q, pand, por,
pxor, pandn). Function Multi-Versioning (FMV) via runtime CPUID
detection: native FMA3 codegen for FMADD/FMSUB/FNMADD/FNMSUB
(vfmadd231ss/sd, vfnmadd231ss/sd, vfnmsub231ss/sd) on hosts with
FMA3+AVX, with automatic fallback to decomposed mul+add/sub on older
CPUs. Override with `BIFROST_NO_FMA3=1`.

**Syscalls** — ~170 Linux AArch64 syscalls including file I/O (read/write/
openat/close/readv/writev/pwrite64/statx/fstatat/sendfile), process info
(getpid/gettid/uname/prlimit64), timing (clock_gettime/nanosleep/
clock_nanosleep), threading (clone, futex, set_tid_address), fork+execve
(clone without CLONE_VM, execve with ELF reload), event loops (eventfd2,
epoll, timerfd, ppoll), signal delivery (rt_sigaction, rt_sigprocmask,
sigaltstack, rt_sigreturn, rt_sigpending, rt_sigqueueinfo), file system
(mkdir, rmdir, rename, link, unlink, chmod, chown, fchmod, fchown, flock,
sync, fsync, fdatasync, truncate, utimensat, fallocate), and misc
(getrandom, ioctl, getcwd, waitid, unshare). Unsupported syscalls return
`-ENOSYS` silently unless `-v` is set.

**Signal Delivery** — Production-quality: proper AArch64 siginfo_t (128B)
and ucontext_t (448B) per kernel uapi headers. Supports SIG_BLOCK/UNBLOCK/
SETMASK (rt_sigprocmask), SS_ONSTACK/SS_DISABLE (sigaltstack), SA_RESETHAND,
SA_NODEFER, SA_SIGINFO, SA_ONSTACK. SIGSEGV delivery with fault_addr and
si_code (SEGV_MAPERR/SEGV_ACCERR). Host-to-guest signal forwarding for
SIGINT/SIGTERM/SIGCHLD with low-latency syscall-boundary draining.

**Dynamic Linker** — DT_NEEDED processing, shared library loading from
multiarch paths, global symbol table, GOT/PLT relocations (RELATIVE, ABS64,
GLOB_DAT, JUMP_SLOT, IRELATIVE). TLS relocations (TLS_DTPMOD, TLS_DTPREL,
TLS_TPREL, TLSDESC) with static TLS model. Activate via
`BIFROST_NATIVE_DYNLINK=1`.

**Fork + execve** — fork() via host fork() with copy-on-write memory.
Child disables JIT (interpreter-only), inherits CoW copy. execve() loads
new AArch64 ELF, resets CPU state, flushes JIT cache. Parent's wait4()/
waitid() forward to host. Enables external commands in toybox sh.

**VFS** — `/proc/self/{exe,cmdline,maps,status,auxv,environ}`,
`/proc/{meminfo,cpuinfo,version}`, `/dev/{null,zero,urandom,random,tty}`,
`/dev/{fb0,dsp,snd}`. Uses `memfd_create` for seekable virtual file
descriptors.

**TLS** — TPIDR_EL0 / TPIDRRO_EL0 via MRS/MSR; 64KB TLS scratch area
pre-allocated; per-thread TLS via `clone(CLONE_SETTLS, ...)`. Static TLS
block allocation for dynamically-linked binaries.

**ELF** — Static and dynamically-linked ELF64 AArch64 (ET_EXEC and ET_DYN);
PT_LOAD with BSS zero-fill; RELA relocations; PT_NOTE parsing for GNU
property features (LSE detection); PT_INTERP loading; PT_TLS parsing;
full initial stack with argc/argv/envp/auxv.

**Graphics** — Virtual `/dev/fb0` framebuffer (memfd-backed, mmap-able).
`FBIOGET_VSCREENINFO`/`FSCREENINFO` ioctls. Default 640x480@32bpp BGRA.
Headless: `--fb-dump PATH` writes a PPM image on exit. Optional SDL2
window backend via `make USE_SDL2=1`.

**Audio** — OSS `/dev/dsp` passthrough with in-memory PCM buffering.
`--audio-dump PATH` writes a WAV file on exit (16-bit, 44100Hz, stereo).

## Test Status

All 41 test programs pass under both the default frostJIT path and the
interpreter (`--no-jit`). The test suite has been verified clean under
ASan+UBSan. JIT is the default execution mode (6.4x speedup on compute
workloads, 571 MIPS on bench_mips).
`ctest/jit_*.elf` regression suite also passes under the interpreter
(`--no-jit`) to catch decoder drift. See [TESTS.md](TESTS.md) for the
full test matrix, including toybox compatibility (`echo`, `ls /`, `od`,
`head`, `sort`, `rev`, `wc`, `cat`, `printf "%g"`, `seq`, `factor` and
many more all work).

Run the test suite:

```bash
make test     # run all .elf under JIT, plus jit_*.elf under interpreter
make verify   # JIT divergence checker (slow, catches codegen bugs)
```

## Limitations

This is release-candidate quality software. Key limitations:

- **Limited dynamic linking.** The dynamic linker (PT_INTERP) is loaded
  and its entry point is used, allowing simple dynamically-linked musl
  binaries to run. However, full dynamic linking (DT_NEEDED processing,
  runtime relocations, glibc support) is not yet complete. Static
  binaries are recommended. Full dynamic linking and glibc support are
  planned for v2.0 (see [ROADMAP.md](ROADMAP.md)).
- **No ASLR.** Binaries load at their preferred vaddr.
- **`fork()` works via host fork().** `clone()` without `CLONE_VM` uses
  host fork() with copy-on-write memory. The child disables JIT and uses
  the interpreter. `execve()` loads a new ELF and resets CPU state.
  Parent `wait4()`/`waitid()` forward to host. External AArch64 commands
  work in toybox sh when symlinks exist in `/tmp/aarch64-bin/`.
- **Signal delivery is production-quality.** `rt_sigaction` installs
  handlers, `kill`/`tgkill` deliver signals, proper 128-byte `siginfo_t`
  and 448-byte `ucontext_t` are constructed on the guest stack per kernel
  uapi headers, `SA_RESTART`/`SA_RESETHAND`/`SA_NODEFER`/`SA_SIGINFO`/
  `SA_ONSTACK` are all implemented, and host-to-guest signal forwarding
  (SIGINT/SIGTERM/SIGCHLD/SIGWINCH) works with low-latency draining.
  Pending blocked signals are dropped (no pending queue).
- **frostJIT is the default execution mode.** All 41 tests pass, including
  the comprehensive int↔FP conversion test, the new FMA (FMADD/FMSUB/
  FNMADD/FNMSUB) test, and 19 real-world programs. The interpreter is
  available via `--no-jit` as a fallback for programs that hit a JIT bug
  or for debugging.
- **`strtod("inf")` and `strtod("-inf")` now work correctly** (return
  `inf` / `-inf` respectively). The root cause was a 32-bit SCVTF
  misdecode — see CHANGELOG.md for the full fix. `strtod("-nan")`
  returns `nan` (sign bit lost) — a separate, lower-priority issue in
  musl's `__floatscan` sign propagation that does not affect decimal
  or exponential inputs.
- **`BIFROST_ENABLE_FWD=1`** (arm_reg_cache load-forwarding) is an
  opt-in IR optimization that gives ~1.2x speedup on bench_mips. All
  JIT tests pass with it enabled, and `toybox ls /` now works under
  FWD (fixed in Turn 19 — the CCMP handler was clobbering scratch
  vregs without spilling).

For the full development roadmap, see [ROADMAP.md](ROADMAP.md).

## Release History

See [CHANGELOG.md](CHANGELOG.md) for the full per-commit history. The
current release is **v1.4.0** (2026-06-27):

- **MD5 now produces correct hashes.** The root cause was the
  FCVTZS/FCVTZU/SCVTF/UCVTF fixed-point variants being silently NOP'd
  (the integer-variant mask required bit 21 = 1; the fixed-point variant
  has bit 21 = 0 with a 6-bit scale field). Toybox MD5's K-table init
  uses `fcvtzu w1, d0, #32` to compute `floor(|sin(i+1)| * 2^32)`; with
  the NOP, K[i] was filled with stack garbage and the hash output was
  unrelated to the input. MD5 now joins SHA-1/224/256/384/512/CRC32 in
  the "verified correct under both JIT and interpreter" set.
- **JIT is now the default execution mode.** The 41-test suite,
  toybox integration, and musl libc all pass under the JIT, and
  `bench_mips` shows a 6.4x speedup. Use `--no-jit` to opt out.
- **JIT correctness overhaul** — 20+ bugs fixed across FP decode,
  32-bit shift semantics, int↔FP conversion (SCVTF/UCVTF/FCVTZS/
  FCVTZU), and system register reads. `toybox seq`, `printf "%g"`,
  `strtod("inf")`, `ls /`, and `od` all work now.
- **JIT SIGSEGV delivery** — memory faults in JIT'd code are now
  caught and delivered as SIGSEGV to the guest (rc=139), instead of
  crashing with `std::terminate` (rc=134). JIT'd code has no DWARF
  unwind info, so C++ exceptions can't propagate through it — the
  C-helper boundary (`jit_load_mem_slow`/`jit_store_mem_slow`/
  `jit_interp_step`) now catches `UnmappedMemory` and calls
  `deliver_signal()`.
- **JIT performance overhaul** — 571 MIPS on bench_mips (6.4x over
  interpreter, 10-run average) via self-loop chaining, liveness-based
  register freeing, and register-cache-aware ALU codegen.
- **Shared `fp_decode` helpers** in `decoder.hpp` keep the interpreter
  and JIT's IR translator in sync.
- **Audio backend** — OSS `/dev/dsp` passthrough + WAV dump.
- **VFS abstraction** — VNode + FdTable + procfs + devfs.
- **~16 new syscalls** (networking, inotify, statx).
- **frostJIT** — native UDIV/SDIV, SMADDL/UMADDL, MRS/MSR, 15 new FP
  instructions.

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
