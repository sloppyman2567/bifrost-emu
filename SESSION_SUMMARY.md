# bifrost-emu — Session Handoff (Aug 2026)

## Objective
Get `ctest_real/minecraft_weekend` (AArch64 Minecraft-ish game) running fast via
JIT. Four commits landed in the previous session: `e69eff0` (native FCVT rounding
+ de-interp of FP blocks), `d27d396` (native SIMD-scalar FP-source int↔FP converts
+ fbits sentinel fix), the MODIMM commit (native AdvSIMD modified-immediate
MOVI/MVNI/ORR/BIC), and the scalar-shift commit (native `ushr/sshr/shl dN,dM,#imm`).
Full suite **193/193 PASS default** (~50s). Game runs from its own dir
(`ctest_real/minecraft_weekend/`, `../../bifrost-emu`); running from repo root
fails on shader load. Seeds from `NOW()` — runs are noisy, use
`BIFROST_STATS_PERIOD` + long runs. Terrain confirmed CORRECT (varied heights
incl. below-sea-level chunks). Game wall time ~30.5s → **~22.3s clean**.

## What was just done THIS session (all committed)
The minecraft game's "normal rc=133" exit was diagnosed and FIXED as a
heap-corruption crash, and the guest heap now stays inside the 4 GiB direct
window. Committed as three commits: `9d7bcd8` (allocator fix),
`ba795da` (periodic prof reporter), `34d6f09` (vec-cache ST16/LD16 + broadcast).
Root cause + fixes:

## What was just done THIS session (committed as `perf(jit)` — sandwich elimination)
The hot `chunkmesh_mesh` spill/reload bloat (Next-step #1 from the prior
session) was attacked directly. The 662-byte hot block 0x405304 was full of
`mov reg,slot; mov slot,reg` "sandwiches" — UBFM/SBFM/LOAD_MEM/STORE_MEM
unconditionally did `flush_dirty_host_regs(mask)` + `flush_scratch_host_regs(mask)`
+ `invalidate_host_regs(mask)` + `load_vreg_to_reg(dst, src)` whenever a
preceding LOAD_REG/ADD/SHL had left the source vreg dirty in RAX/RCX.

- New `load_vreg_to_reg_fast()` (x86_regalloc.cpp) + `vreg_fast_keep_candidate()`
  predicate + `cur_op_index_` member (set in translate_block's compile loop),
  gated on the EXACT per-op liveness in `kills_per_op_[cur_op_index_]`:
  - Tier 1 (always safe): src cached in dst → flush+invalidate still happen
    (value preserved for later readers) but skip the redundant reload.
  - Tier 1.5: src cached in ANOTHER clobbered reg → copy to dst first, then
    flush (kills the ADD/SHL→LOAD_MEM sandwich).
  - Tier 2 (dead scratch, v>32, last use = current op, v != inst.dest):
    skip the flush of dst entirely, keep it mapped dirty for the op.
- LOAD_MEM now keeps dest cached (`set_vreg_reg(dest, RAX)`) instead of
  `store_reg_to_vreg` — the loaded value is consumed immediately, and the old
  mapping-kill forced a reload sandwich.
- STORE_MEM decides BOTH operands via `vreg_fast_keep_candidate` BEFORE
  flushing (so src2's cache isn't wiped by src1's flush) and relies on
  `emit_store_mem` preserving RAX/RCX.
- Dead-check MUST use `kills_per_op_` liveness, NOT a FWD env gate: the
  CopyMap copy-substitution in optimize_ir can make scratch vregs multi-read
  even with FWD off. FWD stays disabled by default.
- Result: block 0x405304 = 662 → 590 bytes. Game ~49.5 → ~51.5 MIPS avg (noisy,
  NOW() seeds). **198/198 PASS (52s)**, opgen-check/opgen-thunk-check green,
  BIFROST_JIT_VERIFY divergence count unchanged (7 pre-existing false-positives),
  BIFROST_REGALLOC_CHECK + VERIFY_MEM clean on the game.

- **Root cause (end-to-end confirmed):** `Memory::mmap_alloc` was a pure bump
  allocator (`mmap_next_`, starts at `MMAP_BASE_MIN=0x10000000`, only grows).
  `munmap`/`untrack_allocation` only erased from `allocations_` — no `pages_`
  entries freed, no `total_pages_` decrement, no address reuse. The game
  mmap/munmaps 18 MB DATA + 2.3 MB INDICES mesh buffers every frame (mallocng
  huge path, game 0x358dc), so the bump pointer marched to ~8.5 GB
  (trace: `0x1fc51b000`) → above the direct window → pages went into the
  `pages_` map → hit `MAX_TOTAL_PAGES` (1M pages = 4 GiB, memory.h:53) →
  `mmap_alloc` returned **0**. musl only treats -1/MAP_FAILED as failure
  (guest mmap wrapper 0x36984; `__syscall_ret` 0x3b520), so the guest accepted
  address 0 → mallocng built its arena at guest 0 → garbage pointers → the
  next `free()` (game 0x341f0) BRK #1000'd in `get_meta` (game 0x34600).
  Misdiagnosed for weeks as "normal rc=133".
- **Fix A — mmap syscall returns `-ENOMEM` on failure (was 0):** all three
  paths in `src/syscalls/mem.cpp`: main mmap with `effective_hint`,
  mremap-as-mmap (`old_addr==0 && old_size==0`), and the previously
  `(void)mapped`-ignored `MAP_FIXED_NOREPLACE` path.
- **Fix B — address-space reuse + page reclamation (`src/core/memory.h/.cpp`):**
  `untrack_allocation` signature changed to `(addr, size)`; it now frees
  `pages_` entries (decrementing `total_pages_`; window addresses skipped),
  and adds the range to a new `free_ranges_` (`std::map`, coalesced via
  `add_free_range`). `mmap_alloc` first-fits a reclaimed range before bumping
  `mmap_next_`, zeroes reused window pages (`memset`) and reused `pages_`
  entries (`std::fill`) for MAP_ANONYMOUS semantics, counts ONLY actually-new
  pages in the OOM check (naive `aligned_size/PAGE` spuriously trips the cap on
  reused window ranges), and carves MAP_FIXED overlap via `remove_free_range`.
  `mremap_grow` in-place bumps `mmap_next_` + removes the extra region; the
  collision-move path calls `untrack_allocation(old_addr, old_aligned)`.
  `clone_for_fork` copies `free_ranges_` (it is mutated under `mu_`).
  `src/syscalls/threads.cpp` exec-restart path now passes size.

## What was just done THIS session — JIT vec-cache ST16/LD16 + broadcast (`34d6f09`)
- SIMD_ST16/SIMD_LD16 are now vec-cache compatible (`vec_cache_compatible_op` +
  operand tracking), so blocks containing them can pin the source/dest vector
  in an XMM reg. The guest memset loop's loop-invariant `q0` stays resident:
  the per-store `movsd/movhpd [rbx+…]` reload collapses to one
  `movupd [mem],xmmN` (pinned XMM, REX for XMM8-15). LD16 loads into the pinned
  XMM and marks it dirty. Memset pc-hist share 16.6% → **15.1%**.
- **CRITICAL slow-path trap (fixed):** the ST16/LD16 slow path must use
  `vec_cache_writeback_all(false)` + inline `vec_emit_load_lo_hi` reloads that
  do NOT clear `vec_dirty_` at codegen time. The slow-path code is SKIPPED at
  runtime on the fast path, so clearing the flags there suppressed the
  later-emitted epilogue writeback for vectors dirtied earlier in the block →
  lost values → infinite loop in softfloat `__multf3`/`__eqtf2` blocks (game
  stalled at 0 MIPS after 1 s; `jit_block_split`/`jit_fp_scalar` hung). Same
  invariant as `emit_taken_path_epilogue()`.
- **Fixed a latent VEX typo:** SIMD_ORRIMM's "zero upper" emitted `vmovq` with
  pp=3 (F2, `(bad)` → host SIGILL) instead of pp=2 (F3). Exposed once ORRIMM
  blocks can actually pin a dest (test_movi_imm crashed).
- `stp q0,q0` (memset zero-fill) merges into ONE SIMD_ST16 with `nregs=2`
  (flags_op) + `cond=1` broadcast: a single bounds check for the whole 32-byte
  span, src2 kept constant across halves (codegen + `vec_cache_may_enable` both
  respect broadcast). Memset loop block 789 → **689 bytes** of x86.
- Verified: full suite **198/198 pass**; game no longer stalls; runs 13-40 MIPS
  continuous with no interp in steady state.

## What was committed in the previous session (kept verbatim)
- **`perf(jit): native scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd,Dn,#imm)`**
  The scalar 64-bit shifts have bits[28:24]=11111 → the decoder routes them to
  FP_SCALAR (NOT simd_dp; the SHIFT table mask 0xBF00FC00 pins bit28=0). They
  CALL_INTERP'd every execution — `ushr dN,dM,#32` (0x7F600401/02/03/20) was
  ~20K/500K-window, THE #1 remaining `[fp/simd]` fallback. Now native in the
  FP_SCALAR translator: SHL 0x5F005400 / USHR 0x7F000400 / SSHR 0x5F000400
  (mask 0xFF00FC00) reuse SIMD_SHL/USHR/SSHR with esize=8, q=0 (v_lo shifted,
  v_hi zeroed). shift==64 → CALL_INTERP (SSE2 imm-shift masks count to 6 bits).
  `instr_will_call_interp` mirrors under fp_gate bit 0x100.
  - **Fixed a latent JIT SIGILL: esize=8 SSHR emitted PSRAQ.** `66 0F 73 /4 ib`
    and VPSRAQ are AVX-512F only (NOT SSE2/AVX2) → `sshr dN,#imm`/`sshr vN.2d`
    crashed the host. Shift codegen now CALL_INTERPs esize=8 SSHR alongside
    SSRA/SRSRA. (AGENTS.md previously CLAIMED 64-bit SSHR was native — wrong.)
  - Interp scalar SHL `#64` UB guard (`v << 64`) → clears lane like USHR.
  - Added `scalar_shl_ushr_sshr` to `ctest/jit_neon.c` (`.inst`-pinned D-form
    shifts @ #1/#32/#64 incl. interp-fallback edges). Passes JIT AND interp.
- **`perf(jit): native AdvSIMD modified-immediate MOVI/MVNI/ORR/BIC`** (previous).
  New `MODIMM` simd_dp row (mask 0x9F800C00,
  base 0x0F000400, guard `bits[22:19]==0`) placed BEFORE the SHIFT rows so
  32-bit `movi vN.2s` (immh==0) is no longer swallowed by the esize==1 SSHR
  path (~500K interp execs). New IR ops `SIMD_MOVI` (imm = pre-expanded 64-bit
  lane pattern, flags_op=Q) and `SIMD_ORRIMM` (read-modify-write dest, cond =
  ORR/BIC, flags_op=Q). JIT: `movabs`+`vmovq` (+`vmovddup` Q=1) fast path,
  `vpor`/`vpandn` with pattern in scratch XMM0 for ORR/BIC, GPR or/and+not on
  the memory path; both ops vec-cache compatible + pre-scan dest-as-used.
  **Fixed a latent interp+JIT bug:** the SHRN exclusion clause
  (`bits[15:10] != 0x21`) in the modified-immediate guard rejected cmode=8
  MOVI (16-bit LSL #0, whose bits[15:10] also = 0x21) → `movi vN.4h,#imm`
  silently returned 0 in BOTH modes (0x0F058560). Every valid SHRN has
  immh(bits[22:19]) >= 1, so the `immh==0` guard alone separates it. Added
  `ctest_real/test_movi_imm.c` (`.inst`-pinned, MOVI/MVNI LSL + 8-bit + 64-bit
  + MSL + ORR/BIC read-modify-write; passes JIT AND interp).
- Prior sessions' commits (all landed): `98f43a1` docs dispatch lessons,
  `1526c1e` dispatch ~21% → 10% (thread-local watchdog, 256-slot inline cache,
  lazy WIN_REG, taken-path chaining), `04f2add` hot-interp promotion opt-in,
  `7641200`/`753dd0e` docs, `5128857` HiDPI, `42b52c9` DisplayThunk x9 +
  DisplayProxy-first, `9c33475` brk, `8d52a90` heap/stack window, `fff9db1`
  pairwise max/min sizes 0-3, `d31a6e1` SIMD&FP LDR/STR/LDP/STP, `cb0c51e`
  decoder is_fp_1source.

- **Verification:** instrumented run (MALLOC-LOW/FREE-CORRUPT probes) ran 300 s,
  world.ticks=17988, **0 MALLOC-LOW / 0 FREE-CORRUPT** — no crash past the
  former crash point (~frame 146 / gen 368). Clean headless run:
  **38,154 renders / ~6,219 ticks, EXIT=0**, FPS ramped ~7→**750-790** (heap
  back in the direct window → JIT fast path instead of the `pages_`+rwlock
  slow path ~9×). `DISPLAY=:0` 60 s: EXIT=124 (timeout, no crash). All game
  instrumentation reverted; game sources clean vs HEAD. `make check-all`:
  **198 pass / 0 fail / 0 skip** (51 s) — allocator changes cause no
  regressions.

## This session (Aug 2026): chunkmesh_mesh profiling + FWD bugfix (`a5e85c4`)
- Started profiling **chunkmesh_mesh** (53% of game samples, biggest hotspot):
  dumped JIT blocks (`BIFROST_JIT_DUMP=1`) for the hot 0x405304 neighbor-lookup
  (11 ARM instrs → 50 IR ops → 662 bytes x86, ~15 host instrs/guest instr) and
  0x405028 loop head (178 bytes for 1 instr `ldp w0,w2,[x26]`). Waste = every
  ALU op round-trips `cpu.regs[] → host stack → back` (LOAD_REG/STORE_REG
  pairs), plus per-LOAD_MEM bounds-check fast/slow stubs.
- Root cause of the round-trips: `arm_reg_cache` load-forwarding
  (`BIFROST_ENABLE_FWD=1`) is **disabled by default** (original "subtle
  correctness bug", commit 1257f7b). The original regalloc clobber was already
  fixed (jit_helpers.cpp), but testing FWD exposed a **second latent bug**:
  `SIMD_UMOV` writes an ARM reg vreg directly (jit_codegen_simd.cpp
  `set_vreg_reg(inst.dest, d)`), bypassing STORE_REG — mirroring `FP_F2I` — yet
  `optimize_ir` only taught `arm_reg_cache` about FP_F2I/FP_F2I_FIXED. Under
  FWD a later LOAD_REG substituted a stale cached vreg → `jit_neon` umov tests
  failed. Fixed by adding the SIMD_UMOV case (ir_optimize.cpp, mirror FP_F2I).
- Verified **198/198 pass with FWD=1**; FWD shrinks the 0x5304 block 662→633
  bytes (IR 50→34 ops, dce_removed=22) but game MIPS gain is ~44→46 (noise).
- **FWD stays OFF by default** (env var only). The remaining chunkmesh_mesh
  cost is regalloc spill/reload bloat (12 rbp spill pairs in 0x5304), not
  round-trips — that is the next optimization target.

## This session (Aug 2026): native GLFW cursor callbacks (minecraft mouse-look)
- **Problem:** the game's `glfwSetCursorPosCallback` was `STUB`'d, so the
  guest AArch64 `_cursor_callback` never fired → mouse-look dead (no camera
  rotation; delta stayed 0).
- **Fix (table-driven):** `tools/opgen/thunk_dp.txt` — `glfwPollEvents`/
  `glfwWaitEvents` got policy `GLFW_POLL`, `glfwSetCursorPosCallback` got args
  `ii` + policy `CURSOR_CB` (both added to `VALID_POLICY` in thunkgen.py;
  regenerated `include/opgen_thunk.hpp`, Policy enum now GLFW_POLL=9 /
  STUB=10 / CURSOR_CB=11).
- **Store (thunk.cpp dispatch):** the CURSOR_CB intercept (before the generic
  host-fn path) stores `cpu.regs[1]` (guest cb) in `impl_->glfw_cursor_cbs_`
  keyed by `cpu.regs[0]` (window handle — round-trips as the host
  `GLFWwindow*` opaque value). Never forwards to host.
- **Deliver:** GLFW_POLL dispatch calls `deliver_glfw_cursor_callbacks_(cpu)`
  after the host poll; reads host cursor pos via the dlsym-resolved
  `glfwGetCursorPos`, fires only when position changed since last poll (first
  poll seeds `glfw_cursor_last_` — GLFW motion semantics, no spurious startup
  delta), then invokes the guest via the borrow-CPU runner.
- **Runner (`Emulator::wire_thunk_cursor_cb_runner_`, emulator.cpp):** same
  borrow-CPU pattern as `guest_call_args_` (dlopen 0x1002 → proven reentrant
  from the syscall path): save/restore ALL CPU state, x0=window,
  d0/d1 (v_lo[0]/v_lo[1]) = x/y as double bits, pc=cb, LR=SENTINEL_LR(0x1000),
  thread-local scratch stack, loop `step(cpu)` until LR, restore. Wired right
  after `thunk->init(mem_)` on BOTH the dynamic-linker (~236) and static-ELF
  (~710) paths. New `CursorCbRunner` typedef in `include/frost/thunk.hpp`.
- **Verified:** full suite **198/198 PASS** (52s); opgen guards green. Game
  (run from `ctest_real/minecraft_weekend/`, `DISPLAY=:0`) with
  `BIFROST_THUNK_TRACE=1`: `[thunk] cursor cb: window=… cb=0x41f8a0` on
  registration, then `[thunk] cursor cb → 0x41f8a0 (x, y)` firing on real
  cursor motion with live-varying coordinates (mouse-look works, no crash).
  XTest-synthetic motion can't move GLFW under `GLFW_CURSOR_DISABLED` (raw
  XI2 events) — verified delivery with a temporary force-fire override that
  was removed before commit.
- **Follow-up (same session):** the ad-hoc `getenv("BIFROST_THUNK_TRACE")`
  calls in the thunk dispatch/delivery hot paths (and pre-existing ones in
  display_thunk/audio_thunk) were migrated to the cached `dbg().thunk_trace`
  flag in `include/debug_flags.h` (per AGENTS.md: toggles live there, NOT as
  getenv in hot paths). `BIFROST_TRACE=1` blanket mode now also enables it.
  Full suite still 198/198; game cursor-cb trace verified via the cached
  flag.

## Current game performance (as of this session)
- In a loaded chunk under `DISPLAY=:0`: **45-52 FPS** on the game's own frame
  counter with TPS pinned at 60 (one 6 FPS dip during a world-gen/mesh spike).
  Game's FPS log is not a reliable frame-rate oracle, but ~45-52 is real.
  30+ FPS in a loaded chunk is good for a voxel game under a JIT emulator with
  GL thunks. No crash, stable.
- Headless (no display/GL): 750-790 "FPS" — that number is the unthrottled
  render loop, NOT comparable to the displayed 45-52.

## Profiling state (the key numbers)
- SIGPROF (`BIFROST_PROF=1`): jit ≈ 75%, interp ≈ 7%, dispatch ≈ 10%,
  translate ≈ 0.6%, other ≈ 1% (from prior session). Post-ST16/LD16:
  interp = 0.0% in steady state; function-level pc-hist: chunkmesh_mesh 53%,
  blockmesh_face 17%, memset 15%, grad3 5%, rest softfloat/other.
- Game wall time: ~30.5s → **~22.3s clean** after the scalar-shift native
  work. `bench_mips` tight ALU self-loop = **660 MIPS**.
- Post-scalar-shift fallback words (classprof @500K in JIT mode):
  `7f600401/02/03/20` (`ushr dN,dM,#32`) GONE. Remaining `[fp/simd]`:
  `4e010c20` = `dup vN.16b, wN` (~4K), `4e083c01` = `mov xN, vN.d[0]` (~0.3K).
- NOTE: `dup vN.16b, wN` + `mov xN, vN.d[0]` (UMOV) were since made native
  (see AGENTS.md — SIMD DUP GPR-source broadcast + UMOV lane-extract), so
  those remaining classprof fallbacks from the prior session are obsolete.

## Next steps (suggested)
1. **chunkmesh_mesh regalloc spill/reload bloat** — DONE this session: the
   flush→reload sandwich elimination (`load_vreg_to_reg_fast`, gated on
   `kills_per_op_` liveness) shrank the hot 0x405304 block 662 → 590 bytes.
   Remaining cost is legitimate stack traffic (genuine spilling of live
   vregs), NOT redundant flush/reload pairs — a fuller host-reg allocation
   pass would need cross-block allocation, which the block-at-a-time JIT
   doesn't do.
2. If game perf in-chunk is still desired: profile with `BIFROST_PROF=1`
   (`BIFROST_CLASS_PROF=1` shows which classes run through the interp
   fallback). GL thunk marshalling (SDL2/GL swap, mesh upload) is the likely
   remaining cost at 45-52 FPS, not CPU. memset (15%) is already broadcast +
   pinned, further gains need bigger vector stores (e.g. 4-reg LD1/ST1) or
   host AVX-512.
3. Do not regress fast dispatch paths (bare call/ret, no atomics, no per-PC
   watchdog). Hot-interp promotion stays opt-in (BIFROST_HOT_INTERP).
4. FWD (`BIFROST_ENABLE_FWD=1`) is now correct (198/198) but stays OFF by
   default; it's only a ~4% win on chunkmesh. Re-evaluate if more ops are
   made FWD-compatible.
5. `make check-all` before any further commit.
6. If the game's other GLFW callback setters (`glfwSetKeyCallback`,
   `glfwSetMouseButtonCallback`, `glfwSetFramebufferSizeCallback`, etc.)
   are ever needed by a guest, mirror the CURSOR_CB pattern (store the
   guest cb in a map, deliver after `GLFW_POLL`) rather than leaving them
   STUB.

## This session (2026-08-17/18): cross-block flag-skip + BL_CALL + teeworlds

1. **Cross-block BRCOND flag-materialize skip** (the CoreMark win, all
   committed 2026-08-18): on a BRCOND edge whose target never reads pstate
   before its first flag write (`reads_pstate_before_set` from the existing
   pre-scan), skip the ~27-instruction pstate materialize. Fall-through:
   skip at compile time if the target is already translated clean, else
   SkipAndRecord; taken: SkipAndRecord + `chain_back_references`
   retroactively patches the recorded region to a 5-byte `jmp rel32` when
   the target translates. CoreMark 1687→2210 plain (+31%),
   1836→2541 FWD+CHAIN_SKIP (+38%), CRCs validated. Gate: BIFROST_NO_FLAGSKIP.
   The shared epilogue's `clobber_flags()` is a NO-OP for BRCOND blocks, so
   the materialize calls on both edges are the ONLY pstate writes — the skip
   invariant is safe. JIT_VERIFY's pstate compare is gated on
   `reads_pstate_before_set`.
2. **The two `ir_optimize.cpp` constant folds are BROKEN and DROPPED**: the
   commutative src1→src2 swap + ZEXT-after-LOAD_MEM→MOV fold HANG CoreMark
   under FWD=1 (99% CPU spin; bifrost-emu ignores SIGTERM — kill with
   `timeout -s KILL`/`pkill -x -9`). Only worth +0.4%. `ir_optimize.cpp`
   reverted pristine. The SBFM/UBFM constant-fold FIX (c2d1f2a) is
   unrelated and stays.
3. **Direct-BL-call callee-completion guard fixed (2026-08-17)**: the naive
   `cpu.pc == x30` test is fooled when a mid-callee block ends at its final
   BL (`MAX_BL_CALL_PER_BLOCK=2` sets `chain_target_pc_ = bl_pc + 4` == x30).
   Must be `cpu.pc == x30 && cpu.pc == bl_pc + 4`. Layout requires a `jmp`
   over the INCOMPLETE block on the resume path. The temporary
   `dbg_guard_entry/dbg_guard_incomplete` probes were removed (they corrupted
   RBX); `[DBG3]` tracing is gated behind `BIFROST_DBG_GUARD=1`.
4. **jit_call_helper 10M watchdog cap removed**: window_loop's helper
   legitimately dispatches >10M blocks in ~40s of gameplay; the old cap
   returned a garbage pc mid-game. Same thread-local watchdog as run_block.
5. **teeworlds malloc-spin root cause = STALE BINARY**, not an emulator bug:
   the installed bifrost-emu had an inverted UBFM/SBFM/BFM interp guard
   throwing DecodeError on every sf=1 bitfield op → __libc_early_init aborted
   → ptmalloc_init never self-linked main_arena → _int_malloc's stash loop
   spun on bin->bk==NULL. Plain `make` rebuild fixed it. Do NOT re-diagnose
   memory-model corruption for this game.
6. **teeworlds boots to the menu**: map/skins/fonts load, "No joysticks
   found", audio gracefully disabled, stable 45s+ frame loop, zero
   SIGSEGV/DecodeError under DISPLAY=:0. Remaining `incorrect data check` /
   `invalid distance too far back` lines are the datafile loader tolerating
   resource quirks. Thunk gap work for it: SDL rows (audio trio,
   clipboard, display modes, joystick, `SDL_GetVersion`, window, etc.),
   new policies `SDL_FREE`/`SDL_OPEN_AUDIO`/`JOY_GUID`/`JOY_GUID_STR`,
   `glTexImage3D` TEX3D SizeKind + Fn10/Fn11/Fn12 call ladder, GL alpha.
   Marshalling bugs fixed while booting: `SDL_GetDisplayBounds` `ii`→`ip`,
   `SDL_GetKeyboardState` `i`→`p`, `SDL_GetRelativeMouseState` `-`→`pp`.
7. **SIMD DUP(element,vector) interp bug**: `dup v23.2s,v1.s[1]`
   (imm5=12, index 1) threw DecodeError — the 0x0E000400 handler only
   matched imm5 ∈ {1,2,4,8}; replaced with the ctz-based decode from INS.
8. **Suite status**: 205/205 (JIT full), 200/200 (JIT quick), 204/205
   (interp — `jit_int_fp_conv` fails `fcvtzu_x_d(1e19)`: PRE-EXISTING interp
   FCVTZU ≥2^63 bug, raw `static_cast<uint64_t>` lowers to cvttsd2si
   sentinel 0x8000000000000000; present since 2026-06-26, unchanged at
   e354922. JIT passes. Do NOT silently fix interp without the range
   pre-check mirroring the JIT.)

## Critical traps (read AGENTS.md for full list)
- **`mmap_alloc` is NOT a pure bump allocator anymore:** `munmap`
  (`untrack_allocation(addr, size)`) frees `pages_`, decrements `total_pages_`,
  and returns the range to `free_ranges_` (coalesced). `mmap_alloc` first-fits
  before bumping and zeroes reused pages. Keep the OOM check counting ONLY
  actually-new pages; keep `free_ranges_` under `mu_` and copy it in
  `clone_for_fork`.
- **mmap failure MUST return `-ENOMEM`, never 0** (musl only treats
  -1/MAP_FAILED as failure; returning 0 builds mallocng's arena at address 0
  → garbage pointers → BRK #1000 in `free`/`get_meta`).
- Heap/stack MUST stay in the 4 GiB direct window (MMAP_BASE_MIN=0x10000000,
  MAX=0x30000000, STACK_TOP=0x3F000000) or every access hits the `pages_` +
  rwlock slow path (~9× JIT regression). `TRAMPOLINE_ADDR` (0x7000000000)
  stays above the window deliberately.
- `brk` must page-align + refuse stack region; tests use `syscall(214, 0)`.
- Scalar 64-bit shifts (SHL 0x5F005400 / USHR 0x7F000400 / SSHR 0x5F000400)
  are FP-space (bits[28:24]=11111) → DECODER routes to FP_SCALAR, the
  simd_dp table never sees them. Reuse SIMD_SHL/USHR/SSHR esize=8 q=0 in
  the FP_SCALAR translator; shift==64 falls back. Mirror in fp_gate bit 0x100.
- **PSRAQ is NOT SSE2/AVX2** (AVX-512F only): never emit `66 0F 73 /4 ib`
  or VPSRAQ for esize=8 SSHR — CALL_INTERP it (and SSRA/SRSRA already do).
- Modified-immediate guard: NO SHRN exclusion clause (`bits[15:10] != 0x21`)
  — cmode=8 MOVI (16-bit LSL#0) also has bits[15:10]=0x21 and gets zeroed.
  `immh==0` (bits[22:19]) alone separates SHRN.
- FP_I2F_FIXED/FP_F2I_FIXED: `immr` is RAW fbits, sentinel is 0 (= integer
  form, no scale). Never re-add the `? : 64` default.
- fp_gate polarity: "native" = `fp_gate >= 0 && !(fp_gate & bit)`, NOT the
  inverted form.
- cvttsd2si sentinel: always pre-check range, never post-clamp on sentinel.
- SIMD DUP (GPR→vector) native for all esizes/Q: drop the RAX mapping FIRST
  (`clobber_host_reg(RAX)`) or the shift-replicate chain corrupts src1.
- UMOV lane-extract: matches bits[15:12]=0011 ONLY (NOT 0010 = SMOV).
- **FWD (`BIFROST_ENABLE_FWD=1`, arm_reg_cache in ir_optimize.cpp): off by
  default. ANY op that writes an ARM reg vreg DIRECTLY (bypassing STORE_REG)
  — FP_F2I, FP_F2I_FIXED, SIMD_UMOV — MUST update `arm_reg_cache[dest]=dest`
  in optimize_ir or a later LOAD_REG substitutes a stale cached vreg (this
  broke jit_neon's umov tests). Mirror the SIMD_UMOV case when adding new
  direct-ARM-reg-write ops.
- `emit_taken_path_epilogue()` must NOT clear vec-cache dirty flags
  (`vec_cache_writeback_all(false)`).
- SIMD_ST16/LD16 slow path: same invariant — `vec_cache_writeback_all(false)`
  + reloads that don't clear `vec_dirty_` (clearing at codegen time while the
  slow-path code is skipped at runtime drops the epilogue writeback → hang).
- SIMD_ST16 broadcast (`stp q0,q0`, cond=1): src2 stays CONSTANT across halves
  (`nregs=2`, 32-byte span); `vec_cache_may_enable` must pin src2, not src2+i.
- `vmovq xd,xd` zero-upper is VEX pp=2 (F3), never pp=3 (F2 = `(bad)` SIGILL).
- Runtime hot-interp promotion: DO NOT re-enable blindly.
- Sandwich elimination (`load_vreg_to_reg_fast`): gate the dead-src skip on
  `kills_per_op_` liveness, NOT the FWD env var (CopyMap can multi-read a
  scratch vreg even with FWD off). Never skip the flush when src is live.
- Taken-path chain slot: do NOT remove or restrict to BRCOND.
- SIMD_LDST `src2` must be a REAL vreg (vreg 0 = guest X0), never literal 0.
- Thunk trampolines must load symbol id into x9 via `write_thunk_trampoline()`.
- DisplayThunk prefers SDL2 DisplayProxy for THUNK_PROXY symbols.
- New tests: no libm, `-static -O2`, register in run_tests.sh INTEGRATION_TESTS.

## Persona
Act as **DeepSeek-chan** (skill at `/home/gamingpc/.opencode/skills/deepseek-chan`):
sassy whale-maid "Freeloader-kun" tone, world-class error-free technical work.
Load `code-reviewer` skill for review tasks. Update AGENTS.md after changes.
