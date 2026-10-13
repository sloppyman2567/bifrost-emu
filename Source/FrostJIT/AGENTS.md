# FrostJIT

Read [the JIT guide](../../Docs/JIT.md) before implementation, debugging or review.
The [source map](../../Docs/ARCHITECTURE.md) describes subsystem boundaries.

- `Compiler/BlockAnalysis.*` owns fallback prediction and leaf eligibility;
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
  generated test and expectations together.
- Run `make verify` and relevant guest regressions after behavior changes. A
  file split also needs a complete link/library build: missing definitions and
  old archive members can evade per-file compilation checks.

## State and publication contracts

- Every op that writes an architectural GPR directly must update the optimizer's
  forwarding cache and `writes_arch_gpr_directly()` pin-selection predicate.
  Flags-only ops such as CCMP/CCMN do not write placeholder GPR destinations.
- A pre-call pinned-vector writeback must include values written anywhere in a
  loop block, including writes after the call that carry into the next iteration.
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
