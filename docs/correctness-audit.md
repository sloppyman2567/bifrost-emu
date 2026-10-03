# Emulator Correctness Audit Findings

Updated: 2026-09-25

This is a historical audit snapshot. Its per-finding verification statements
describe that fix pass. On 2026-10-02 the current build passed the quick suite
(239 JIT checks, 237 interpreter checks plus two expected skips), as well as
standalone TLS and graphics-thunk regressions documented in [TESTS.md](TESTS.md).
Those results do not individually close every finding in this ledger or the
broader audit.

This ledger records the 26 reported correctness findings and the implementation
status from the 2026-09-25 fix pass. Source changes have been made for all listed
findings. No build or tests were run, so these changes still need execution
verification.

## JIT concurrency and atomics

### C-01 — Unlocked JIT block-map lookup races translation (P1)

- **Where:** `include/jit/frostjit.hpp`, `lookup_only()` and
  `lookup_call_target()`.
- **Finding:** shared-JIT execution can look up `blocks_` while a call helper
  translates a new block and mutates or rehashes the same `unordered_map`.
  Concurrent read and mutation is undefined behavior and can crash or corrupt
  threaded guests.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-02 — JIT narrow LSE atomics use incorrect host widths/encoding (P1)

- **Where:** JIT LSE atomic lowering in `src/jit/`.
- **Finding:** byte LSE operations still use word opcodes (for example, the
  LDADD path emits XADD r/m32 instead of XADD r/m8), so they can read or
  update four bytes instead of one. Other narrow non-CAS paths have the same
  width mismatch.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-03 — JIT atomic accesses do not synchronize with interpreter atomics (P1)

- **Where:** JIT atomic lowering and the interpreter's atomic shard locking.
- **Finding:** JIT atomics bypass the mutex used by interpreter atomic
  operations. Concurrent guest threads executing a mix of JIT and interpreter
  code can race on the same guest location.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-04 — Ordinary stores do not invalidate exclusive reservations (P1)

- **Where:** ordinary `STR` and LSE store paths versus `LDXR`/`STXR` monitor
  state in `src/interp/` and `src/jit/`.
- **Finding:** a normal store or LSE atomic write to a reserved location does
  not clear another CPU's exclusive reservation. A later `STXR` can report
  success when the architecture requires the reservation to have failed.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-05 — Valid AArch64 LDR literal encodings are rejected (P1)

- **Where:** `src/frontend/decoder.cpp:689-690`.
- **Finding:** the shared decoder rejects valid PC-relative literal-load
  encodings with bit 29 clear. Both interpreter and JIT therefore fail to
  execute those instructions.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-06 — LDPSW zero-extends instead of sign-extending (P1)

- **Where:** `src/frontend/decoder.cpp:408`,
  `src/interp/interpreter.cpp:1100`, and `src/ir/ir_translate_mem.cpp:303`.
- **Finding:** the two loaded 32-bit words are zero-extended. LDPSW must
  sign-extend each word to 64 bits, so negative values become large positive
  values in both execution modes.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-07 — ADD/SUBS immediate with Rn=31 reads XZR instead of SP (P2)

- **Where:** `src/frontend/decoder.cpp:461`,
  `src/interp/interpreter.cpp:324-325`, and
  `src/ir/ir_translate.cpp:161-168`.
- **Finding:** the ADD/SUB immediate encoding reads SP whenever `Rn=31`,
  including flag-setting forms. The decoder records `reads_sp` only when
  `S=0`; the interpreter and IR translation therefore read XZR for ADDS/SUBS
  with `Rn=31`, producing incorrect results and NZCV flags. See Arm's
  [ADD immediate pseudocode](https://finkmartin.com/aarch64/add_addsub_imm.html)
  and [SUBS immediate pseudocode](https://finkmartin.com/aarch64/subs_addsub_imm.html).
- **Status:** Source fix applied in this pass; execution verification not run.

### C-08 — LDRSB/LDRSH to W registers retain sign bits above bit 31 (P2)

- **Where:** `src/interp/interpreter.cpp:1237` and
  `src/ir/ir_translate_mem.cpp:141`; applies to immediate, unscaled, and
  register-offset forms.
- **Finding:** these forms sign-extend to 64 bits. A W-register destination
  must instead produce a 32-bit result and clear the upper half of Xn.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-09 — FPCR rounding mode is not applied to arithmetic/rounding (P2)

- **Where:** `src/interp/interp_branch.cpp:340`,
  `src/jit/frostjit.cpp:777`, `src/jit/jit_codegen_fp.cpp:102`, and
  `src/interp/interp_fp.cpp:4120`.
- **Finding:** guest FPCR is stored but not synchronized with host rounding
  state. Host FP arithmetic and FRINT operations can therefore use a different
  rounding mode from the one selected by the guest.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-10 — SIMD FMLA/FMLS fallback paths can double-round (P2)

- **Where:** interpreter SIMD FP arithmetic and the JIT fallback used when
  FMA3 is unavailable.
- **Finding:** multiply followed by add/sub is not fused and can produce a
  different rounded result from AArch64 FMLA/FMLS. The JIT FMA3 path is fused;
  the interpreter SIMD and JIT non-FMA3 paths still use separate operations.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-11 — FMULX has incorrect and missing scalar/vector implementations (P2)

- **Where:** `src/interp/interp_fp.cpp:1036,1076,4620-4673`,
  `src/jit/jit_codegen_simd.cpp:496`, and scalar FP translation fallback.
- **Finding:** Arm FMULX returns signed 2.0 when one operand is zero and the
  other is infinity ([Arm FMULX reference](https://df.lth.se/~getz/ARM/A64/fmulx_advsimd_vec.html)).
  The vector implementation instead uses ordinary multiplication. Scalar
  FMULX encodings, including by-element forms, fall through to the interpreter's
  unknown-FP no-op path, so the JIT fallback also leaves the destination
  unchanged.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-12 — JIT FMAXNM/FMINNM mishandles a NaN in operand 2 (P2)

- **Where:** `src/jit/jit_codegen_fparith.cpp:176` and
  `src/jit/jit_codegen_simd.cpp:503`; interpreter reference at
  `src/interp/interp_fp.cpp:3939`.
- **Finding:** x86 MIN/MAX returns the second operand when either operand is
  NaN. For one quiet NaN and one numeric input, AArch64 FMAXNM/FMINNM selects
  the numeric value. The JIT can return NaN for `number, qNaN`.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-13 — Scalar accesses crossing the direct-window boundary split storage (P2)

- **Where:** `src/core/memory.cpp`, direct-window and sparse-page read/write
  paths.
- **Finding:** an access that straddles the 4 GiB boundary falls through to
  the sparse-page path for the whole access. Its low-address bytes are then
  read from or written to `pages_` instead of the direct window, creating a
  shadow copy of part of the guest range.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-14 — Guest mapping faults and protections are not enforced (P1)

- **Where:** `src/core/memory.cpp:238` (`is_mapped`), `:345` (`write`),
  `:400` (`read`); `src/syscalls/mem.cpp:356` (`mprotect`).
- **Finding:** every range wholly inside the 4 GiB direct window is reported
  mapped. Direct-window reads/writes do not check guest mappings; sparse-page
  reads and writes create storage for missing pages. `mprotect` returns success
  without recording access permissions. Unmapped accesses, read-only mappings,
  and PROT_NONE mappings can therefore silently access or modify guest state.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-15 — JIT invalidation misses page-rounded changes (P1)

- **Where:** `src/syscalls/mem.cpp:236`, `:254`, `:352`, and `:446`;
  `src/core/memory.cpp:965` (`untrack_allocation`);
  `src/jit/jit_cache.cpp:218` (`invalidate_range`).
- **Finding:** `munmap` and MAP_FIXED replace/free page-rounded ranges but pass
  the original byte length to JIT invalidation, which checks exact byte-range
  overlap. For example, `munmap(page, 1)` frees the page but can leave cached
  blocks later in it. MADV_DONTNEED changes memory without invalidating JIT
  translations; it also clears the exact byte range instead of whole pages.
  Cached code can run after its backing bytes were removed, replaced, or
  discarded.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-16 — MAP_FIXED_NOREPLACE is neither atomic nor complete (P1)

- **Where:** `src/syscalls/mem.cpp:104-140`; main ELF PT_LOAD mapping at
  `src/frontend/elf_loader.cpp:103-110` and
  `src/core/emulator.cpp:764-769`.
- **Finding:** the handler checks an allocation snapshot, then allocates under
  a separate lock, and the hinted allocator does not recheck overlap. Two
  guest threads can both succeed at the same address. The snapshot also misses
  main-executable PT_LOAD ranges installed with `map_range`, so a request can
  succeed over executable text instead of returning EEXIST.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-17 — MAP_FIXED_NOREPLACE can expose stale direct-window data (P2)

- **Where:** `src/syscalls/mem.cpp:140` and
  `src/core/memory.cpp:478-481`, `:585-601`.
- **Finding:** after `munmap`, direct-window bytes remain in place. The
  NOREPLACE path supplies a nonzero hint, so `mmap_alloc` does not mark the
  range as reused and skips its direct-window zeroing. A new anonymous mapping
  at that address can reveal bytes from the prior mapping.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-18 — Unaligned munmap frees the containing page (P2)

- **Where:** `src/syscalls/mem.cpp:341-352` and
  `src/core/memory.cpp:965-979`.
- **Finding:** the syscall rejects wrapping ranges but does not reject an
  unaligned address. `untrack_allocation` rounds the address down and frees
  the containing page, so an invalid call such as `munmap(ptr + 1, 1)` can
  discard a live allocation and return success.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-19 — pkey_mprotect applies a guest address to host memory (P2)

- **Where:** `src/syscalls/misc_extended.cpp:921-927`.
- **Finding:** the handler calls host `::mprotect` with the numeric guest
  address. It usually returns a host error instead of changing guest mapping
  state, and can change a host mapping if the numeric address overlaps one.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-20 — MOVK W preserves stale upper bits in the JIT (P2)

- **Where:** `src/ir/ir_translate.cpp:66-76`; interpreter reference at
  `src/interp/interpreter.cpp:302-307`.
- **Finding:** the W-form MOVK mask ORs ones into the upper 32 bits, then
  merges them from the old 64-bit register value. AArch64 W-register writes
  must clear bits 63:32. The interpreter masks its old value to 32 bits, but
  the translated JIT result can retain stale high bits.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-21 — CLZ feature fallback leaves the JIT result undefined (P2)

- **Where:** `src/jit/jit_codegen_alu.cpp:349-356`; CLZ translation at
  `src/ir/ir_translate.cpp:600-609`; synthetic CLZ in CLS at `:672-676`.
- **Finding:** when the host lacks LZCNT, codegen calls the interpreter and
  returns without defining the CLZ IR destination. The translated instruction
  then continues through its ZEXT/STORE_REG uses, which can overwrite the
  interpreter's correct architectural result with an undefined temporary.
  CLS has an additional failure: its synthetic CLZ carries `arm_pc=0`, so
  that fallback steps guest code at address zero instead of computing CLZ.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-22 — FP-to-integer feature fallback overwrites the interpreter result (P2)

- **Where:** `src/ir/ir_translate_fp.cpp:310-324` and
  `src/jit/jit_codegen_fparith.cpp:332-339`.
- **Finding:** for signed FCVT rounding modes that need SSE4.1, a host without
  SSE4.1 executes the guest conversion through the interpreter, then continues
  with the translated ZEXT/STORE_REG that consumes the still-undefined FP_F2I
  temporary. This can overwrite the correct interpreter result with stale
  data for both W- and X-register destinations.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-23 — CRC32C lookup table contains incorrect entries (P2)

- **Where:** `src/interp/interpreter.cpp:889-922`; `CRC32` has no native IR
  lowering and falls back to the interpreter in JIT mode as well.
- **Finding:** the Castagnoli table diverges from the reflected CRC32C table
  starting at index `0x15`. For example, this table has `0x251D3B73` at that
  index, while a reference CRC32C table has `0x25AFD373`
  ([table source](https://github.com/EarthScope/libmseed/blob/main/crc32c.c)).
  CRC32C byte/halfword/word/doubleword instructions therefore produce wrong
  results for affected input bytes in both execution modes.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-24 — AES interpreter dispatch misdecodes the round operation (P2)

- **Where:** `src/interp/interp_crypto.hpp:390-412`.
- **Finding:** the AES block matches only the exact AESE encoding with mask
  `0xFFFFFC00`, then tries to select AESE/AESD/AESMC/AESIMC from bits[11:10].
  Those bits are fixed for this instruction family; the four operations are
  distinguished by opcode bits[15:12]. As written, AESE takes the AESMC case,
  while AESD/AESMC/AESIMC miss this dispatch. The JIT sends these operations
  through the interpreter too. Arm's [AESE pseudocode](https://df.lth.se/~getz/ARM/A64/aese_advsimd.html)
  confirms the different round operation.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-25 — SHA1 schedule updates do not match Arm semantics (P2)

- **Where:** `src/interp/interp_crypto.hpp:246-267`.
- **Finding:** `sha1su0()` XORs corresponding lanes directly, but Arm's
  [SHA1SU0 operation](https://df.lth.se/~getz/ARM/A64/sha1su0_advsimd.html)
  first concatenates Vn's low 64 bits with Vd's high 64 bits, then XORs that
  shifted value with Vd and Vm. `sha1su1()` also uses a different lane
  recurrence from Arm's [SHA1SU1 operation](https://df.lth.se/~getz/ARM/A64/sha1su1_advsimd.html).
  Guest SHA-1 implementations using these schedule instructions compute wrong
  message words in both modes because JIT falls back to the interpreter.
- **Status:** Source fix applied in this pass; execution verification not run.

### C-26 — SHA256 schedule updates do not match Arm semantics (P2)

- **Where:** `src/interp/interp_crypto.hpp:343-373`.
- **Finding:** `sha256su0()` and `sha256su1()` use lane formulas that differ
  from Arm's shifted-concatenation and rolling-result operations. The current
  SHA256SU0 helper omits the corresponding Vd lane in its sum; SHA256SU1 uses
  a different recurrence and applies `sig0` where Arm applies `sig1`.
  See Arm's [SHA256SU0 operation](https://df.lth.se/~getz/ARM/A64/sha256su0_advsimd.html)
  and [SHA256SU1 operation](https://df.lth.se/~getz/ARM/A64/sha256su1_advsimd.html).
  SHA-256 implementations using these schedule instructions compute wrong
  message words in both modes because JIT falls back to the interpreter.
- **Status:** Source fix applied in this pass; execution verification not run.

## Disposition

- The variable-shift alias concern was dropped after tracing guest
  instructions through IR showed that normal translations use a fresh
  destination; it is not tracked as an open finding.
- C-01 through C-26 have source fixes in the worktree. None has been run
  through a build or test suite during this pass.
- The broader correctness audit remains open beyond these reported findings;
  this list does not claim that every emulator subsystem has been reviewed.
