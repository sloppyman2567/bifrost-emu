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
- JIT lifetime across fork/exec: `fork_guest` switches the inherited JIT off in the child (its code
  buffer and locks belong to threads that no longer exist) and `execve` gives the new image a fresh
  one (`Emulator::restart_jit_for_exec`, which abandons the old JIT because the caller is still
  returning through its code buffer). Anything process-wide a JIT starts (the preemption ticker)
  must be re-startable in a fork child (`pthread_atfork`). Without this every program a shell runs
  was about 70 times slower. `test_exec_jit.sh` and `test_exec_mt` cover it.
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
  emits (Tier-2 relies on it to skip the call treatment). The JIT inlines most
  kinds, so the generated guest fixture rarely reaches a `run<K>` body: the helper
  host test (`make test-native-simd-helpers`, part of `make verify`) runs every
  body against the same QEMU hashes. `vec_inst_operands` lists each kind's
  vector reads and writes by hand; add a new kind there (a kind that accumulates
  into Vd reads it).
- A pair (`LDP`/`STP`/`LDPSW`) uses the native guarded path. Its translation must
  complete every load before writing any architectural register (and write the base
  last), and a store pair must write none: a fault on the second half restarts the
  whole instruction. `jit_host_fault` checks this, including `ldp x13, x12, [x13]`.
- The exclusive monitor is `System/Core/ExclusiveMonitor.hpp` (a fork child resets its held bits); the interpreter, the
  `jit_ldxr/stxr/stlr` helpers and `CodeGen/Integer/Atomics.cpp` implement one
  protocol and must change together: version read before the value at LDXR; STXR/STLR
  hold the bucket across check and write and bump on a write. Never bump before the
  write without the lock (that loses ABA protection under real parallelism; only
  `jit_llsc_native`'s four-CPU LIFO shows it). The access is a registered fault site
  whose stub releases the bucket first. `CLEAR_EXCLUSIVE` implements CLREX.
- `flags_match_pstate_` (host flags loaded by a BRCOND, unchanged since) lets exits and
  `clobber_flags` skip the store; every flag setter must clear it, and only public
  Tier-1 compiles use it (`host_flags_match_pstate()`). A BRCOND with its flags still in
  `pstate` tests them there: one flag as a bit, HI/LS and the signed pairs through RAX (and
  RCX). `clobber_flags(false, true)` (RDX only, ALU ops and guarded accesses) is for callers
  that pick registers through the allocator afterwards; others assume RAX/RCX/RDX and keep
  the RAX save, since they may hold a value in RAX the allocator does not know about.
- `fold_ahead_kind_` (BlockCompiler.cpp) must mirror its consumer's fold guards exactly: a
  skipped `mov` whose consumer then does not fold leaves the vreg unmapped. Kind 3 is the CSET
  form of CSEL (constants 0 and 1, read through `csel_const`). Pass 0b of
  `optimize_ir` relies on `LOAD_MEM` and the shared 32-bit `ADDS/SUBS` emitter zero-extending.
- Scalar FMUL/FMULX/FMLA/FMLS by element are `FP_BINOP`/`FMADD`/`FMSUB` with a source element
  (`lane`, IR `cond` = element + 1); `fp_decode::scalar_fp_by_element` decides for the translator
  and `BlockAnalysis` alike. The element comes from `fp_load_lane`: under the low-half FP cache an
  upper-half element is read from memory, never from the XMM.
- Vector FMLA/FMLS by element are `SIMD_FP_FMA` with a lane (`fp_decode::vector_fp_fma_by_element`,
  checked before the native SIMD table). Single-register LD1R is `LOAD_MEM` + `SIMD_DUP`.
- A single LD16/ST16 in Tier-1 is `compile_guarded_vec16`. Its 16-byte page checks stay in its own
  tail: the shared cold stubs exist for widths up to 8 (`flush_deferred_guards` checks `g.width`).
- Block growth past the cap is only for loops (`selfloop_extent`, at most twice the cap); its look-ahead
  reads mapped executable pages directly and must stay a hint that adds no code-word dependency.
- Android data TBI (`Memory::data_address`): every native data access untags its final address
  (base + offset) with `emit_untag_data_address` in a scratch copy before the bound and page checks;
  the guest register keeps its tag. A new access path that skips it is correct but interprets every
  tagged (Scudo heap) access. `data_tbi()` is part of the disk-cache fingerprint.
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
- A single-thread self-loop back-edge is a patchable `jmp` slot, patched to the body
  or, when the demand-slot layout needs reconciling, to `emit_selfloop_reconcile_stub`'s
  out-of-line stub. Reconcile code must never precede the slot: differential replay
  leaves it unpatched and falls into the exit emitted for the layout at the branch. The
  MT edge orders its checks before the reconcile for the same reason.
- Use actual reservation bounds via the shared window-limit helpers
  (`emit_window_bound_cmp` reads a header-page table through `WIN_REG` and returns
  the jcc condition: `jae` against a table, `ja` against `emit_window_limit`, which
  is for code whose `WIN_REG` is not the window base). Software checks go on and off
  while compiled code executes (userfaultfd registered missing pages,
  `Memory::software_check_page`): scalar guards read the gate table, which is 0
  while they are on, and their checked path compares with the real bound before the
  page flags; other emitters test the flag. Read either at every access, never hoist.
- Published code/metadata outlive in-flight CPUs. MT publication/invalidation
  changes atomic target cells, not live instruction bytes; preserve compiler
  locking, invalidation generations and target revocation for Tier-1/Tier-2.
- `emit_mt_chain_slot` reads the CPU through its `cpu_reg`: after an exit pops the callee-saved registers
  (ordinary frames, Tier-2 chain-outs) RBX is the caller's, so those pass RDI. Reading RBX there compares
  whatever the C++ caller left in it, which only crashes when the compiler happens to leave it null.
- MT code stays bounded through `g_jit_preempt_epoch` (`Runtime/Preemption.hpp`), polled with loads only by
  backward edges and IBL probes; new native entries must call `jit_grant_lease(cpu)`, and before `bind_ibl`: MT
  probes have no generation check, so a lease taken after the table sync could accept a stale entry. Anything that retires or
  replaces code advances the epoch (`invalidate_dispatch_cache`). Forward static edges carry no poll; ST code and timed
  Tier-2 sampling code keep the 256-edge budget (`preempt_by_epoch()`). MT `BL` calls through a revocable cell.
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
- Fault exits jump to a frame return already emitted in their compile unit
  (`emit_frame_return(true)`); block exits keep their own (a jump on the
  dispatcher-return path cost 10%). Reset `frame_return_off_` wherever a block or
  region compile starts (`rewind_codegen` drops it); never jump to another unit's.
- Tier-1 cold tails (widths up to 8; a 16-byte access keeps its checks in its tail) call one
  copy per JIT of the page checks and jump to one fault exit
  (`emit_cold_stubs`, `use_cold_stubs`). A page-check stub preserves every register and answers
  in ZF, and its failing path must clear ZF. A persistent-cache recording keeps full tails: a
  reference out of the block cannot be reused in another process. Tier-2 keeps inline guards.
- A bare `0x40` REX selects SIL/DIL over DH/BH: `emit_rex_if_needed` is only for operations
  with no byte register.
- Restore the complete ST chain-skip slot through `write_unpatched_slot`;
  its unpatched form is `kSlotNop5`. MT revokes data cells, never instructions.
- Per-block bookkeeping is the JIT's largest heap cost (~700k blocks per Android app;
  `[JIT] metadata MiB:` in `Runtime/MemoryStats.cpp`). Keep `BlockEntry` grouped by
  field size, with 32-bit code offsets. MT keeps no `back_refs_` (its edges are reached
  through `mt_edge_refs_`); an MT slot's cell is found with `mt_slot_edge(source_pc, slot)`.
  Add a container to `dump_memory_stats` when you add per-block state, and to
  `release_resources` (`Runtime/Generations.cpp`) when it is per-block or generated code addresses it.
- A full code cache is replaced as a whole: `Emulator::renew_jit` makes a fresh `FrostJIT` current and
  retires the old one, which the reaper releases only once every thread acked a dispatch boundary with no
  generated frames (`cpu.jit_call_depth == 0` in `run_block`) or is parked in a syscall that C++ runs
  (`Runtime/Generations.hpp` has the protocol). The object outlives its release because generated code
  addresses its members. A helper that generated code calls finds its instance with
  `FrostJIT::running(emu)`, never `emu.jit()`, and a loop that dispatches blocks must follow the current
  instance (`jit_call_helper` retargets). Never block for long inside generated frames: that pins a generation.
- Under chain-skip, Tier-1 exits leave RDI/RSI undefined: every chain target must enter past its
  prologue's `mov rbx, rdi` and emulator store (`chain_entry`), and code on the way (MT chain slot, IBL
  probe, the Tier-2 trial chain wrapper) addresses the CPU through RBX. A new chain target or edge stub
  that needs RDI/RSI must load them itself, as the trial wrapper's cold tail does before entering `fn`.
- Generated code reads the `Emulator*` from `CPU::jit_emu` (`emit_load_emu`); every block and region
  prologue stores it (`emit_store_emu`) and a chain edge keeps it because the CPU is the same. Do not
  put it back in a frame slot: the 32 KiB chain-skip frame made that a page away from the rest of
  each native call's stack.
- The persistent cache (`Runtime/CacheRecording.cpp`) reuses Tier-1 bytes in later processes. An emitter
  that embeds a host address emits it as imm64 and calls `note_host_reloc` right after; a guest PC goes
  through the imm64 form when `wide_guest_pc` says so; compile-time guest reads use `fetch_address`
  (the proof compile shifts PCs). Undeclared host addresses make blocks unrecordable (they are caught,
  not reused), so a missing declaration shows as `host-address` in `[JIT] disk cache unrecordable:`.
  A new per-block side table needs the install path to fill it too (`install_cached`).
- A non-thunk SVC leaves its block with `cpu.jit_svc_pc` set and C++ runs it (`run_pending_svc`, which
  clears the field first because `clone` copies the CPU). Anything else that calls a block's `fn` directly
  and then reads `cpu.pc` must run a pending SVC before executing at that pc. Thunk SVCs (`x8 == 0x1000`)
  stay inline.
