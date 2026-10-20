# FrostJIT

Read [the JIT guide](../../Docs/JIT.md) before implementation, debugging or review.
The [source map](../../Docs/ARCHITECTURE.md) describes subsystem boundaries.

- `Compiler/BlockAnalysis.*` owns fallback prediction, leaf eligibility and the
  dead-NZCV successor scan (`nzcv_dead_in`);
  `BlockCompiler.cpp` orchestrates Tier-1 compilation; `VerificationStores.cpp`
  records differential replay footprints. Prediction must track actual fallback
  support, but final fallback counts come from optimized IR.
- `IR/` owns typed instructions, translation, scalar SSA, optimization and
  validation. Factories/readers are the parameter contract. SSA currently models
  scalar values; do not assume MemorySSA or complete vector/FP state coverage.
- `CodeGen/` owns operation emission; `Backend/` owns host instruction encoding;
  `RegisterAlloc/` owns scalar mappings and spills; `Runtime/` owns dispatch,
  caching, fault recovery and profiling. Tier-2 has separate planning/emission
  and publication contracts; Tier-1 success alone does not validate it.
- Preserve aliases and precise fault state. Spill/drop mappings before
  overwriting a live host register. Taken-path emission must not consume dirty
  state needed while emitting the fall-through path. Keep bounds, permissions
  and userfaultfd guards intact.
- Keep generated decode/signature tables in `Include/Generated/` synchronized
  through `Tools/CodeGen/`, never manual generated-header edits. Native SIMD
  helper encodings live in `native_simd.txt`. After changing it or a helper,
  run `make nativegen-expect` (QEMU) and commit the spec, generated header,
  generated test and expectations together. Inline bodies in
  `CodeGen/Native/NativeSIMDInline.cpp` must match their `run<K>` lane for
  lane, and `native_simd_inlinable` must accept exactly what the emitter
  emits (Tier-2 relies on it to skip the call treatment).
- A pair (`LDP`/`STP`/`LDPSW`) uses the native guarded path. Its translation must
  complete every load before writing any architectural register (and write the base
  last), and a store pair must write none: a fault on the second half restarts the
  whole instruction. `jit_host_fault` checks this, including `ldp x13, x12, [x13]`.
- The exclusive monitor is `System/Core/ExclusiveMonitor.hpp`; the interpreter, the
  `jit_ldxr/stxr/stlr` helpers and `CodeGen/Integer/Atomics.cpp` implement one
  protocol and must change together: version read before the value at LDXR; STXR/STLR
  hold the bucket across check and write and bump on a write. Never bump before the
  write without the lock (that loses ABA protection under real parallelism; only
  `jit_llsc_native`'s four-CPU LIFO shows it). The access is a registered fault site
  whose stub releases the bucket first. `CLEAR_EXCLUSIVE` implements CLREX.
- `flags_match_pstate_` (host flags loaded by a BRCOND, unchanged since) lets exits and
  `clobber_flags` skip the store; every flag setter must clear it, and only public
  Tier-1 compiles use it (`host_flags_match_pstate()`). A BRCOND on one flag with its flags
  still in `pstate` tests the bit in memory. `clobber_flags(false, true)` (RDX only) is for
  callers that pick registers through the allocator afterwards; others assume RAX/RCX/RDX.
- `fold_ahead_kind_` (BlockCompiler.cpp) must mirror its consumer's fold guards exactly: a
  skipped `mov` whose consumer then does not fold leaves the vreg unmapped. Pass 0b of
  `optimize_ir` relies on `LOAD_MEM` and the shared 32-bit `ADDS/SUBS` emitter zero-extending.
- A scalar FP load or store is `LOAD_MEM` + `FMOV_G2F` or `FMOV_F2G` + `STORE_MEM`,
  never `SIMD_LDST`: that op is not cache-compatible and would veto the XMM cache
  for the whole block. Vd is written only after the load, so a fault leaves it intact.
- Native saturating integer SIMD must set FPSR.QC like the interpreter
  (`emit_simd_qc_unless_equal`); differential verification does not compare
  FPSR, so check it against QEMU.
- Run `make verify` and relevant guest regressions after behavior changes. A
  file split also needs a complete link/library build: missing definitions and
  old archive members can evade per-file compilation checks.

## State and publication contracts

- Every op that writes an architectural GPR directly must update the optimizer's
  forwarding cache and `writes_arch_gpr_directly()` pin-selection predicate.
  Flags-only ops such as CCMP/CCMN do not write placeholder GPR destinations.
- A pre-call pinned-vector writeback must include values written anywhere in a
  loop block, including writes after the call that carry into the next iteration.
- An exit may omit its NZCV store only through `exit_flags_dead` (public Tier-1
  compiles): the successor's first words must overwrite all four flags
  (`ADDS`/`SUBS`/`ANDS`/`FCMP`) after nothing but flag-neutral, fault-free, non-branching
  integer instructions. A successor that merely does not read the flags proves nothing.
  Extend `nzcv_dead_in`'s neutral list only with instructions that cannot fault, trap,
  branch or read NZCV. The words become a block dependency (`flag_dep_table_`, checked
  by `invalidate_range`, `remember_code_granules`, `staged_entry_invalidated` and
  `block_code_unchanged`); keep all four in step. `BlockEntry` is read on every dispatch
  lookup: variable-size per-block data belongs in an append-only side table, not in it.
- Materialize lazy NZCV before flag-clobbering allocation or operand emission.
  Respect each flag helper's scratch set and ARM carry versus x86 borrow
  convention. Keep cross-block NZCV observable through flag-neutral successors.
- If codegen executes the interpreter, represent that effect as `CALL_INTERP`;
  fallback counts, verification, pinning and region admission depend on it.
  The exception is the FP NaN cold step (`emit_fp_interp_step`), which runs
  only for exceptional values. It commits, single-steps and reloads in place,
  and an op that uses it writes Vd only after its guard.
  Changes of PC/stop state must return to the dispatcher through the proper exit.
- Native BL/BLR calls honor exit-group, callback stop sentinels and bounded host
  call depth; unwind without restoring stale caller state.
- Use actual reservation bounds via the shared window-limit helpers
  (`emit_window_limit_cmp` reads the header-page table through `WIN_REG`;
  `emit_window_limit` where `WIN_REG` is not the window base). Missing-page
  tracking and W/X-only mappings can enable sticky software guards while already
  compiled code executes; preserve their runtime observation.
- Published code/metadata outlive in-flight CPUs. MT publication/invalidation
  changes atomic target cells, not live instruction bytes; preserve compiler
  locking, invalidation generations and target revocation for Tier-1/Tier-2.
- MT Tier-1 compiles run under the shared table lock (`translate_entry`).
  Codegen may read `blocks_` but must not change state that shared-lock
  readers use; route publication through `publish_or_stage` /
  `publish_translation`. Invalidation paths that skip the exclusive lock must
  log their range for in-flight compiles.
- Cache maintenance and mprotect keep a Tier-1 block whose recorded
  instruction words (`code_words_`) are unchanged. A translation must depend
  on no other guest bytes: a new compile-time guest read must be recorded or
  must leave `code_words_count` zero. Callers pass a `CodeInvalidation` reason.
- Known-entry loop growth uses cached branch metadata only as a hint; actual
  translation retains all limits and records every absorbed instruction word.
- Tier-1 deferred guard tails must restore the allocator/vector dirty state
  captured before the access and finalize fault-site offsets before publication.
  Inline cold and slow paths that the hot path skips may reload XMMs at
  runtime but must leave compile-time vector validity and dirtiness unchanged.
  Tier-2 keeps inline guards. W-form fusion is Tier-1-only and must scope its
  encoding width to the ALU operation, preserving operand setup and lazy NZCV.
- Restore the complete ST chain-skip slot through `write_unpatched_slot`;
  its unpatched form is `kSlotNop5`. MT revokes data cells, never instructions.
