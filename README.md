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

  bifrost-emu  v1.3.0-beta.4
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.3.0-beta.4](https://img.shields.io/badge/version-1.3.0--beta.4-orange.svg)](CHANGELOG.md)

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
v1.3.0-beta.3; v1.3.0-beta.4 restructured the decoder itself from a
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
  and came first, making EXTR unreachable. v1.3.0-beta.4 fixes this.)

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
├── interpreter.cpp       Instruction execution (pure switch on d.cls — no if-chain)
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
| `extr.elf` | Verifies EXTR instruction decode and execute |

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
| `extr.elf` (assembled) | ✅ Works | Verifies EXTR (v1.3.0-beta.4) |
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
| `test_float.elf` (musl static) | ⚠️ Decode error | `printf("%f")` — no longer hangs (fixed in beta.3); now fails fast on an unhandled FP instruction in the softfloat path |
| `test_malloc.elf` (musl static) | ✅ Works | `malloc`/`free`/`qsort` all succeed; exit 0 (fixed in beta.3) |
| `test_sdl2.elf` (musl+SDL2 static) | ⚠️ Watchdog abort | Gets past atomics + mallocng init; hangs later in SDL2 setup |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | Unhandled instruction after mallocng |
| `toybox-aarch64` | ⚠️ Exit 1 | PC=0 (STP/LDP mode bug, planned for v2.0) |

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

## What's New in 1.3.0-beta.4

**The decoder is now a true hierarchical switch.** v1.3.0-beta.3 had a
stub `switch (bits[28:24])` at the top of `decode()` that did nothing
(`default: break;`) and then fell through to ~500 lines of flat
`if ((inst & MASK) == VAL)` chains. v1.3.0-beta.4 replaces that with
a real two-level hierarchical switch: outer switch on bits `[28:24]`
(the ARM ARM major encoding group), inner switch on the group-specific
discriminator. Every flat `if` chain is now a `case` with early
`return`.

**EXTR is now actually decoded.** v0 had two dead-code bugs around
EXTR:

1. The bitfield check (mask `0x1F000000`, ignoring bit 23) came BEFORE
   the EXTR check (mask `0x1F800000`, requiring bit 23 = 1). The
   bitfield mask matched every EXTR encoding, so the EXTR check was
   unreachable — every EXTR was silently misdecoded as
   SBFM/BFM/UBFM. v1.3.0-beta.4 routes on bit 23 first, so EXTR is
   correctly decoded.
2. The interpreter's EXTR handler had the operand order backwards:
   it concatenated `Rm:Rn` instead of `Rn:Rm` (per the ARM ARM,
   EXTR extracts from the concatenation `Xn:Xm`). v1.3.0-beta.4
   fixes the operand order.
3. The interpreter's EXTR handler used `(rn << width)` with
   `width == 64`, which is undefined behavior in C++ (shifting a
   `uint64_t` by its full width). v1.3.0-beta.4 uses `__uint128_t`
   for the 128-bit concatenation.

The interpreter already had an `EXTR` case (it was just never
reached). With the decoder fix, EXTR now works end-to-end. A new
test program (`test/extr.s`) verifies the behavior.

**Additional decoder correctness fixes:**

- **64-bit CBZ/CBNZ/TBZ/TBNZ** (`sf=1`, bits[31:29] = 101) now
  decode correctly. v0's flat masks caught only the 32-bit form
  (bits[31:29] = 001).
- **BRK and HLT** now enforce `bits[4:0] == 0` per the ARM ARM. v0's
  flat masks required this implicitly via the full 32-bit mask, but
  the hierarchical version makes it explicit.
- **Add/subtract extended register** now enforces `bits[23:22] == 00`
  (v0's mask `0x1FE00000` required this implicitly; the hierarchical
  version makes it an explicit check).
- **SIMD data processing** now explicitly requires `bit 31 == 0`
  (v0's mask `0x9E000000` included bit 31; the hierarchical version
  checks it explicitly for clarity).
- **Load/store various** (LDUR/STUR, LSE atomics, LDR/STR register
  offset) now explicitly requires `bit 29 == 1` (v0's masks required
  this implicitly).

**STP/LDP pre-index collision fix is now structural.** v1.3.0-beta.3
fixed the STP/LDP pre-index vs ORR collision by careful if-chain
ordering (checking STP/LDP before logical shifted register). That fix
was fragile — any reordering of the if-chain could re-introduce the
bug. v1.3.0-beta.4 makes the fix structural: STP/LDP pre-index V=0
lives in outer case `0x09`; logical shifted register lives in outer
case `0x0A`. They cannot collide regardless of code ordering.

**Added `extr` mnemonic to `mini_arm64_asm.py`** so test programs
can use EXTR directly.

**`test/extr.s`** — new test program that verifies EXTR works
end-to-end. Loads `x0 = 0xBABECAFE` and `x1 = 0xBEEFDEAD`, executes
`extr x2, x1, x0, #0` (which should give `x2 = x0 = 0xBABECAFE`),
compares against the expected value, and prints `OK` or `NO`.

**No regressions.** All five original `.elf` test programs (hello,
count, fib, cat, echo) produce byte-identical output and exit codes
vs v1.3.0-beta.3. The three working musl-static C tests
(`hello.c`, `loop.c`, `test_malloc.c`) also continue to work. The
pre-existing `test_float.elf` hang (musl's `printf("%f")` softfloat
path) is unchanged — it's not a regression.

See [CHANGELOG.md](CHANGELOG.md) for the complete release history.

## What's New in 1.3.0-beta.3

**The big one: the mallocng hang is fixed.** The root cause was a
32-bit rotation bug in the UBFM/SBFM/BFM instruction handler. When
executing `lsl w24, w26, #4` (which the compiler encodes as
`ubfm w24, w26, #28, #27`), the emulator used `ror64` followed by a
`uint32_t` cast, which lost the wrapped bits — `ror64(0x2, 28)` puts
the wrapped bits at position 36, and `(uint32_t)0x2000000000 = 0x0`
instead of the correct `0x20` (= 32). This made musl's mallocng stride
0, which caused `alloc_slot` to infinitely recurse because no group size
class could satisfy the `stride * nslots + 16 <= pagesize/2` check.
`malloc`/`free`/`qsort` all work now. This was THE #1 blocker since
v1.1.5-alpha.1.

**BIC/ORN/EON/BICS fix (the printf/fclose root cause).** The logical
shifted register group has an N bit (bit 21) that inverts the second
operand: AND→BIC, ORR→ORN, EOR→EON, ANDS→BICS. The interpreter was
ignoring N entirely — BIC was treated as AND. This broke musl's `strlen`
zero-byte detection (`bic x2, x3, x2` computed `x3 & x2` instead of
`x3 & ~x2`), causing strlen to scan past NUL terminators, which
corrupted stdio buffer management, producing garbled printf output and
the exit-133 fclose crash. With the fix, all printf formats (`%s %d %u
%x %c %ld %llx`), puts, fwrite, and qsort produce correct output, and
programs exit cleanly (exit 0, no fclose crash).

**Bitmask immediate decode fix.** The decoder's `decode_bitmask_imm` used
`~0` for `esize==64` regardless of `S`, producing all-ones instead of
the correct mask for masks like `0xFFFFFFFFFFFFFFF0`. The interpreter's
inline bitmask decode had a separate bug (shifted ones to `esize-1-S`
instead of bit 0, producing `0xf8f8f8f8` for `0x1f`). Both fixed: the
decoder uses `(1ULL << width) - 1`, and the interpreter uses `d.imm_u`
from the decoder.

**Hierarchical STP/LDP decoder fix.** The decoder now checks Load/Store
pair (STP/LDP) before logical shifted register, preventing the pre-index
STP/LDP vs ORR encoding collision. The STP/LDP mask was expanded to
catch all three addressing modes (post-index, signed offset, pre-index)
for both GP and SIMD registers, with a mode validation check to reject
LDUR/STUR that shares the same top bits.

**The decoder switch is complete, the legacy if-chain is deleted.**
Every instruction handler — branches, system, data processing (immediate
and register), load/store, atomics, SIMD data-processing, and FP scalar —
now lives in the `switch(d.cls)` block in `interpreter.cpp`. The
~500-line legacy if-chain that lived below the switch since alpha.1 is
gone. The interpreter now does a single `decode()` call per instruction
and dispatches purely on `d.cls`. The decoder is the true single source
of truth, no exceptions.

**FP scalar decoder fix.** The FP scalar decoder only matched
`0x1E200000` (32-bit single-precision). It missed all 64-bit
(`sf=1`, top byte `0x9E`) and double-precision (`ftype=01`) FP
instructions, causing decode errors on any binary using D registers.
Fixed by adding the broad `0x1E000000` and `0x9E000000` top-byte masks,
matching the original if-chain's three-way check. `test_float` no
longer hangs — it now fails fast with a decode error on an unhandled
FP instruction in the softfloat path (an improvement over the infinite
hang).

**mallocng MAP_FIXED overlap handling.** When musl's mallocng calls
`mmap(MAP_FIXED, addr, ...)` inside the brk region (which it does to
carve out guard pages and meta_area slots), the brk is now pushed
forward past the mmap'd region. This prevents a subsequent `brk(new)`
extension from re-mapping the same pages via `map_range` and corrupting
musl's metadata. The 1.1.5-alpha.1 changelog described this fix but the
actual code was missing; this release finally implements it. (Note: this
was NOT the root cause of the mallocng hang — the UBFM rotation bug was.
But this fix is still correct and necessary for long-running malloc
workloads.)

**Hang watchdog.** The run loop tracks the last PC and counts how many
times it's been executed consecutively. If the same PC is hit more than
50 million times in a row (which only happens for `b .` self-branches
or genuinely stuck atomic-CAS loops), the emulator aborts with a
diagnostic message instead of spinning forever. Legitimate tight loops
(`fib`, `count`, etc.) cycle through multiple PCs and never trip the
watchdog.

**MADD family decoder fix.** The old decoder classified `SMULH` as
`sub_op=7`, but per the ARM ARM pseudocode (verified at
https://www.scs.stanford.edu/~zyedidia/arm64/smulh.html), `SMULH` is
`sub_op=2`. The old code's `case 7: d.cls = InstClass::SMULH` was
unreachable; `SMULH` instructions would have fallen through to the
UNKNOWN case and thrown a `DecodeError`. Fixed to use the correct
`sub_op=2`.

**LSE atomics decoder fix.** The old decoder treated `SWP` as a
distinct encoding (bit 21=1) and `LDADD` family as bit 21=0. Per the
ARM ARM, **all** LSE atomics have bit 21=1 — they're distinguished by
the `opc` field at bits 15:12, not by bit 21. The old code's LDADD
handler (checking bit 21=0) would never match real LDADD instructions;
only the SWP handler caught them, and it did swap semantics — silently
wrong for LDADD/LDCLR/LDEOR/etc. The new decoder classifies any
bit-21=1 encoding in the 111000 group as `LSE_ATOMIC` and sub-dispatches
on `atom_op` in the interpreter. The `has_lse_` gate is now checked in
the interpreter's `LSE_ATOMIC` case: if the binary doesn't declare LSE,
the encoding is executed as LDUR/STUR (matching real hardware).

**Decoder warning cleanup.** Fixed three compiler warnings in
`decoder.cpp`: the tautological hint-mask comparison (`(inst & 0xFFFFF010)
== 0xD5033090` was always false), the unused `nbytes` variable in the
unsigned-offset load/store decoder, and the unused `sf` parameter in
`extend_reg`.

**Migration status section deleted.** The README no longer has a
"Current Migration Status" / "Still in if-chain" section — there is no
if-chain anymore. The full instruction list now lives under
"Decoder Switch — Complete Coverage" above.

**Fixes from 1.1.x / 1.3.0-alpha/beta.1/beta.2 carried forward:**
- SIMD LDR/STR Q-form (128-bit) — was transferring only 1 byte
- FMOV Vd.D[1], Rn — was unimplemented (broke 128-bit softfloat)
- BFM destination field position — was inserting at bit 0
- ADC/ADCS/SBC/SBCS — were unimplemented
- MOVI Vd.2D, #0 — was only handling byte broadcast form
- PT_NOTE-based LDUR/LSE disambiguation — matches real hardware
- MAP_FIXED overlap handling for mallocng (now actually implemented)
- Exclusive monitor: branches no longer clear the monitor
- LDXR decode: `low6=0x3F` with `o0=0` is LDXR, not LDAR
- fclose/`__stdio_exit` crash: catch UnmappedMemory during exit

Full release notes in [CHANGELOG.md](CHANGELOG.md).

## Limitations

This is beta-quality software. Known issues:

- **`printf("%f", ...)` fails with a decode error.** musl's float
  formatter uses 128-bit softfloat routines that hit FP instructions we
  don't yet model. This is an improvement over beta.2 (which hung
  forever); the failure is now fast. Integer printf formats (`%d`,
  `%x`, `%c`, `%s`, `%ld`, `%llx`) all work.

- **Function pointer tables in static-PIE binaries** may not relocate
  correctly (`test_fnptr` hits a decode error).
- **No signal delivery** — `rt_sigaction` is a no-op.
- **No dynamic linking** — static binaries only.
- **No ASLR** — binaries load at their preferred vaddr.
- **`toybox-aarch64` crashes at PC=0** — STP/LDP mode calculation bug.
  Fix requires further decoder work, planned for v2.0.
- **glibc 2.36+ static binaries** hit a decode error on an unhandled
  instruction.
- **`test_sdl2.elf`** gets past atomics and mallocng init (thanks to
  the beta.3 fixes) but hangs later in SDL2 setup. The hang watchdog
  catches it as a fast-fail.

## Roadmap

**Short-term (1.3.0 final):**
1. Fix `printf("%f")` — audit the softfloat FP instruction path and
   implement the missing FP ops
2. Fix `test_fnptr` — investigate static-PIE self-relocation
3. More test coverage: threads, signals

**Medium-term (1.4.0):**
1. Signal delivery (`rt_sigaction` + `rt_sigreturn` + trampoline page)
2. SDL2 rendering for the graphics backend (video/audio/input) — the
   mallocng fix in beta.3 unblocks this; SDL2 init now gets past the
   allocator
3. Sub-decode the SIMD DP and FP scalar catch-all groups (currently
   routed as generic `SIMD_DP` / `FP_SCALAR` and re-dispatched in the
   interpreter; the hierarchical decoder structure makes adding
   dedicated `InstClass` values for each a clean refactor)

**Long-term (2.0+):**
1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS. The hierarchical decoder
   structure in v1.3.0-beta.4 makes this easier — the outer switch
   on bits[28:24] maps directly to a JIT dispatch table.
2. **Game support** — framebuffer/DRM, audio, input. Long-term goal:
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
