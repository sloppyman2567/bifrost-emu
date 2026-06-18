# Changelog

All notable changes to **bifrost-emu** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release tags (`-beta.N`, `-rc.N`) for unstable versions.

## [1.3.0-alpha.3] — 2026-06-18

Incremental migration release. Migrated the entire immediate group
from the legacy if-chain to the decoder switch, bringing the total
migrated instruction classes to 17. Also includes the decode cache
and MOVI fix from alpha.2.

### Migrated to Switch (from if-chain)
- `ADR` / `ADRP` — PC-relative address computation
- `MOVN` / `MOVZ` / `MOVK` — move immediate
- `ADD_IMM` / `ADDS_IMM` / `SUB_IMM` / `SUBS_IMM` — add/subtract immediate
- `SBFM` / `BFM` / `UBFM` — bitfield extract/insert/move
- `EXTR` — extract register (fixed to use 128-bit concatenation)
- `AND_IMM` / `ORR_IMM` / `EOR_IMM` / `ANDS_IMM` — logical immediate

### Fixed
- **EXTR switch case** — was using a broken two-shift approach; fixed
  to use 128-bit concatenation (`(hi << width) | lo`) matching the
  if-chain's algorithm.
- **Logical immediate switch case** — replaced the simplified
  `decode_bitmask_imm` with the if-chain's exact bitmask decode
  algorithm, which handles all edge cases correctly.

### Architecture
- The decoder (`decoder.cpp`) is the single source of truth for
  instruction decode. The interpreter dispatches on `d.cls` via
  `switch`. A decode cache (`PC → DecodedInst`) avoids re-decoding
  on repeated execution.
- **17 of ~50 instruction classes** are now handled in the switch.
  The remaining ~33 still fall through to the legacy if-chain.
  See README.md "Current Migration Status" for the full list.

### Verification
All 16 tests pass (6 assembly + 10 musl-static C). No regressions.
Performance: ~23 MIPS with decode cache.

---

## [1.3.0-alpha.2] — 2026-06-18

Full decoder rewrite and decode cache.

### Added
- **Extended `DecodedInst`** with all fields needed by every handler
  (reads_sp, writes_sp, hw, immr, imms, N, opc_ls, dp_opcode,
  nzcv_field, Q, ftype, cmode, fp_opcode, rmode, is_sub, sysreg
  fields, etc.).
- **Rewrote `decoder.cpp`** with complete decode logic for all
  instruction groups: branches, system, immediate, register, load/store,
  atomics, SIMD/FP. The decoder now extracts ALL fields the interpreter
  needs.
- **Instruction decode cache** (`PC → DecodedInst`). Since guest code
  is not self-modifying, each PC always decodes to the same instruction.
  Cache turns millions of decode() calls into hash-map lookups for
  tight loops.
- **~10 new `InstClass` values** for future migration (SVC_IMM,
  BRK_IMM, MSR_SYS, MRS_SYS, HINT, CLREX_INST, SIMD_DP, FP_SCALAR, etc.)

### Fixed
- **MOVI Vd.2D, #0** (cmode=0xE, Q=1) — was only handling byte broadcast
  form (cmode=0xF, Q=0). musl uses `movi v1.2d, #0` to zero 128-bit
  vector registers for softfloat comparisons.

### Note
Attempted full interpreter rewrite (pure switch, no if-chain) but hit
multiple subtle decode bugs in the migration (STP/LDP mode bits,
ADD/SUB shifted vs extended register, LDRSW is_load). Reverted to the
working hybrid approach. The decoder is now much more complete and the
cache provides real performance.

---

## [1.3.0-alpha.1] — 2026-06-17

Major architectural release: the decoder is now wired up as the single
source of truth for instruction decode. The interpreter calls `decode()`
once per instruction, then dispatches via `switch(d.cls)`. This eliminates
the entire class of ordering bugs (like the LDUR/LSE collision) because
`decode()` is the authoritative mapping from bit patterns to instruction
classes.

### Architecture Change
- **Decoder is now the entry point.** `Emulator::execute()` calls
  `decode(d, inst)` at the top, then switches on `d.cls`. Instructions
  that the decoder handles cleanly (branches, ADC/SBC, FMOV Vd.D[1])
  are executed in the switch and return immediately. Everything else
  falls through to the legacy if-chain (transitional, will be deleted
  in v2.0).

- **Hybrid dispatch (Phase 1).** This release uses a hybrid approach:
  the switch handles migrated instruction classes, the if-chain handles
  the rest. This lets us incrementally move handlers without breaking
  anything. Phase 2 (future) will move all remaining handlers to the
  switch and delete the if-chain.

- **JIT-ready.** The `decode()` function is now pure and reusable.
  The future v2.0 JIT will call `decode()` then emit x86_64 code based
  on `d.cls` — sharing the exact same decode logic as the interpreter.

### Added
- **`ADC_REG`, `ADCS_REG`, `SBC_REG`, `SBCS_REG`** instruction classes
  in decoder.hpp. These are now decoded by `decode()` and executed in
  the switch — previously they were inline in the if-chain.
- **`FMOV_VD1`, `FMOV_RVD1`** instruction classes for
  `FMOV Vd.D[1], Rn` and `FMOV Rn, Vm.D[1]`. Now decoded and executed
  via the switch.
- **Extended `InstClass` enum** with all FP/SIMD instruction types
  (FADD, FSUB, FMUL, FDIV, FMADD, FMSUB, FABS, FNEG, FSQRT, FCMP,
  FCVT, FCVTZS, FCVTZU, SCVTF, UCVTF, FRINT, FCSEL, etc.) for future
  migration to the switch.

### Migrated to Switch (from if-chain)
- `B`, `BL` — unconditional branches
- `Bcond` — conditional branch
- `CBZ`, `CBNZ` — compare and branch
- `TBZ`, `TBNZ` — test bit and branch
- `BR`, `BLR`, `RET` — branch to register
- `ADC_REG`, `ADCS_REG`, `SBC_REG`, `SBCS_REG` — add/sub with carry
- `FMOV_VD1`, `FMOV_RVD1` — FP move with index

### Verification
All existing tests pass with no regressions:
- 6 assembly tests (hello, count, fib, cat, echo, repl) ✅
- 10 musl-static C tests (loop, recursion, structs, bitops, switch,
  advanced, argv, args_math, strings, math) ✅
- Known failures unchanged (printf %f, malloc/free, test_fnptr — same
  as 1.1.5-alpha.1)

### Next Up (1.3.0-beta.1 / 1.3.0)
1. Migrate more handlers from if-chain to switch (ADD/SUB, logical,
   load/store, etc.)
2. Fix `printf("%f")` — audit FP value propagation
3. Fix `malloc`/`free` — rewrite brk/mmap interaction
4. Performance: decoded instruction cache (now possible since decode
   is centralized — cache DecodedInst by PC)
5. Signal delivery (1.3.0 target)
6. SDL2 graphics backend (1.3.0 target)

---

## [1.1.5-alpha.1] — 2026-06-17

Major alpha release with multiple correctness fixes, syscall expansions,
and broader test coverage. The headline fix is the SIMD load/store bug
that broke 128-bit (`str q0`/`ldr q0`) operations — this was silently
corrupting softfloat values on the stack and broke musl's `printf("%f")`
path. Several other instructions used by musl's 128-bit softfloat
routines (`__multf3`, `__addtf3`, `__eqtf2`, etc.) are also now
implemented.

### Fixed
- **SIMD LDR/STR Q-form (128-bit) decode** — the previous handler
  interpreted `opc` incorrectly for SIMD loads/stores. The correct
  encoding per the ARM ARM:
    - `opc=00, size=xx` → STR B/H/S/D form (1/2/4/8 bytes)
    - `opc=01, size=xx` → LDR B/H/S/D form (1/2/4/8 bytes)
    - `opc=10, size=00` → STR Q form (128-bit / 16 bytes)
    - `opc=11, size=00` → LDR Q form (128-bit / 16 bytes)
  The old code treated `opc=10` as a load (because `(opc & 2) || (opc & 1)`
  was the load test), so `str q0` was silently dropped and `ldr q0`
  only transferred 1 byte. This corrupted 128-bit long doubles on the
  stack, breaking musl's `__multf3` and the entire `printf("%f")` code
  path. Fixed in all three load/store handlers (unsigned-offset,
  pre/post-indexed, register-offset).

- **`FMOV Vd.D[1], Rn` and `FMOV Rn, Vm.D[1]`** — these instructions
  move a 64-bit GPR to/from the HIGH 64 bits of a vector register
  (encoding `0x9EA00000` family). Previously unimplemented; musl's
  softfloat routines use them heavily to construct 128-bit long doubles
  from two 64-bit GPRs.

- **`BFM` (bitfield move) destination position** — the previous BFM
  implementation inserted source bits at position 0 of the destination,
  instead of at the field position `[immr..imms]`. This broke `bfi`
  (bitfield insert), which musl uses to assemble FP exponent and
  mantissa fields. Fixed both the non-wraparound case (imms >= immr)
  and the wraparound case (imms < immr).

- **`ADC`/`ADCS`/`SBC`/`SBCS`** — add/subtract with carry. Encoding
  `0x1A000000` family. Previously unimplemented; caused decode errors
  in softfloat routines that use multi-precision arithmetic (e.g.
  `__multf3` uses `adc` to propagate carry between 64-bit limbs).

### Added
- **New syscalls** (~10 more, total ~88):
  - `dup` (23), `dup2` (33) — file descriptor duplication
  - `pipe2` (59) — pipe creation with flags
  - `mkdirat` (34), `unlinkat` (35), `renameat` (38) — filesystem ops
  - `utimensat` (88) — file timestamps
  - `fstatat` (79) — file stat by path (was already there but improved)
- **`read_path` helper** in syscalls.cpp for reading NUL-terminated
  path strings from guest memory (used by the new filesystem syscalls).
- **`brk_start_` member** on `Emulator` to track the initial brk
  address, needed for the MAP_FIXED overlap fix below.

### Changed
- **`MAP_FIXED` overlap handling** — when musl's mallocng calls
  `mmap` with `MAP_FIXED` on an address inside the brk region (which
  it does to carve out memory for its metadata arena), the brk is now
  pushed forward past the mmap'd area. This prevents the MAP_FIXED
  mmap from zeroing out brk-managed pages and corrupting mallocng's
  metadata. (Partial fix — see Known Limitations.)

### Verification
Compiled and ran 13 musl-static C test programs (all compiled with
`aarch64-linux-musl-gcc -static -O2`). Results:

| Test | Description | Result |
|------|-------------|--------|
| `loop.c` | `for` loop + `printf("%d\n", ...)` | ✅ |
| `test_recursion.c` | Recursive `fib(20)` | ✅ |
| `test_structs.c` | Structs, pointers, `strcpy`/`strcat`/`strlen` | ✅ |
| `test_bitops.c` | 64-bit arithmetic, bit ops, `%016llx` | ✅ |
| `test_switch.c` | Switch/jump-table, 2D arrays, `goto` loops | ✅ |
| `test_advanced.c` | Ackermann recursion, pointer arithmetic | ✅ |
| `test_argv.c` | `argc`/`argv` parsing | ✅ |
| `test_args_math.c` | `strtol`, sum/product of argv | ✅ (new) |
| `test_strings.c` | `strcmp`/`strchr`/`strrchr`/`memset`/`memcpy` | ✅ (new) |
| `test_math.c` | 64-bit mul/div, shifts, ternary | ✅ (new) |
| `test_fnptr.c` | Function pointer table dispatch | ⚠️ Decode error (relocation) |
| `test_fileio.c` | `open`/`read`/`write`/`close` | ⚠️ Partial (`fclose` crash) |
| `test_float.c` | `printf("%f", ...)` with doubles | ❌ Still hangs (partial fix) |
| `test_malloc.c` | `malloc`/`free`/`qsort` | ❌ Still hangs (partial fix) |

All 6 pre-existing assembly test programs (`hello`, `count`, `fib`,
`cat`, `echo`, `repl`) still pass — no regressions.

### Known Limitations
This is an alpha release. The SIMD/BFM/ADC fixes unblock many more code
paths, but several issues remain:

- **`printf("%f", ...)` still hangs in some cases.** The SIMD LDR/STR
  fix resolved the stack corruption that caused the original infinite
  recursion in `__multf3`. However, musl's `__fmt_fp` (the float
  formatter) now enters a different loop involving `__fixunstfsi`
  (long double → unsigned int conversion). The root cause appears to
  be incorrect FP value propagation through the softfloat chain.
  Investigating. Integer printf formats (`%d`, `%x`, `%c`, `%s`, `%ld`,
  `%llx`) all work correctly.

- **`malloc`/`free` still hangs in mallocng init.** The `MAP_FIXED`
  overlap fix helps, but musl's `__malloc_alloc_meta` still enters an
  infinite recursion when its `brk()`+`mmap()` growth path is
  exercised. The brk syscall works correctly, but musl's metadata
  tracking gets confused by the interaction between brk extension and
  MAP_FIXED mmap carving. This is the same class of bug that blocks
  `toybox-aarch64`. Planned fix: rewrite the brk/mmap interaction to
  more closely match Linux kernel semantics.

- **`test_fnptr` decode error.** Function pointer tables in static-PIE
  binaries aren't being relocated correctly. The function pointer ends
  up pointing at a `.rodata` string instead of the function entry
  point. Likely a `R_AARCH64_RELATIVE` relocation issue where musl's
  self-relocator conflicts with our pre-applied relocations.

- **`test_fileio` `fclose` crash.** File contents print correctly,
  but on `fclose`/`__stdio_exit`, musl calls `memchr` on a `FILE*`
  struct field that contains a garbage pointer. Likely a stdio
  cleanup path issue where a `FILE*` struct field is read after the
  underlying buffer has been reused.

- **`toybox-aarch64` still exits with code 1 at PC=0.** The STP/LDP
  mode calculation bug described in 1.1.0-rc.2's notes is still
  pending the v2.0 hierarchical decoder restructure.

- **glibc 2.36+ static binaries** still hit a decode error — unchanged.

### Compatibility Matrix
| Binary | 1.1.1-alpha.1 | 1.1.5-alpha.1 |
|--------|---------------|---------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works |
| `hello_arm64_musl` (static) | ✅ Works | ✅ Works |
| `loop.elf` (musl static-PIE) | ✅ Works | ✅ Works |
| `test_recursion.elf` | ✅ Works | ✅ Works |
| `test_structs.elf` | ✅ Works | ✅ Works |
| `test_bitops.elf` | ✅ Works | ✅ Works |
| `test_switch.elf` | ✅ Works | ✅ Works |
| `test_advanced.elf` | ✅ Works | ✅ Works |
| `test_argv.elf` | ✅ Works | ✅ Works |
| `test_args_math.elf` | ❌ n/a | ✅ **Works (new!)** |
| `test_strings.elf` | ❌ n/a | ✅ **Works (new!)** |
| `test_math.elf` | ❌ n/a | ✅ **Works (new!)** |
| `test_fnptr.elf` | ❌ n/a | ⚠️ Decode error (new) |
| `test_fileio.elf` | ⚠️ Partial | ⚠️ Partial (unchanged) |
| `test_float.elf` | ❌ Hangs | ❌ Hangs (partial fix) |
| `test_malloc.elf` | ❌ Hangs | ❌ Hangs (partial fix) |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | ⚠️ Decode error (unchanged) |
| `toybox-aarch64` | ⚠️ Exit 1 (PC=0) | ⚠️ Exit 1 (PC=0, unchanged) |

### Next Up (1.1.5-alpha.2 / 1.1.5-beta.1)
1. Fix `printf("%f")` — audit `__fixunstfsi` and FP value propagation.
2. Fix `malloc`/`free` — rewrite brk/mmap interaction.
3. Fix `test_fnptr` — investigate static-PIE self-relocation conflict.
4. Fix `test_fileio` `fclose` crash — stdio cleanup bug.
5. Performance: decoded instruction cache (avoid re-decoding each step).
6. More test programs: threads (`pthread_create`), signals.
7. toybox stable — requires the v2.0 hierarchical decoder restructure.

---

## [1.1.1-alpha.1] — 2026-06-17

Alpha release. Fixes a decoder collision between `LDUR` (unscaled load)
and LSE atomic instructions that broke static-PIE binaries compiled with
musl-gcc. Marked as alpha because the broader mallocng and FP-format
code paths still have unresolved issues (see Known Limitations below).

### Fixed
- **LDUR/LSE atomics decode collision** — the LSE atomics handler at
  `interpreter.cpp:1088` was incorrectly catching `LDUR`/`STUR`
  instructions because their encoding patterns genuinely overlap in
  three of the four discriminating bit fields (bits 29:24, bit 21, and
  bits 11:10 are all identical between LDUR/STUR and LSE atomics). The
  ARM Architecture Reference Manual disambiguates them by the binary's
  declared feature set: if the ELF declares AArch64 LSE atomics via
  the `GNU_PROPERTY_AARCH64_FEATURE_1_LSE` bit in `.note.gnu.property`,
  the encoding is interpreted as LSE; otherwise it is LDUR/STUR.

  Previous attempt (in 1.1.1-rc.1, never released) used a bit-15
  heuristic that only caught LDUR with `imm9 bit 3 = 1` (i.e. offsets
  like -8, -16, -24, ...). This unblocked musl's `memcpy`/`printf` for
  the common case but still misrouted LDUR with smaller offsets
  (-4, -3, ...), which appears in musl's mallocng allocator.

  This release implements the proper fix: the ELF loader now parses
  `PT_NOTE` segments looking for `NT_GNU_PROPERTY_TYPE_0` notes with
  the `GNU` vendor name, and within them scans for property records
  of type `GNU_PROPERTY_AARCH64_FEATURE_1_AND` (0xC0000000). If the
  `GNU_PROPERTY_AARCH64_FEATURE_1_LSE` bit (0x8) is set in the
  property data, the loaded ELF's `has_lse` flag is set to true and
  the LSE atomics handler is enabled. Otherwise — the default for
  musl-static binaries compiled without `-march=...+lse` — the LSE
  atomics handler is skipped entirely and all `LDUR`/`STUR`
  encodings are routed to the unscaled load/store handler.

  This is the contract the ARM ARM specifies and matches what real
  hardware does at runtime: the CPU decodes based on the binary's
  declared feature flags (set by the compiler via `.note.gnu.property`).

### Added
- **PT_NOTE parsing for GNU property features** (`arm64_emu.hpp`,
  `ElfLoader::load`). Currently only `GNU_PROPERTY_AARCH64_FEATURE_1_LSE`
  is consumed; the infrastructure is in place to extend to other
  feature bits (BTI, PAC, etc.) as needed.
- **`has_lse` field on `Loaded` struct and `has_lse_` member on
  `Emulator`**, propagated from the loader to the interpreter.

### Verification
Compiled and ran 9 test programs through the emulator (all compiled
with `aarch64-linux-musl-gcc -static -O2`). Results:

| Test | Description | Result |
|------|-------------|--------|
| `loop.c` | `for` loop + `printf("%d\n", ...)` | ✅ `Loop value is: 10` |
| `test_recursion.c` | Recursive `fib(20)` | ✅ `fib(20) = 6765` |
| `test_structs.c` | Structs, pointers, `strcpy`/`strcat`/`strlen` | ✅ All correct |
| `test_bitops.c` | 64-bit arithmetic, bit ops, `%016llx` format | ✅ All correct |
| `test_switch.c` | Switch/jump-table, 2D arrays, `goto` loops | ✅ All correct |
| `test_advanced.c` | Ackermann recursion, pointer arithmetic | ✅ Ackermann correct |
| `test_argv.c` | `argc`/`argv` parsing with extra args | ✅ All args correct |
| `test_fileio.c` | `open`/`read`/`write`/`close` | ⚠️ Prints file, then unmapped-read error |
| `test_float.c` | `printf("%f", ...)` with doubles | ❌ Hangs in musl's float formatter |
| `test_malloc.c` | `malloc`/`free`/`qsort` with function pointers | ❌ Hangs in musl's mallocng init |

All 6 pre-existing assembly test programs (`hello`, `count`, `fib`,
`cat`, `echo`, `repl`) still pass — no regressions.

### Known Limitations
This is an alpha release. The LDUR/LSE fix is correct and robust, but
several higher-level code paths still hit unresolved emulator bugs:

- **`printf("%f", ...)` hangs.** musl's `__printf_core` float-formatting
  path (`fprintf` → `fmt_fp` → `__fmt_fp`) uses FP/SIMD instructions
  whose emulation has bugs. Even `printf("%f\n", 3.14)` hangs. Integer
  formats (`%d`, `%x`, `%c`, `%s`, `%ld`, `%llx`, etc.) all work.
  The FP arithmetic implementation was added in 1.1.0-rc.2 and has
  not been hardened against musl's float formatter. Planned fix:
  audit `FADD`/`FMUL`/`FDIV`/`FCVT`/`FRINT*` for IEEE 754 edge cases,
  especially rounding-mode handling and subnormal numbers.

- **`malloc`/`free` hangs in mallocng init.** musl's `__malloc_alloc_meta`
  enters an infinite recursion when its `brk()`+`mmap()` growth path
  is exercised. The `brk` syscall returns the requested address
  (correct), but musl's mmap-with-`MAP_FIXED` over the brk region
  confuses the allocator's metadata tracking. This is the same
  class of bug that breaks `toybox-aarch64` (PC=0 crash). Planned
  fix: implement proper `MAP_FIXED` overlap handling in `mmap`, and
  audit `mremap` for the in-place growth contract that musl expects.

- **`test_fileio` partially works** — file contents are printed
  correctly, but on `close(fd)` musl's stdio cleanup triggers an
  unmapped read at a garbage address. Likely a `fclose`/`__stdio_exit`
  path issue where a `FILE*` struct field is read after the underlying
  buffer has been reused. Investigating.

- **glibc 2.36+ static binaries** still hit a decode error on an
  unhandled instruction — unchanged from 1.1.0-rc.2.

- **`toybox-aarch64`** still exits with code 1 at PC=0 — unchanged.
  The STP/LDP mode calculation bug described in 1.1.0-rc.2's notes
  is still pending the v2.0 hierarchical decoder restructure.

### Compatibility Matrix
| Binary | 1.1.0-rc.2 | 1.1.1-alpha.1 |
|--------|------------|---------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works |
| `hello_arm64_musl` (static) | ✅ Works | ✅ Works |
| `loop.elf` (musl static-PIE, `-O2`) | ❌ Truncated output | ✅ **Works (new!)** |
| `test_recursion.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_structs.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_bitops.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_switch.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_advanced.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_argv.elf` (musl static) | ❌ n/a | ✅ **Works (new!)** |
| `test_fileio.elf` (musl static) | ❌ n/a | ⚠️ Partial (file prints, then crash) |
| `test_float.elf` (musl static) | ❌ n/a | ❌ Hangs in `printf("%f")` |
| `test_malloc.elf` (musl static) | ❌ n/a | ❌ Hangs in mallocng init |
| `hello_arm64_static` (glibc) | ⚠️ Decode error | ⚠️ Decode error (unchanged) |
| `toybox-aarch64` | ⚠️ Exit 1 (PC=0) | ⚠️ Exit 1 (PC=0, unchanged) |

### Next Up (1.1.1-alpha.2 / 1.1.1-beta.1)
1. Fix `printf("%f", ...)` — audit FP arithmetic emulation.
2. Fix `malloc`/`free` — proper `MAP_FIXED` overlap handling in `mmap`.
3. Fix `test_fileio` crash on `fclose` — likely stdio cleanup bug.
4. More test programs: threads (`pthread_create`), networking
   (`socket`/`connect`/`send`/`recv`), signals (`signal`/`kill`).

---

## [1.1.0-rc.2] — 2026-06-17

Major refactor: split the monolithic `arm64_emu.cpp` into separate files,
added real FP/SIMD arithmetic, VFS, and a public API header. The codebase
is now structured for the v2.0 JIT (shared decoder between interpreter
and future JIT compiler).

### Added
- **Real FP/SIMD arithmetic** — previously all FP ops were stubbed as NOP.
  Now implements:
  - **Arithmetic**: FADD, FSUB, FMUL, FDIV, FMAX, FMIN, FNMUL
  - **1-source**: FABS, FNEG, FSQRT, FRINTN/P/M/Z/A/X/I (all rounding modes)
  - **Convert**: FCVT (S↔D), FCVTZS/FCVTZU (FP→int), SCVTF/UCVTF (int→FP)
  - **Compare**: FCMP/FCMPE with NaN handling, FCMP #0.0
  - **Other**: FMOV (immediate decode), FCSEL, FMADD/FMSUB (fused multiply-accumulate)
  - All use C++ native double/float with IEEE 754 semantics, both S and D registers.
- **VFS (Virtual File System)** — synthetic `/proc` and `/dev` entries:
  - `/proc/self/{exe,cmdline,maps,status,auxv,environ}`
  - `/proc/{meminfo,cpuinfo,version}`
  - `/proc/sys/kernel/osrelease`
  - `/dev/{null,zero,urandom,random}`
  - Uses `memfd_create` for seekable virtual file descriptors.
- **File split** — `arm64_emu.cpp` split into:
  - `decoder.hpp` / `decoder.cpp` — pure instruction decode (shared with future JIT)
  - `interpreter.cpp` — `Emulator::execute()` (instruction execution)
  - `syscalls.cpp` — `Emulator::syscall()` (all syscall handlers + VFS + threads)
  - `graphics.hpp` / `graphics.cpp` — `GraphicsBackend` (framebuffer stub for 1.3.0)
  - `api/bifrost.h` — public C API for `libbifrost`
- **SIMD STP/LDP** — 32/64/128-bit pair store/load now handled (was silently dropped)
- **STP/LDP handler** moved before logical handler (prevents ORR collision)
- **Trace** now includes x29 (FP) and x30 (LR) in debug output

### Fixed
- **readv syscall number** — was case 73 (pselect6!), now case 67 (readv). This
  was a pre-existing bug: musl calls pselect6 (73) and our code ran the readv
  handler with wrong args, causing unmapped reads at 0x1000.
- **CAS argument order** — Rs is the comparand, Rt is the new value (was swapped).
- **LSE atomics handler** — moved to top level (was nested inside exclusive
  load/store handler, unreachable due to encoding group mismatch).
- **LSE atomics opcode table** — each opcode is a distinct operation
  (0=LDADD, 1=LDCLR, 2=LDEOR, 3=LDSET, 4-7=SMAX/SMIN/UMAX/UMIN, 8=SWP, C-F=CAS).
- **mmap MAP_FIXED** — hint only honored when MAP_FIXED is set; replaced pages zeroed.
- **mremap** — grows mappings in-place (critical for musl's meta_area tracking).
- **getppid** syscall added (was missing entirely).
- **set_tid_address / gettid** — now properly per-thread.

### Known Issues
- **toybox** crashes at PC=0 due to STP/LDP mode calculation bug. The "correct"
  mode calc (bits 25:24) breaks musl hello (exits 133). The old "buggy" mode
  calc (bits 24:23) works for musl but corrupts toybox's stack. Proper fix
  requires hierarchical decoder restructuring — planned for v2.0 alongside JIT.
- **glibc 2.36+ static binaries** hit a decode error on an unhandled instruction.
- **No signal delivery** — `rt_sigaction` is a no-op. Planned for 1.2.0.
- **No dynamic linking** — static binaries only.

### Compatibility Matrix
| Binary | 1.0.0-beta.1 | 1.1.0-beta.1 | 1.1.0-rc.2 |
|--------|--------------|---------------|------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_musi` | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | ⚠️ Exit 1 | ⚠️ Decode error |
| `toybox-aarch64` | ❌ Hangs (atomics) | ⚠️ Exit 1 (past mallocng) | ⚠️ Exit 1 (PC=0) |

### File Structure
```
bifrost-emu/
├── arm64_emu.hpp         Emulator class (CPU, Memory, ElfLoader, threads)
├── decoder.hpp           Shared decode tables (interpreter + future JIT)
├── decoder.cpp           Pure instruction decode
├── interpreter.cpp       Emulator::execute() — instruction execution
├── syscalls.cpp          Emulator::syscall() — all syscalls + VFS + threads
├── graphics.hpp          GraphicsBackend class (framebuffer)
├── graphics.cpp          Graphics impl (stub for 1.3.0)
├── api/bifrost.h         Public C API for libbifrost
├── main.cpp              CLI entry point
├── mini_arm64_asm.py     Built-in ARM64 assembler
├── test/                 Sample ARM64 programs
├── Makefile              Build, test, install targets
├── CHANGELOG.md          This file
├── README.md             Project documentation
└── LICENSE               Public domain (Unlicense)
```

---

## [1.1.0-beta.1] — 2026-06-17

**The mallocng loop is broken.** Toybox now gets past musl's mallocng
initialization — no more infinite `brk()` loop — and exits with code 1
on a null pointer dereference (a different, much simpler bug). glibc
static hello also gets further (exit 1 instead of hanging or 133).

The root cause turned out to be a structural bug in the instruction
decoder: the entire LSE atomics handler was unreachable, so every
CAS / LDADD / LDCLR / SWP / etc. was silently treated as a NOP.
This broke musl's lock acquisition, which caused the mallocng loop.

### Fixed (major)
- **LSE atomics handler was unreachable** (the big one). The LSE
  atomics block (CAS, LDADD, LDCLR, LDEOR, LDSET, SMAX, SMIN, UMAX,
  UMIN, SWP) was nested inside the exclusive load/store handler,
  which checks `bits 29:24 == 001000`. But LSE atomics have
  `bits 29:24 == 111000` — a completely different encoding group.
  The LSE atomics code was never reached; every LSE atomic was a NOP.

  Fix: moved the LSE atomics handler to the top level (before all
  load/store handlers), with proper bit checks to distinguish LSE
  atomics from regular load/store encodings that share bits 29:24 ==
  111000:
    - `bit 21 = 0` (load/store reg-offset has bit 21 = 1)
    - `bits 11:10 = 00` (LSE atomics fixed bits)

- **CAS argument order**. In `CAS <Rs>, <Rt>, [<Rn>]`, **Rs** is the
  comparand and **Rt** is the new value. The previous code had these
  swapped (used Rt as comparand, Rs as new value), which would have
  broken every CAS even if the handler had been reachable.

### Fixed (minor, from 1.1.0-alpha.1 carryover)
- **mremap** now grows mappings in-place by mapping additional pages
  at `old_addr + old_size`. Previously, mremap always allocated new
  memory + copied, which broke musl's meta_area tracking (musl expects
  mremap to grow mappings in-place when possible).
- **mmap MAP_FIXED zeroing**: when mmap is called with MAP_FIXED over
  existing pages, the old data is now zeroed out (matching Linux kernel
  behavior).
- **mmap MAP_FIXED hint handling**: the `addr` hint is now only honored
  when `MAP_FIXED` (0x10) is set. Without MAP_FIXED, the bump allocator
  picks a fresh address.
- **getppid** syscall added (was missing entirely, returned -ENOSYS).
- **set_tid_address** / **gettid** now properly per-thread.

### Changed
- **Stack size** increased from 8 MB to 64 MB.
- **Stack top** moved from `0x7ff0000000` to `0x8000000000` (avoids
  a 1-page guard-region conflict that was causing unmapped reads
  during deep musl recursion).
- **LSE atomics opcode table** corrected: each opcode is a distinct
  operation (0=LDADD, 1=LDCLR, 2=LDEOR, 3=LDSET, 4-7=SMAX/SMIN/
  UMAX/UMIN, 8=SWP, C-F=CAS variants). Previously, opcodes 0-3 were
  all treated as LDADD, 4-7 as LDCLR, etc.

### Known Issues
- **musl-static toybox** now gets past mallocng init but exits with
  code 1 on a null pointer dereference (PC=0). This is a different,
  simpler bug than the mallocng loop — likely a signal delivery or
  function-return issue. Investigation in 1.1.0-rc.1.
- **glibc 2.36+ static binaries** exit 1 instead of hanging or
  exiting 133. Same root cause as toybox — gets further but hits a
  null pointer.
- **No FP/SIMD arithmetic**, **no signal delivery**, **no dynamic
  linking** — unchanged.

### Compatibility Matrix
| Binary | 1.0.0-beta.1 | 1.1.0-alpha.1 | 1.1.0-beta.1 |
|--------|--------------|---------------|--------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_musi` | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | ❌ Hangs (brk loop) | ⚠️ Exit 1 (gets further) |
| `toybox-aarch64` | ❌ Hangs (atomics) | ❌ Hangs (mallocng) | ⚠️ Exit 1 (**past mallocng!**) |

### Next up (1.1.0-rc.1)
- Debug the PC=0 crash in toybox. Likely a function return issue —
  trace what's at the top of the stack when PC becomes 0, check if
  a `RET` is reading a corrupted LR or if a function pointer is NULL.
- Implement signal delivery (`rt_sigaction` + `rt_sigreturn` +
  trampoline page). Some musl init paths install signal handlers
  and expect them to work for SIGSEGV / SIGSYS.
- Audit existing 100 instructions for correctness bugs (NZCV flag
  edge cases, sign-extension issues).
- Implement FP/SIMD arithmetic (`FADD`/`FMUL`/`FCVT`/`FCMP`).

---

## [1.1.0-alpha.1] — 2026-06-17

First alpha toward the 1.1 "stability" release. Three real bugs in the
atomic / memory subsystem are fixed, but glibc-static and toybox still
don't run end-to-end (they get past the previous failure points but hit
a deeper musl-mallocng recursion issue that needs investigation).

### Added
- **Local exclusive monitor** for `LDXR`/`STXR`/`LDAXR`/`STLXR`/`CLREX`.
  Per-CPU monitor state (`excl_tag_valid`, `excl_tag_addr`,
  `excl_tag_size`) tracks the most recent exclusive load. `STXR` now
  succeeds only if the monitor is tagged for an overlapping address
  range, and clears the monitor either way. Any branch, SVC, or
  non-exclusive store also clears the monitor (conservative — real HW
  only clears on conflicting access, but clearing more often is always
  safe). `CLREX` (encoding `0xD503305F`) is now explicitly decoded
  instead of being treated as a NOP.

### Fixed
- **LSE atomics opcode table** (major). The previous code mapped
  opcodes 0-3 to LDADD variants, 4-7 to LDCLR variants, 8-11 to LDEOR
  variants, and 12-15 to LDSET variants — treating the 4-bit opcode
  as if it encoded the A/L ordering suffix. In reality, each opcode is
  a different operation: 0=LDADD, 1=LDCLR, 2=LDEOR, 3=LDSET, 4-7=
  SMAX/SMIN/UMAX/UMIN, 8=SWP, 12-15=CAS variants. The old code
  computed `a + b` for what should have been `a & ~b` (LDCLR), causing
  musl's lock bit to never be properly cleared.
- **CAS detection**. The old code used a partial mask that never
  matched real CAS instructions. CAS is encoded within the LSE atomic
  ops space (bits 29:24 = 111000) with opcodes 0xC-0xF. Now detected
  by checking `atom_opcode >= 0xC` before the LSE switch. CAS
  semantics: load old value, compare against Rt, store Rs if matched,
  always return old value in Rt.
- **mmap MAP_FIXED handling**. The previous code honored the `addr`
  hint unconditionally, returning the same address musl asked for even
  when `MAP_FIXED` wasn't set. This caused musl's malloc to think each
  `mmap(heap_end, ...)` succeeded without actually getting new memory,
  leading to a 4KB-at-a-time heap growth loop. Now: `addr` is only
  honored when `MAP_FIXED` (0x10) is set; otherwise we ignore the
  hint and use the bump allocator (matching Linux kernel behavior).

### Changed
- `set_tid_address` now stores the pointer per-thread and returns the
  calling thread's TID (was returning 1 unconditionally).
- `gettid` returns the guest TID of the calling thread (was returning
  `getpid()`).

### Known Issues
- **musl-static toybox** still hangs. The previous failure (LDXR/STXR
  always succeeding) is fixed, but toybox now hits a different bug:
  musl's mallocng enters a deep recursion (SP drops ~54KB per
  iteration) when allocating memory during `__libc_start_main`. Each
  iteration calls `brk()` to extend the heap by 4KB and recurses
  further. Likely cause: musl's "growable array" tracking structure
  isn't being updated correctly, possibly due to a subtle memory
  ordering or atomic semantics issue we haven't pinned down yet.
- **glibc 2.36+ static binaries** now hang instead of exiting 133.
  The mmap MAP_FIXED fix changed the address glibc receives, and the
  getrandom vDSO assertion is no longer triggered — but glibc enters
  its own brk loop before reaching main(). Probably related to the
  same mallocng-style issue as toybox.
- **No FP/SIMD arithmetic**, **no signal delivery**, **no dynamic
  linking** — unchanged from 1.0.0-beta.1.

### Compatibility Matrix
| Binary | 1.0.0-beta.1 | 1.1.0-alpha.1 |
|--------|--------------|---------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works |
| `hello_arm64_musi` | ✅ Works | ✅ Works |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | ❌ Hangs (brk loop) |
| `toybox-aarch64` | ❌ Hangs (LDXR/STXR) | ❌ Hangs (mallocng recursion) |

### Next up (1.1.0-beta.1) — *completed in 1.1.0-beta.1*
- ~~Investigate the musl mallocng recursion~~ — root cause found:
  the LSE atomics handler was unreachable due to an encoding-group
  mismatch (bits 29:24 == 001000 vs 111000). Fixed in 1.1.0-beta.1.
- Audit existing 100 instructions for correctness bugs (NZCV flag
  edge cases, sign-extension issues). — *still pending*
- Implement signal delivery (`rt_sigaction` + `rt_sigreturn` +
  trampoline page). — *still pending*
- Implement FP/SIMD arithmetic (`FADD`/`FMUL`/`FCVT`/`FCMP`). — *still pending*

---

## [1.0.0-beta.1] — 2026-06-17

First tagged release. The interpreter is stable enough to run musl-static
ARM64 binaries and a small set of glibc-static binaries; threads and the
event-loop syscalls are in place but not yet battle-tested against real
multi-threaded workloads.

### Added
- **Versioning**: `1.0.0-beta.1` semver string, `--version` flag, `VERSION`
  constant exported from `arm64_emu.hpp`.
- **Threading support** (the big one):
  - `clone()` syscall now spawns real OS threads instead of returning
    `-ENOSYS`. Supports `CLONE_VM`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`,
    `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`. Fork-style clones
    (no `CLONE_VM`) still return `-ENOSYS`.
  - `futex()` is now a real implementation using per-address
    `(mutex, condvar)` pairs. Supports `FUTEX_WAIT`, `FUTEX_WAKE`,
    `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_REQUEUE`,
    `FUTEX_CMP_REQUEUE` (treated as wake). PI futexes return `-ENOSYS`.
  - `Memory` class is now thread-safe (per-page mutex on access).
  - `Emulator` now owns a pool of `GuestThread`s, each with its own
    `CPU` state and running on a real `std::thread`. The main thread
    runs on `main_cpu_`; cloned threads run on the pool.
  - `set_tid_address()` now stores the pointer per-thread and returns
    the calling thread's TID.
  - `gettid()` returns the guest TID of the calling thread.
- **Event-loop syscalls** (delegate to host kernel):
  - `eventfd2` (#19), `epoll_create1` (#20), `epoll_ctl` (#21),
    `timerfd_create` (#85), `timerfd_settime` (#86), `timerfd_gettime` (#87),
    `pselect6` (#72), `ppoll`/`poll` (#168), `socketpair` (#199),
    `listen` (#201), `accept` (#202), `clock_nanosleep` (#206),
    `rt_sigreturn` (#133), `sendfile` (#40 — see Known Issues).
- **Memory atomicity primitives**: `Memory::atomic_cas_32` and
  `atomic_cas_64` for use by LSE atomics and futex implementations.
- **GitHub-ready**: `LICENSE` (public domain / Unlicense), `.gitignore`,
  `CHANGELOG.md`.
- **Build**: Now requires `-pthread` (already in the documented build
  command).

### Changed
- **Rename**: project renamed from `bifrost` to `bifrost-emu` for SEO.
  The binary, namespace comments, banner, and CLI error messages all
  reflect the new name.
- **BRK is fatal**: `BRK #imm` now ends emulation with exit code 133
  (`128 + SIGTRAP`), matching real Linux behavior. Previously it was
  skipped, which caused libc's `abort()` to fall through and dump
  random `.rodata` strings to stderr.
- **`Emulator::execute`** and **`Emulator::syscall`** now take a `CPU&`
  parameter instead of operating on the implicit `cpu_` member. This
  was necessary for multi-threading and also makes a future JIT
  (sharing the same decoder) easier to drop in.
- **Verbose output** uses `[bifrost-emu]` prefix instead of `[emu]`.
- **Instruction trace** now includes the guest TID: `[trace tid=1]`.

### Known Issues
- **Syscall number collisions**: a handful of legacy case labels in the
  syscall switch use numbers that conflict with the real AArch64 syscall
  table (e.g., `case 22` is labeled `pipe2` but is actually
  `epoll_pwait` on AArch64). These work for the test programs because
  musl hello / toybox happen not to use the conflicting syscalls, but
  real-world binaries that exercise `epoll_pwait`, `ppoll`, or
  `sendfile` (real #71) may misbehave. **Plan for v1.0.0-rc.1**: audit
  and renumber all syscalls against `linux/unistd.h`.
- **glibc 2.36+ static binaries** still hit the `getrandom` vDSO
  assertion during libc init and exit with code 133. musl-static
  binaries work perfectly.
- **No signal delivery**: `rt_sigaction` and `rt_sigprocmask` are
  no-ops; signals cannot actually be caught. `tgkill` / `tkill` return
  success but deliver nothing. This means timer-based code, segfault
  handlers, and `pthread_kill` will not work.
- **No FP/SIMD arithmetic**: `FADD`, `FMUL`, `FMLA`, etc. are stubbed.
  Loads/stores of FP/SIMD registers work, but any computation will
  produce garbage. This blocks most games and many numeric libraries.
- **No exclusive monitor**: `LDXR`/`STXR`/`LDAXR`/`STLXR` do not track
  exclusive reservations. `STXR` always reports success. This causes
  musl's malloc to loop forever in `toybox` (see issue tracker).
- **`bind` and `connect` return `-ENOSYS`**: marshalling `sockaddr`
  from guest memory safely requires knowing the address family, which
  we don't currently parse.

### Compatibility Matrix
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

### Internal Architecture Notes
- **JIT preparation**: the `execute()` and `syscall()` methods now take
  a `CPU&` parameter, which is the only state they touch (plus the
  shared `Memory`). A future v2.0 JIT can compile from the same decoder
  tables by emitting code that operates on a `CPU*` argument.
- **Memory locking**: per-page mutex on every read/write. This is
  conservative; for v1.1 we can switch to a read-write lock and skip
  locking entirely for instruction fetches (which are read-only and
  never race with themselves).

---

## [0.9.0] — 2026-06-16 (pre-release, untagged)

Initial development version. Single-threaded, no futex, no clone.
- Working interpreter for ~100 ARM64 instructions
- musl-static hello world works
- glibc-static hello world fails on getrandom vDSO assertion
