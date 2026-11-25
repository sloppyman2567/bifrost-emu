# FrostJIT

Read [the JIT guide](../../Docs/JIT.md) before implementation, debugging or review.
The [source map](../../Docs/ARCHITECTURE.md) describes subsystem boundaries.
Document a JIT change by updating the guide's section for that mechanism (no dated sections in the guide); put its dated
write-up, measurements and rejected attempts at the top of [the JIT history](../../Docs/archive/JIT-history.md) and list
it under the guide's Recorded changes.

One place per kind of fact, so a later change has one place to correct:

- The guide (`Docs/JIT.md`): how the mechanism works now, with at most one short measurement.
- The history: dated write-up, every number, rejected attempts, what was and was not tested.
- This file: contracts, what must not break and which test covers it. A number only where it is the reason for the rule.
- `Docs/roadmap.md`: open work in full; finished work as one line with the headline number and a link to the history section.
- `Docs/CHANGELOG.md`: 2 to 6 lines for people. The commit message carries the evidence.

A new history section can make an older "not measured" or "next section" line wrong: recheck them.

- `Compiler/BlockAnalysis.*` owns fallback prediction, leaf eligibility and the
  dead-NZCV successor scan (`nzcv_dead_in`);
  `BlockCompiler.cpp` orchestrates Tier-1 compilation; `VerificationStores.cpp`
  records differential replay footprints. Prediction must track actual fallback
  support, but final fallback counts come from optimized IR.
- `Carry/` is the planned register-resident second JIT ([design](../../Docs/JIT.md#planned-codegen-retaining-guest-state-across-regions)).
  Today it holds `LoopReport.cpp` only: `note_carry_block` and `dump_carry_candidates` read sampled hotness, each block's static
  chain targets and `carry_info_`, print the `[carry]` lines of the `BIFROST_PROF`/`BIFROST_PC_HIST` report and emit no code. `carry_info_` is
  written under the compile lock, so the reader takes `compile_mutex_` (try-lock, as `dump_memory_stats` does) and then the table lock.
  `carry_report` covers it.
- A mid-block exit (`BRCOND_ZERO`, `BRCOND_BIT` or `BRCOND` with `aux = 1`, `Compiler/SideExit.cpp`; a `BRCOND` after an `FP_CMP` is never one) leaves through a tail emitted after the block from a captured layout: it
  must store every mapped guest register, not only the dirty ones (a loop pin is statically clean and ahead of memory), and the optimizer's
  branch barriers must stay (dead-store elimination clears at a branch). Its chain slot lives in a stub `BlockEntry` under `side_exit_key`; anything
  that enumerates `blocks_` must tolerate those entries (no guest pc, `fn` null), anything that retires a block must retire its `side_stubs_`, and
  a new place that creates MT edges for a tail sets `edge_source_override_`. Such a block is `has_call` and unrecordable; `jit_side_exit` covers it.
- `fold_ahead_kind_` 4 (a constant stored by the next eager `STORE_REG`) is decided by the pre-scan under the same guards the `STORE_REG` case re-checks
  (not `keep_store_dest_`, GPR/SP destination, imm32 fit): a skipped `mov` whose consumer then stores a register leaves the vreg unmapped. Do not
  defer stores into RAX/RCX/RDX/R8 for a block that is not a loop to save the eager store: it was tried and added instructions (a spill at each clobber).
  Carry formation (`BIFROST_CARRY`) stays off while the persistent cache records, as its blocks are unrecordable.
- Register passing (`BIFROST_CARRY`, `Compiler/RegisterPassing.cpp`; off with `BIFROST_NO_REG_PASS`): a chain edge may enter its target past the loads
  of the guest registers the predecessor already holds. The predecessor's copies are clean arch-vreg mappings (`vreg_home_[a]` clean, equal to
  `cpu.regs[a]`) in `RP_HOSTS` (never RAX, which carries the next pc, or R11, the MT cell jump's scratch); `rp_release` makes one when a temporary
  that still holds a guest register's value dies, and `rp_capture_layout` records them where a slot is emitted (the block's fall-through slot,
  `emit_taken_path_epilogue` for the taken slot and for each deferred mid-block exit, which swaps in the state captured at its branch; the
  emitter that writes `rp_taken_layout_` for an exit tail must not leave it set for the block's own taken slot, `flush_deferred_side_exits` restores it).
  The successor's register entry follows its entry loads (`rp_emit_entry_loads`) and is published in `RegPass` (`reg_pass_`, indexed by
  `BlockEntry::reg_pass`, with the layouts), never in `BlockEntry` itself. A slot is patched to the register entry only through
  `chain_target_for`, which requires the slot's layout to cover the entry layout exactly (guest register and host register), at every patch
  site (`try_chain_block`, `chain_back_references`, `refresh_mt_edges`). A copy must stay equal to memory: anything that writes a guest register's
  memory without going through `STORE_REG`, `store_reg_to_vreg`, `set_vreg_reg` or `alloc_reg_for` on that register, or that may change one behind
  the allocator (calls, SVC, `invalidate_all_vregs`), must call `rp_unbind`/`rp_unbind_all`; a block that is a loop (pins own R12-R15), compiles
  for the persistent cache, runs under `BIFROST_JIT_VERIFY` or starts a native libc routine has no copies and no register entry, and the block
  trace stays in front of an entry that would skip it. Everything after the entry loads (the pc store of a block that calls out, the statistics
  count, the vector loads) runs on both entries. A partial-entry stub (`rp_emit_stubs`, compile time only: patch sites do not emit code) loads
  entry registers from memory and jumps to the register entry, so the entry layout must list every register the block's exit layout may carry
  that the block did not map itself. A register loaded only to be handed on (`BIFROST_REG_PASS_THROUGH=1`) must be one the block never touches
  and a block that calls out gets none, because the call drops every copy. Verifying a change: a temporary compare of each layout entry with `cpu.regs[]` before every
  slot (a `cmp reg, [rbx+disp]; je; ud2` per entry, not kept) over the JIT tests and CoreMark found a layout recorded for the wrong slot.
  `jit_reg_pass` covers it.
- `IR/` owns typed instructions, translation, scalar SSA, optimization and
  validation. Factories/readers are the parameter contract. SSA currently models
  scalar values; do not assume MemorySSA or complete vector/FP state coverage.
- `CodeGen/` owns operation emission; `Backend/` owns host instruction encoding;
  `RegisterAlloc/` owns scalar mappings and spills; `Runtime/` owns dispatch,
  caching, fault recovery and profiling.
- JIT lifetime across fork/exec: a fork child keeps the parent's JIT and translations (a Zygote child
  starts warm). `fork_guest` takes every live instance's locks with its own, all or none
  (`FrostJIT::try_lock_for_fork`), and the child re-creates them and restarts the preemption ticker
  (`reset_after_fork_child`, `Runtime/Fork.cpp`); the private code buffer gives each process its own
  copy. A new JIT lock that a translation, publication or invalidation holds goes into that set. When
  the locks cannot be taken, or per-thread JITs exist, the child runs interpreted as before. `execve`
  gives the new image a fresh JIT (`Emulator::restart_jit_for_exec`, which abandons the old one because
  the caller is still returning through its code buffer). Anything process-wide a JIT starts must be
  re-startable in a fork child. `test_fork_jit`, `test_exec_jit.sh` and `test_exec_mt` cover it.
- JIT switches are `DebugFlags` members (`Source/System/Core/DebugFlags.hpp`), parsed once from the environment with
  the variable's name quoted in the parser: the Makefile lists those quoted names into the disk-cache fingerprint
  (`JitCodegenSwitches.inc`), so a switch must not be read with `getenv` in the JIT or set with `setenv` at run time.
  The exceptions are `PersistentCache` (its settings arrive from `Main`'s options through the environment) and
  `FrostJIT::force_rwx_code_buffers` for shared-JIT mode.
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
  emits. The JIT inlines most
  kinds, so the generated guest fixture rarely reaches a `run<K>` body: the helper
  host test (`make test-native-simd-helpers`, part of `make verify`) runs every
  body against the same QEMU hashes. `vec_inst_operands` lists each kind's
  vector reads and writes by hand; add a new kind there (a kind that accumulates
  into Vd reads it).
- A block that starts a registered guest `memcpy`/`memmove`/`memset` (`NativeLibcRoutines`, `Emulator::native_libc()`) begins
  with `emit_native_libc_entry`, emitted at the chain entry before anything is cached. It leaves by the dynamic
  `CALL_INTERP` exit, so it must stay where no register, flag or vector state is live, and `jit_native_libc` must leave `cpu.pc`
  at the routine's own entry when it declines. Keep it out of verify and slow-store compiles; a block
  compiled with it must not be reusable from the disk cache (the shifted proof compile has to differ, which it does because
  the registry lookup misses at the shifted pc).
- A Tier-1 access may skip its gate compare only when `scan_covered_guards` proves an earlier access of the block used the same
  base value and offset at a width no smaller, with only straight-line scalar ops between and no `BRCOND_SKIP` in the block;
  `guard_checked_` records that the earlier access really emitted its compare. A covered access is still a registered fault site
  with its fault exit (`DeferredGuard::covered` drops only the compare and the page checks); an acquire load is never covered.
  Add an op to the scan's list only if it cannot branch, call, fault or write the base. `jit_guard_stubs` has the pair cases.
- A single-thread self-loop's conditional jump is retargeted past the patchable slot when nothing sits between them
  (`note_selfloop_jcc`, patched in `translate_block`). Anything that rewrites or NOPs the slot must treat the jump too: replay
  in `Dispatch.cpp` uses `BlockEntry::selfloop_jcc_back`, and the disk cache records it (`NativeBody`, format version in
  `PersistentCache.cpp`; bump it when `NativeBody` changes). The field must stay 16 bits so `BlockEntry` keeps its size.
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
  Tier-1 compiles use it (`host_flags_match_pstate()`). NZCV lives in `CPU::nzcv_host` as the
  `lahf; seto al` image (N bit 15, Z bit 14, bit 8 = x86 CF = NOT ARM C, V bit 0):
  `emit_materialize_flags` adds a `cmc` for ADD-convention producers, stores all of EAX (a narrower
  store than a reader's load misses store forwarding) and clobbers RAX only, and
  `emit_load_flags_from_pstate` (`add al, 0x7f; sahf`) clobbers RAX only. A constant NZCV goes
  through `host_from_arm_nzcv`; C++ uses `nzcv()`/`set_nzcv()`, never the raw bits. A BRCOND with
  its flags in memory tests one flag as a bit and reloads RFLAGS for the others.
  `clobber_flags(false, true)` (RDX only, ALU ops and guarded accesses) is for callers
  that pick registers through the allocator afterwards; others assume RAX/RCX/RDX and keep
  the RAX save, since they may hold a value in RAX the allocator does not know about.
- `fold_ahead_kind_` (BlockCompiler.cpp) must mirror its consumer's fold guards exactly: a
  skipped `mov` whose consumer then does not fold leaves the vreg unmapped. Kind 3 is the CSET
  form of CSEL (constants 0 and 1, read through `csel_const`). Pass 0b of
  `optimize_ir` relies on `LOAD_MEM` and the shared 32-bit `ADDS/SUBS` emitter zero-extending.
- Scalar FMAX/FMIN/FMAXNM/FMINNM (`FP_BINOP` 4-7) take the cold step for NaN inputs only. Equal operands are exact
  inline: the operation in both operand orders, ANDed for max and ORed for min (x86 returns the second operand on a tie,
  Arm orders +0 above -0). Do not test the result for zero: `max(x, 0)` is zero for every negative input, and each such
  result was an interpreter step (about 800 host instructions). `jit_fp_scalar_fminmax` covers every special pair.
- Scalar FMUL/FMULX/FMLA/FMLS by element are `FP_BINOP`/`FMADD`/`FMSUB` with a source element
  (`lane`, IR `cond` = element + 1); `fp_decode::scalar_fp_by_element` decides for the translator
  and `BlockAnalysis` alike. The element comes from `fp_load_lane`: under the low-half FP cache an
  upper-half element is read from memory, never from the XMM.
- Vector FMLA/FMLS by element are `SIMD_FP_FMA` with a lane (`fp_decode::vector_fp_fma_by_element`,
  checked before the native SIMD table). Single-register LD1R is `LOAD_MEM` + `SIMD_DUP`.
- `SIMD_STRUCT_MEM` is LD2-LD4/ST2-ST4 (multiple structures) and does not end the block, unlike the `SIMD_NATIVE_STRUCT`
  helper that still serves lane and replicate forms, `.1D` and register lists that wrap past V31. The translator emits the
  post-index base update as separate IR after it, so the op itself never writes a GPR. Its guard covers the whole span with
  software checks and steps the guest instruction on any failure, and `compile_ir_inst` emits it before the vector cache is
  switched off (`struct_guard_emitted_`): the fault exit publishes dirty pinned vectors only while the cache is active.
  `compile_simd_structure_` uses XMM0-2 and XMM12-15 only; `vec_inst_effects` declares that clobber set (0xF007) and the
  registers read or written, so change them together. A post-index `Rm` is any of X0-X30 and only 31
  selects the immediate: the interpreter, the helper and the translator agree, and `jit_neon_struct` checks X30.
- `SMADDL`/`UMADDL`/`SMSUBL`/`UMSUBL` (and `smull`, `umull`, `smnegl`, `umnegl`) are not IR ops: the translator lowers them to `SEXT`/`ZEXT`
  32, `MUL` and `ADD`/`SUB`, so the allocator sees them. Do not bring back a fixed-register emitter for a multiply form. `IRInst::aux`
  carries SIMD element and lane numbers; no op keeps a vreg in it. `jit_maddl` compares against a multiply-free reference: a C
  widening multiply compiles into the instruction under test.
- An `FP_CMP` directly followed by a `BRCOND` (Tier-1 only, any condition but AL/NV) leaves raw `ucomis` flags: the host
  flags are in no convention then, only `fp_raw_flags_live()` (the op index of that BRCOND) says so, and the branch picks one jcc
  and swaps the compare's operands where `fp_raw_flags_cc` says. EQ/NE add a `jp` (unordered sets ZF): EQ hops over its `je`, NE
  takes the branch; `note_selfloop_jcc` records only the ZF jump, the `jp` keeps landing on the patchable slot. Nothing but
  that BRCOND may read them: every exit that needs NZCV converts them with `materialize_fp_raw_flags_to_pstate` before its pack,
  on both paths. A new consumer or an op between the two must not see `fp_raw_flags_live()`; `jit_fcmp_branch` covers every
  condition and exit shape.
- `FP_CMP` (not `FCCMP`) leaves SUB-convention host flags (`flags_in_host_`, `flags_from_sub_`) with no pstate store when its
  consumer, found past constants, moves, register loads and adds, is a conditional branch or an integer select; `fcsel`, `mrs`
  and anything else keep the branchy pstate sequence (jumps on the compare's own PF/CF/ZF, never `pushfq`), which measured faster
  when the outcome is predictable. The choice reads
  `cur_insts_`, the Tier-1 block being compiled (null elsewhere, where the pstate sequence is used); either path is correct.
- `CCMP`/`CCMN` leave their result in the host flags on both paths (`flags_from_sub_` = CCMP) and store nothing. Every
  allocation happens before the condition's jcc so both paths end with one register state; the false path loads the
  immediate through `mov eax; add al, 0x7f; sahf`, saving RAX when it is mapped. Its `#imm` fold and the `fold_ahead_kind_`
  CCMP entry must agree (constant src2 read only by the CCMP, 64-bit needs an imm32 fit). AL and NV always compare.
- A single LD16/ST16 in Tier-1 is `compile_guarded_vec16`. Its 16-byte page checks stay in its own
  tail: the shared cold stubs exist for widths up to 8 (`flush_deferred_guards` checks `g.width`).
- Block growth past the cap is only for loops (`selfloop_extent`, at most twice the cap); its look-ahead
  reads mapped executable pages directly and must stay a hint that adds no code-word dependency.
- `WIN_REG` (R10) holds the window base for a whole chain: the cold prologue loads it once, and a chain entry, an IBL probe hit or an MT cell
  jump enters past that load, so nothing reloads it per block. Every path that calls C++ and then continues in generated code must save
  and restore it around the call (`emit_push(WIN_REG)` / `emit_pop(WIN_REG)`, with `emit_call_aligned` told about the push); a path that
  ends the frame need not. A new call site, or a new entry that skips the cold prologue, keeps that contract. A build with a compare-and-trap
  of R10 at every chain entry (a temporary `cmp r10, imm64; je; ud2` after `chain_entry_off_`, not kept) caught `emit_native_libc_entry`,
  which called its helper without saving R10; `native_libc` and `jit_native_libc` run it.
- Android data TBI (`Memory::data_address`): every native data access untags its final address
  (base + offset) with `emit_untag_data_address` in a scratch copy before the bound and page checks;
  the guest register keeps its tag. Where `WIN_REG` holds the window base (the Tier-1 scalar and 16-byte
  guards, whose bound compare reads the header page through it) the untag is one `and reg, [WIN_REG +
  Memory::JIT_UNTAG_MASK_OFF]`; call it with `window_valid = true` only there. Elsewhere, where the limit
  is an immediate and `WIN_REG` may not be loaded, it stays `shl reg, 8; shr reg, 8`. A new access path that skips it is correct but interprets every
  tagged (Scudo heap) access. `data_tbi()` is part of the disk-cache fingerprint.
- A scalar FP load or store is `LOAD_MEM` + `FMOV_G2F` or `FMOV_F2G` + `STORE_MEM`,
  never `SIMD_LDST`: that op is not cache-compatible and would veto the XMM cache
  for the whole block. Vd is written only after the load, so a fault leaves it intact.
  In Tier-1 a 4- or 8-byte `LOAD_MEM` read only by the next `FMOV_G2F` (`fp_load_`) loads straight into the
  register's cached XMM with `movss`/`movsd` (a registered fault site, slot claimed before the guard captures
  the layout) and the `FMOV_G2F` emits nothing. A scalar result written to a cached XMM leaves every bit above it
  zero (`xorps; movss` or `movq`), so the vector cache's `fp_zero_hi` is free that op (`fp_hi_zeroed_vreg_`, reset
  per op). Keep `v_hi` zero stores as `pxor; movsd`: an immediate `mov qword` made scalar loops ~30% slower.
- Vn is `CPU::v[n]`, 16 contiguous bytes (`lo` bits 63:0, then `hi`), addressed as `vlo_off(n)`/`vhi_off(n)`; byte `b` of
  Vn is `vlo_off(n) + b`. Move a whole register with one 16-byte access (`load_v16`/`store_v16`), never a lo/hi pair, but the
  lo-only scalar FP cache stores 8 bytes: its XMM does not hold `v_hi`.
- Saturating narrowing shifts (`SIMD_SHRN_SAT`) add the rounding bit after the shift, `(x >> s) + ((x >> (s-1)) & 1)`, never `x + 2^(s-1)`
  in the source lane (that wraps). The translator takes the shift from `immh:immb` (source bits minus the field) and the
  source lane size from `immh` (twice the destination's); 64-bit sources stay interpreted in the emitter and in
  `BlockAnalysis` alike. `jit_shrn_sat` (matches QEMU) covers every subop, shift and half.
- Native saturating integer SIMD must set FPSR.QC like the interpreter
  (`emit_simd_qc_unless_equal`); differential verification does not compare
  FPSR, so check it against QEMU.
- Translation cost counts: a pass that costs more translation time than the code it saves is not worth running on every block
  (`[JIT] compile:` in `-v`; the Tier-1 SSA adapter was dropped for that, `BIFROST_TIER1_SSA=1` brings it back). Per-block state
  that only a debug mode reads is built only in that mode: `record_verification_stores` runs under `BIFROST_JIT_VERIFY` alone, so
  anything new that replay needs from a block must be recorded there too. Clearing a 4096-entry table per block is a cost, and so is a hash
  node per vreg: `optimize_ir` keeps its per-block state in thread-local `EpochTable`s (O(1) clear, no allocation), so a new per-block map
  keyed by vreg or ARM register belongs there, not in an `unordered_map`.
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
  integer or scalar FP data-processing instructions (`fcsel` and `fccmp` read the flags and end the scan as live;
  FP loads and stores are other classes and also end it). A successor that merely does not read the flags proves nothing.
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
  only for exceptional values. It commits, single-steps and reloads, and an op that
  uses it writes Vd only after its guard. Tier-1 emits it after the block
  with the layout captured at `emit_fp_cold_tail` (as for vector guards), so the hot
  path falls through; at most two branch sites share one step.
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
  locking, invalidation generations and target revocation.
- `emit_mt_chain_slot` reads the CPU through its `cpu_reg`: after an exit pops the callee-saved registers
  (ordinary frames) RBX is the caller's, so those pass RDI. Reading RBX there compares
  whatever the C++ caller left in it, which only crashes when the compiler happens to leave it null.
- `cpu.jit_stop_pc` is a callback boundary that may name real guest code (`jit_mt_publication` sets it to a loop's own head and expects one
  iteration), so a published MT self-loop must honor it. It cannot change while the loop runs (only this thread sets it), so the loop
  compares it once per entry (`emit_selfloop_stop_entry_check`, before `block_body_start_off_`); if it names the head, the entry moves
  `cpu.jit_epoch_seen` 2^30 away from the epoch, so the first back edge's poll leaves, after one iteration as before. A block whose
  prologue ran before the MT decision (`selfloop_stop_hoisted_` false) compares in the back edge instead. For a pc above imm32 the
  compare covers the low 32 bits only (a false hit leaves early, never the reverse); do not load the pc into a register for it, and
  do not drop the check for targets that look unreachable.
- MT code stays bounded through `g_jit_preempt_epoch` (`Runtime/Preemption.hpp`), polled with loads only by
  backward edges and IBL probes; new native entries must call `jit_grant_lease(cpu)`, and before `bind_ibl`: MT
  probes have no generation check, so a lease taken after the table sync could accept a stale entry. Anything that retires or
  replaces code advances the epoch (`invalidate_dispatch_cache`), which also makes every thread drop all of its dispatch caches and IBL.
  Forward static edges carry no poll; ST code keeps the 256-edge budget. MT `BL` calls through a revocable cell.
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
  W-form fusion (`scan_w32_and_sext_loads`) must scope its
  encoding width to the ALU operation, preserving operand setup and lazy NZCV.
- Fault exits jump to a frame return already emitted in their compile unit
  (`emit_frame_return(true)`); block exits keep their own (a jump on the
  dispatcher-return path cost 10%). Reset `frame_return_off_` wherever a block compile starts (`rewind_codegen` drops it); never jump to another unit's.
- Tier-1 cold tails (widths up to 8; a 16-byte access keeps its checks in its tail) call one
  copy per JIT of the page checks and jump to one fault exit
  (`emit_cold_stubs`, `use_cold_stubs`). A page-check stub preserves every register and answers
  in ZF, and its failing path must clear ZF. A persistent-cache recording keeps full tails: a
  reference out of the block cannot be reused in another process.
- `emit_call_aligned` with `preserve_flags` pads an odd push count with `lea rsp, [rsp-8]`, never `sub`/`add`: those
  write RFLAGS before `pushfq` saves them. The slow paths of `emit_load_mem`/`emit_store_mem` take any source register
  but R8/R9 (the store reloads its value from its own stack copy, since RDX and RCX are argument registers) and
  sign-extend exactly as the fast path does (4 bytes to 64 bits, 1 and 2 bytes to 32). `jit_emitter_slow_paths` runs them.
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
  probe) addresses the CPU through RBX. A new chain target or edge stub
  that needs RDI/RSI must load them itself.
- Generated code reads the `Emulator*` from `CPU::jit_emu` (`emit_load_emu`); every block
  prologue stores it (`emit_store_emu`) and a chain edge keeps it because the CPU is the same. Do not
  put it back in a frame slot: the 32 KiB chain-skip frame made that a page away from the rest of
  each native call's stack.
- The persistent cache (`Runtime/CacheRecording.cpp`) reuses Tier-1 bytes in later processes. An emitter
  that embeds a host address emits it as imm64 and calls `note_host_reloc` right after; a guest PC goes
  through the imm64 form when `wide_guest_pc` says so; compile-time guest reads use `fetch_address`
  (the proof compile shifts PCs). Undeclared host addresses make blocks unrecordable (they are caught,
  not reused), so a missing declaration shows as `host-address` in `[JIT] disk cache unrecordable:`.
  A new per-block side table needs the install path to fill it too (`install_cached`).
- `cpu.pc` is not maintained across a patched chain edge (`defer_pc_store`: every chain-skip layout). A static exit
  stores it after its slot, so only the paths that leave generated code write it, and inside a chained block it holds an
  older block's PC. Code that needs it current must not assume the block start: an exit or helper call site sets it
  itself, and a block that calls out of generated code publishes a chain entry that stores its start PC first
  (`chain_entry_pc_off_`). `emit_call_abs` and `emit_call_rel32_placeholder` set `block_calls_out_`; a new way of
  calling out (a raw `call`, a tail jump to C++) must set it too. The op list at the chain entry only chooses between the
  inline store and the stub after the block. Anything emitted between an exit's `mov eax, pc` and its frame return must
  keep RAX: it is the block function's return value. A new chain target that reads `cpu.pc` stores its own PC first.
- A pinned register's update runs in its pin only for the exact adjacent shape `scan_pin_inplace` accepts (OP, an optional fused
  ZEXT 32, STORE_REG to the same register, the LOAD_REG's value not read after the OP). The compile loop compiles a patched
  `IRInst` with `dest = src1 = a`, so the emitters' `dest == src1` forms must keep the result in `src1`'s register: the `ADD`
  path's dead-`src2` reuse is skipped for `dest == src1`. Widen the shape (an op between, a later write) only with the fault and
  exit argument: the pin must not change before the end of the guest instruction. Never alias a vreg to a pin's register: the
  allocator keeps `reg_vreg_[vreg_home_[v]] == v`. `jit_pin_inplace` covers it; `BIFROST_NO_PIN_INPLACE=1` turns it off.
- No self-loop back-edge (`BRCOND` through `flush_all_vregs_but_pins`, `BRCOND_ZERO`, `BRCOND_BIT`) stores the loop pins
  (R12-R15), so inside the body a pin can be ahead of `cpu.regs[]` although it is mapped clean. The body must read a pin
  through its mapping: the preload raises `max_vreg_`, because `load_vreg_to_reg` and `store_reg_to_vreg` trust
  `vreg_home_[v]` only for `v <= max_vreg_`. Exits taken before a pin's rewrite store every mapped architectural
  register, not only dirty ones (`emit_memory_fault_exit`, `emit_fp_interp_step`); keep that for any new mid-block exit.
  `jit_selfloop_pins` and `jit_fp_ldst_cache` cover both.
- `fp_load_`, `lazy_load_` and `scaled_add_` are Tier-1 pre-scan requests; `zext32_fused_` and `sext_load_` come from `scan_w32_and_sext_loads`. The guarded load reports
  what it emitted in `sext_load_done_` (other load paths zero-extend, so the `SEXT` must not trust the request). A lazy
  `LOAD_REG` and an unshifted `SHL` result exist for one op only: `ensure_vreg` and the compile loop materialize a lazy
  load for any reader but the `SUBS` it was left for, and the loop and the ALU emitter materialize the shift for anything
  but the `ADD` that scales it. A new emitter for `ADD` or `SUBS`, or one that reads a source through `vreg_home_`
  directly, must call `materialize_scaled()` / `materialize_lazy_load()` first.
- A non-thunk SVC leaves its block with `cpu.jit_svc_pc` set and C++ runs it (`run_pending_svc`, which
  clears the field first because `clone` copies the CPU). Anything else that calls a block's `fn` directly
  and then reads `cpu.pc` must run a pending SVC before executing at that pc. Thunk SVCs (`x8 == 0x1000`)
  stay inline.
