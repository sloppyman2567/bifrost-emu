# bifrost-emu — Architecture Context

This document provides a high-level architectural overview of bifrost-emu for
contributors and code reviewers. It complements the README (usage/feature focus)
and CHANGELOG (release history) with structural and design-level context.

---

## Project Identity

- **Name:** bifrost-emu
- **Version:** 1.4.0-rc.1
- **Language:** C++17
- **Platform:** x86_64 Linux host, AArch64 Linux guest
- **License:** Unlicense (public domain)
- **Build:** `make` (g++ -O3, no external dependencies)

---

## Architecture Overview

```
Guest AArch64 ELF binary
        │
        ▼
┌──────────────────┐    ┌───────────────────┐
│  ELF Loader      │    │  Dynamic Linker   │
│  (elf_loader.cpp)│    │  (dynamic_linker) │
└────────┬─────────┘    └────────┬──────────┘
         │                       │
         ▼                       ▼
┌─────────────────────────────────────────────┐
│              Emulator (emulator.cpp)         │
│                                              │
│  ┌─────────┐  ┌─────────┐  ┌──────────────┐│
│  │  Memory  │  │  CPU    │  │ SignalTable  ││
│  │(memory.h)│  │(cpu.h)  │  │ (signal.h)   ││
│  └────┬─────┘  └────┬────┘  └──────────────┘│
│       │             │                        │
│  ┌────┴─────────────┴──────────────────────┐│
│  │           Execution Engine               ││
│  │                                          ││
│  │  Decoder ──► Interpreter ──► (slow path) ││
│  │  (decoder.cpp) (interpreter.cpp)         ││
│  │       │                                  ││
│  │       ▼                                  ││
│  │  IR Translator ──► IR Optimizer          ││
│  │  (ir_translate)   (ir_optimize)          ││
│  │       │                                  ││
│  │       ▼                                  ││
│  │  FrostJIT ──► x86 Backend ──► (fast)    ││
│  │  (frostjit.cpp) (x86_backend.cpp)        ││
│  └──────────────────────────────────────────┘│
│                                              │
│  ┌──────────┐  ┌────────┐  ┌──────────────┐│
│  │ VFS/FdTab│  │Graphics│  │   Audio      ││
│  │ (vfs/)   │  │(fb0)   │  │  (/dev/dsp)  ││
│  └──────────┘  └────────┘  └──────────────┘│
│                                              │
│  ┌──────────────────────────────────────────┐│
│  │         Syscall Layer (~170 syscalls)     ││
│  │  fs.cpp │ mem.cpp │ threads │ misc.cpp   ││
│  └──────────────────────────────────────────┘│
└──────────────────────────────────────────────┘
```

---

## Key Design Decisions

### 1. Decoder is the single source of truth

`decoder.cpp` extracts ARM64 instruction fields into a `DecodedInst` struct.
Both the interpreter and JIT share this decoder — neither performs bit
extraction. This eliminates decode drift bugs (historically a major source
of FCMP/FABS/FSQRT/FMOV-imm divergences).

### 2. Two execution paths: interpreter + JIT

- **Interpreter** (`src/interp/interpreter.cpp`): switch on `d.cls`,
  directly executes. Used as fallback and with `--no-jit`.
- **frostJIT** (`src/jit/frostjit.cpp`): translates ARM64 basic blocks
  to x86-64 via IR. Default execution mode (6.4x speedup on compute).

Both paths go through the same decoder. The JIT falls back to
`CALL_INTERP` for instructions it doesn't model in IR.

### 3. IR layer as the JIT's intermediate representation

```
ARM64 block → translate_to_ir() → IRBlock → optimize_ir() → compile_ir_inst() → x86-64 code
```

The IR has ~50 opcodes (ALU, shifts, loads/stores, branches, FP, SIMD).
The optimizer performs DCE, constant folding, copy propagation,
store-load forwarding, and peephole (redundant ZEXT removal).

### 4. Register allocator

The JIT uses a direct-mapping vreg→x86-reg allocator with 9 allocatable
host registers (RAX, RCX, RDX, R8, R9, R10, R11, R12, R13). ARM64
architectural registers (X0-X30, SP) are vregs 0-31; temporaries start
at vreg 33. The allocator tracks dirty state via a bitmask
(`dirty_host_regs_`) and supports targeted flush/invalidate for FP ops.

### 5. W^X code buffer protection

The 64 MB code buffer uses Write-XOR-Execute: `mprotect` toggles between
RW (during codegen) and RX (during execution). A reference-counted
`make_writable()`/`make_executable()` API supports nested calls
(translate_block → patch_chain). Falls back to RWX if mprotect fails.

### 6. Fork via host fork()

`clone()` without `CLONE_VM` uses the host's `fork()`. The child inherits
a CoW copy of the emulator's entire address space. The child sets
`jit_enabled_ = false` (but does NOT destroy the JIT object — the child
is executing inside the JIT code buffer when the SVC fires, so
`munmap()`-ing it would segfault). The parent's `wait4()` forwards to
the host kernel.

### 7. Function Multi-Versioning (FMV) via CPUID

The JIT queries `cpu_features_` (detected once at construction via
`CPUID` + `XGETBV`) to choose which x86 codegen variant to emit. The
first consumer is FMA3 codegen for `FMADD`/`FMSUB`/`FNMADD`/`FNMSUB`:
on FMA3+AVX hosts, the JIT emits `vfmadd231ss/sd` (single-rounded,
IEEE 754-correct); on older hosts, it falls back to the decomposed
`mulsd`+`addsd` path. `BIFROST_NO_FMA3=1` forces the decomposed path
for debugging. Future FMV work: AVX2 256-bit SIMD, BMI2 `pdep`/`pext`,
AVX-512 masked ops. See `include/jit/cpu_features.hpp`.

---

## File Organization (by subsystem)

| Subsystem | Files | Lines | Purpose |
|-----------|-------|-------|---------|
| **Decoder** | `decoder.cpp`, `decoder.hpp` | ~1,200 | ARM64 instruction decode |
| **Interpreter** | `interpreter.cpp` | ~2,480 | Switch-based execution |
| **IR** | `ir.h`, `ir.hpp`, `ir_builder`, `ir_translate`, `ir_optimize`, `ir_lower`, `ops` | ~3,600 | IR builder/translator/optimizer/executor |
| **JIT** | `frostjit.cpp`, `x86_backend`, `x86_regalloc`, `jit_cache`, `jit_profiler`, `jit_glue`, `frostjit.hpp`, `cpu_features.hpp`, `cpu_features.cpp` | ~6,200 | Block-translation JIT + FMV (CPUID detection) |
| **Core** | `emulator.h/cpp`, `cpu.h`, `memory.h/cpp`, `signal.h/cpp`, `thread_mgr.cpp` | ~2,600 | Engine core |
| **Frontend** | `elf_loader.cpp`, `dynamic_linker.cpp/h` | ~1,040 | ELF loading + dynamic linking |
| **Syscalls** | `syscalls.h/cpp`, `fs`, `mem`, `threads`, `time`, `ioctls`, `misc` | ~2,830 | Linux syscall emulation |
| **VFS** | `vfs.h/cpp`, `vfs_dev`, `vfs_table`, `vfs_host` | ~820 | Virtual filesystem |
| **Graphics** | `graphics.cpp`, `graphics.hpp` | ~640 | /dev/fb0 + SDL2 |
| **Audio** | `audio.h/cpp` | ~230 | /dev/dsp passthrough |
| **CLI** | `main.cpp` | ~320 | Argument parsing + TTY |

**Total:** ~26,200 lines of C++ across 59 source/header files.

---

## Known Issues and Limitations

### Correctness

1. **~~FMADD/FMSUB not truly fused.~~** ✅ FIXED in rc.1 for FMA3-capable
   hosts. The JIT now detects FMA3 support via CPUID and emits native
   `vfmadd231ss/sd`, `vfnmadd231ss/sd`, `vfnmsub231ss/sd` — single-rounded
   per IEEE 754. On non-FMA3 hosts, the decomposed mul+add path remains
   (double-rounded, but interpreter and JIT agree). `BIFROST_NO_FMA3=1`
   forces the decomposed path for debugging. See `include/jit/cpu_features.hpp`
   for the FMV framework. The interpreter still decomposes (would need
   `__builtin_fma` to match the JIT's FMA3 path).

2. **No pending signal queue.** Blocked signals are silently dropped instead
   of being queued for later delivery. Programs that rely on `sigpending()`
   will not work correctly. This is a known simplification documented in
   `signal.cpp`.

3. **SIMD sub-decode is a catch-all.** The `SIMD_DP` and `FP_SCALAR`
   InstClass values are generic catch-alls that require sub-dispatching by
   raw instruction bits in both the interpreter and JIT. This makes it hard
   to add new SIMD ops and was the source of the `toybox sh -c` regression
   (fixed in rc.0). The ROADMAP plans a hierarchical sub-decode refactor.

4. **~~NEON/SIMD strtok bug.~~** ✅ MOSTLY FIXED in rc.1 — 10 NEON bugs
   fixed: `immh` extraction (off by one bit), element-size rule, MOVI/shift
   encoding collision, REV64/REV32 mask + size-awareness, USRA/SSRA/SLI/SRI
   handlers added, INS/UMOV v_hi routing for Q=1, 32-bit ROR wrap-bit loss.
   SHA-1/224/256/384/512 and CRC32 now produce correct hashes. MD5 is
   improved (JIT and interpreter now agree) but still produces a wrong hash
   in toybox's code path — a remaining issue to isolate. `strtok`/`strtok_r`
   no longer break (the shift-handling bugs were the root cause).

### Performance

5. **JIT I/O overhead.** The JIT is ~9% slower than the interpreter for
   I/O-bound workloads (`toybox seq 1 10000`) because block-translation
   overhead is not amortized when most time is in syscalls. The
   `--jit-threshold` flag provides a partial workaround.

6. **Register allocator is FIFO, not LRU.** When all allocatable registers
   are taken, the first one in `ALLOC_REGS[]` is evicted regardless of
   recency. For certain access patterns this causes pathological eviction.
   True LRU would require tracking recency timestamps.

7. **~~FMADD/FMSUB decomposition loses a multiply-add fusion opportunity.~~**
   ✅ FIXED in rc.1 — on FMA3-capable hosts, the JIT now emits native
   `vfmadd231ss/sd` etc., gaining both correctness (single-rounded) and
   ~1 cycle per FMADD. The FMV framework (`cpu_features.hpp`) detects
   FMA3 at startup. Non-FMA3 hosts still use the decomposed path.

### Safety

8. **Vreg space limited to 4096 with debug-only bounds checking.** The
   `check_vreg_bounds()` function only runs in debug builds or when
   `BIFROST_REGALLOC_CHECK=1` is set. In release builds, a block that
   exceeds 4096 vregs would silently overflow the `vreg_home_[]` arrays.
   In practice, blocks max out at ~250 vregs, so this is unlikely.

9. **Single global `g_active_emu_` pointer.** Only one Emulator can be
   active for host signal forwarding at a time. This is fine for the
   single-guest-process use case but would need thread-local storage
   for concurrent multi-guest scenarios.

10. **FIFO eviction in register allocator.** See item 6 above. Not a
    correctness issue but a performance concern for specific patterns.

---

## Build and Test

```bash
make              # Release build (-O3)
make debug        # Debug build (ASan + UBSan)
make test         # Run 39 tests under JIT + interpreter
make verify       # JIT divergence checker (slow)
make lib          # Build libbifrost.a
make install      # Install to /usr/local/bin/
```

No external dependencies required. SDL2 is opt-in: `make USE_SDL2=1`.

Cross-compile test programs with the bundled musl toolchain:
```bash
make tools/fetch-musl-toolchain.sh   # one-time download
make cross SRC=ctest/hello.c OUT=ctest/hello.elf
```

---

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `BIFROST_NATIVE_DYNLINK=1` | Use in-emulator dynamic linker instead of guest ld.so |
| `BIFROST_JIT_VERIFY=1` | Run JIT divergence checker (compares against interpreter) |
| `BIFROST_ENABLE_FWD=1` | Enable experimental load-forwarding in IR optimizer (~5.6% speedup, known bugs) |
| `BIFROST_SYSCALL_TRACE=1` | Trace syscall invocations to stderr |
| `BIFROST_NO_CHAIN=1` | Disable lazy block chaining (debugging) |
| `BIFROST_NO_SELFLOOP=1` | Disable self-loop chaining (debugging) |
| `BIFROST_NO_WEX=1` | Disable W^X protection (always RWX code buffer) |
| `BIFROST_REGALLOC_CHECK=1` | Enable vreg bounds checking in release builds |

---

## Contributing

This project does not accept pull requests. Fork it freely — that's what
the public domain license is for. No attribution required.
