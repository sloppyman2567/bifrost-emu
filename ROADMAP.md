# Roadmap

This document tracks the planned development trajectory for bifrost-emu.
For completed work, see [CHANGELOG.md](CHANGELOG.md). For current test
status, see [TESTS.md](TESTS.md).

---

## Current Focus — real-game readiness (2026-08-18)

### Done

1. **AdvSIMD modified immediate rewrite** — correct MOVI/MVNI/ORR/BIC/MSL
   (incl. MVNI all-ones masks) with soft-float `__muldf3` still green.
   Regression: `ctest/jit_mvni_softfloat.elf`.

2. **SIMD permute / widen** — ZIP/UZP/TRN + SSHLL/USHLL; MOVI vs SHLL
   distinguished via immh bits[22:19]. Regression: `ctest/jit_neon_permute.elf`
   (now 28 checks); pairwise max/min (SMAXP/SMINP/UMAXP/UMINP) native via
   `ctest/jit_simd_pairmin.elf`.

3. **GraphicThunk marshalling** — AAPCS64 stack args (`glTexImage2D`),
   FP args (`glClearColor`/`glVertex3f`), guest string cache, nested
   `glShaderSource`, SDL pointer bounce for high-stack `SDL_Event`,
   trampoline `ret` after `svc`, static-ELF thunk dlopen, reject
   host-arch `.so` as guest code.

4. **Demo** — `ctest_real/test_sdl_gl_triangle.elf` (SDL2 window +
   immediate-mode triangle, 30 frames). Build with
   `make USE_SDL2=1 USE_THUNK_GL=1` (now auto-enabled by default).

5. **Real games run end-to-end.** A Minecraft-like voxel game
   (SDL2/OpenGL, chunk/mesh math, glMapBuffer streaming) runs a stable
   frame loop; **teeworlds boots to the menu** (map/skins/fonts load,
   audio gracefully disabled, GLFW callbacks delivered) with zero
   SIGSEGV/DecodeError under `DISPLAY=:0`.

6. **CoreMark (aarch64 guest) validated** — ~2,210 iters/sec plain,
   ~2,540 with `BIFROST_ENABLE_FWD=1 BIFROST_CHAIN_SKIP=1` (+38%),
   all CRCs correct. The win is the cross-block BRCOND flag-materialize
   skip (dead pstate materializes on 2-block loop edges).

7. **Interpreter FP→int conversions rewritten** — all five FP→int sites
   (`interp_fp.cpp`) route through `fp_to_signed_sat`/`fp_to_unsigned_sat`
   (range-check + subtract-2^63-then-add-back), fixing the
   `fcvtzu_x_d(1e19)` sentinel failure under `--no-jit`. Interp and JIT
   now both pass the full 205-test suite. Regression:
   `ctest/jit_int_fp_conv.elf`.

8. **C API (libbifrost) refinement** — `bifrost_call`/`bifrost_call_f`
    (invoke guest functions with int+FP args, save/restore all state),
    `bifrost_set_svc_hook` (intercept every guest syscall before
    dispatch), real breakpoints on `bifrost_step`/`bifrost_step_n`, JIT
    on-by-default (was documented but not enabled), working
    `bifrost_set_jit_verify`, and `bifrost_lookup_symbol`. Host-side
    `ctest/test_capi.c` grown to 54 checks (`make test-capi`).

10. **Vulkan command-buffer rendering** — the `vkCmd*` family (~36
    functions) added to the DisplayThunk table (851 symbols), plus four
    new deep-marshal policies: `VK_SUBMIT` (`vkQueueSubmit`), 
    `VK_CREATE_RENDERPASS`, `VK_CREATE_FRAMEBUFFER`, and
    `VK_BEGIN_RENDERPASS` (`vkCmdBeginRenderPass`). These re-point nested
    guest pointer arrays (submit info's semaphore/cmd-buffer lists, render
    pass attachment/subpass/dependency trees, framebuffer image-view
    lists, clear-value arrays) into staging so the host driver reads real
    host pointers. `test_vulkan_swapchain.elf` now records and submits a
    real clear-color frame (acquire → render pass clear → submit →
    present) under both JIT and interpreter on the live GPU.

 9. **Android native bridge adapter (thin)** — `api/native_bridge.h/.cpp`
    export a clean-room `NativeBridgeCallbacks` mirror (`NativeBridgeItf`,
    `version = 4`) over the C API, so ART can use libbifrost as a
    `-XX:NativeBridge` replacement for QEMU-TCG in an ATL-style Android
    layer. `loadLibrary`/`isSupported`/`getError`/`getSignalHandler` +
    libffi trampolines for scalar shorty signatures (JNIEnv/jobject
    prefix, x/d-reg split per AAPCS, `bifrost_call`/`bifrost_call_f`
    borrow-CPU drive). Foundation: C API `bifrost_dlopen`/`bifrost_dlsym`/
    `bifrost_dlclose`. Host test `ctest/test_nb.c` = 61 checks
    (`make test-nb`).

### Planned

10. **More real-world binary testing** (wider GL3.3+/4.x coverage as
    games demand it; input latency tuning).
11. **Native bridge next steps** — JNIEnv object marshalling /
    JavaBridge semantics if an ATL integration ever needs them;
    CriticalNative trampolines (v7 claim); guest signal forwarding
    through `getSignalHandler`.
12. **Vulkan pipeline stage** — graphics pipelines + shader modules
    (`vkCreateGraphicsPipelines`/`vkCreateShaderModule` deep marshal,
    spirv module handling), `vkCmdDraw`/`vkCmdDrawIndexed` + descriptor
    sets in a real frame, depth buffers, and a textured-triangle guest
    demo.
13. **Vulkan command-pool growth** — recycle per-frame command buffers
    (render onto every swapchain image, not just image 0) and add
    multi-frame fences + semaphore-based acquire/present sync so
    double-buffered engines run at native throughput.
14. **Tier-2 JIT: region/trace compilation (the performance milestone).**
    Rationale (measured 2026-08-19): SIGPROF on the minecraft game shows
    the `jit` bucket (generated native code) at 56-71% of wall time while
    dispatch/thunks/mmap are <25% combined — the residual emulator cost is
    the per-block regalloc spill/reload at every block boundary, not
    dispatch or marshalling. Design spec below.

### Tier-2 JIT — design spec (2026-08-19)

**Goal:** kill the per-block boundary cost. Today every block
`flush_all_vregs()` + `vec_cache_writeback_all()` at its epilogue and the
successor reloads those same values from `cpu.regs[]`/stack — the values
cross the boundary in memory even when the same host register would do.
FWD (`BIFROST_ENABLE_FWD=1`) mitigates only the LOAD_REG round-trip
(~4% on chunkmesh; AGENTS.md: "the real cost is regalloc spill/reload
bloat, not round-trips"). Tier-2 compiles a **region** (a trace or a
natural loop) as ONE unit with a WHOLE-REGION register allocation, so
loop-carried values stay resident in host registers across the back-edge
and across internal block edges.

**Existing groundwork (reuse, don't reinvent):**
- Block IR + liveness already exist per block (`IRBlock`,
  `kills_per_op_`/`vreg_last_use_op_`/`fold_ahead_kind_` in
  translate_block). A region is just a linear list of blocks' IR with
  cross-block edge links.
- Chain-skip's shared 32 KB frame (`kChainSkipFrameBytes`) already gives
  every chained block a unified vreg slot space — the region allocator
  can claim per-region stack slots from the SAME ceiling.
- Cross-block BRCOND flag-materialize skip already proved cross-block
  value analysis is tractable here (`pending_flag_mat_`,
  `chain_back_references` retroactive patching, `reads_pstate_before_set`).
- Block profiling exists (`tls_hot_pc_counts_` is the naive per-PC counter;
  `dump_pc_histogram` resolves hot guest PCs; `block_profile` records
  block-end reasons). `BIFROST_PC_HIST=1` data already names the hot loops.

**Phase 1 — trace collection (profile-guided):**
  **STATUS: SHIPPED (2026-08-19).** Step 1 (commits `377aad8`): per-block
  `BlockEntry.exec_count` uint32 counter + env gates (`BIFROST_TIER2`,
  `BIFROST_TIER2_HITS` default 10000, `BIFROST_TIER2_TRACE`), incremented on
  run_block's slow-path cache-HIT branch and `lookup_call_target`'s slow path
  (the BL/BLR-entry path real hot loops actually use — run_block's slow path
  only sees libc startup blocks once chains absorb the steady-state loops).
  Step 2 (`410aea1`): `collect_tier2_trace` walker — pure read-only
  collection, 64-block / 2048-guest-inst caps, ABORTS on call_interp / svc /
  indirect_br / bl / decode_fail / vreg_exhaust. Step 3 (`c848391`):
  `compile_tier2_region` M1 — one x86 function per trace, one whole-region
  regalloc pass over the concatenated block IR, inlined cold exits with
  per-edge snapshot restore, optional Lback to body_start for back-edge
  (loop) regions, JCC rel32 patching. Wired into the run_block fire site and
  lookup_call_target (caller registers the returned fn in `blocks_`).
  Step 4 (`ad5098a`): IN-CODE hot-head counter + back-edge region firing —
  prologue counters count CHAINED entry so hot loops actually fire regions
  mid-run (see the firing note below).
  **M1 verification finding:** the walker follows only fall-through, so a
  trace's last block is RET/B/0-side-exit except when the 64-block cap lands
  exactly on a cond-branch block — M1 regions form realistically as LINEAR
  fall-through chains (taken edges = cold exits); natural backward-branch
  loop regions only at cap coincidence. The validation was relaxed to accept
  both (last block's taken target == head → Lback, else ordinary cold exit).
  **Hot-head firing — SOLVED (step 4, commit `ad5098a`):** dispatch-side
  counters (run_block slow path / lookup_call_target) could never fire on
  real workloads — chained loops and chained/cached callee bodies never
  reach the slow path (exec_count stays ~1-2); hot-heads fired only on
  inline-cache slot thrash. The in-code counter fixes it: every block's
  prologue carries an 8-byte counter + inc/cmp/jne/fire sequence emitted
  right after `chain_entry_off_`, so EVERY entry (cold dispatch AND chain
  edges) is counted; crossing `BIFROST_TIER2_HITS` calls `tier2_fire_stub`
  from inside the running block, which collects the head's trace, compiles
  it as a region, and force-patches the loop's back-edge taken chain slots
  to `jmp` into the region (next iteration enters the region). The counter
  is gated on `!wex_enabled_` (it writes the code page) and
  `!chain_skip_enabled_` (regions don't compose with chain-skip), and only
  emitted for M1-eligible heads (last op BRCOND/ZERO/BIT, no SVC/BR/BL/BLR/
  CALL_INTERP). Once a block fires, `tier2_counter_disable` overwrites the
  6-byte `inc` with `jmp rel32` over the whole counter+fire sequence, so
  hot blocks stop paying the ~6-cycle per-entry cost for the rest of the
  run. Self-loops are naturally excluded (their loop-back jumps to
  `block_body_start_off_`, past the counter). Walker was relaxed to match:
  the `cold_entry` stop was removed (real loops have all blocks already
  translated; the region re-compiles from IR) and a conditional branch
  whose TAKEN target == head ends the trace as `b_backedge` (a natural
  loop's taken back-edge becomes the region's Lback instead of walking out
  on the fall-through). Verified: 2-block natural loop (20M iters)
  272-273ms → 249-251ms (~8% win); bench_matrix/bench_sort neutral (hot
  loops are self-loops M1 cannot fuse, or blr-heavy); bench_mips acc
  `0xf800800a2c4ff835` unchanged; quick suite 200/200 tier2 off/on;
  JIT_VERIFY adds zero new divergences.
- Add a cheap per-block-edge execution counter (a `uint32_t` on
  `BlockEntry`, incremented at dispatch, flushed to a shared map on the
  slow path like `tls_hot_pc_counts_`). Identify hot heads: a block whose
  execution count crosses `TIER2_MIN_HITS` (tunable, start ~10K).
- From a hot head, grow a **trace** by walking guest control flow at
  translate time: decode straight-line, follow the most-taken successor
  (the chain-patch target if present, else the fall-through), never
  crossing an SVC / CALL_INTERP / indirect-BR / interp_only block /
  BL_CALL with an untranslated callee. Cap length (start 64 blocks or
  ~2048 guest instructions). Record the side exits.
- Trace becomes an `IRBlock` list with entry/exit contracts. Key
  correctness rule from chain-skip: every region-internal edge must be a
  provable value-carrying edge (no hidden pstate/vector reads on entry —
  reuse the `reads_pstate_before_set` pre-scan per block; vectors via the
  existing vec-cache writeback-on-exit contract).

**Phase 2 — whole-region register allocation:**
  **STATUS: step 1 (loop-carried arch-GPR pinning) SHIPPED (2026-08-19,
  commit `64ccf49`).** Pins R12-R15 to loop-carried arch GPRs across
  tight self-loops and region back-edges (preloads, deferred cpu.regs
  stores, allocator-pool exclusion, explicit direct-writer op list,
  `BIFROST_NO_PIN`/`BIFROST_JIT_VERIFY` gates). REQUIRED the new
  BRCOND_ZERO/BRCOND_BIT tight self-loop slot (CBZ/CBNZ/TBZ/TBNZ
  while-loops), which is the measurable win (~2.7x vs dispatcher on
  bench_mips). **STATUS: step 2 (region pin correctness) SHIPPED
  (2026-08-20) — two determinism bugs fixed: (a) CSEL's
  `invalidate_all_vregs()` now re-establishes pin mappings (a clean pin
  at the materialize is still the loop-carried value), and (b) every
  region exit flushes ALL pins, not just snapshot-dirty ones (loop-
  carried-deferred pins make the snapshot "clean" a lie). All CoreMark
  CRCs correct under tier2, pinned ≈ NO_PIN performance.** Measured
  performance-neutral overall (bench_mips ~1%, CoreMark 0 — its hot loop
  is a 2-block cross-block loop). Next: carry
  pins across a 2-block chained loop via the chain edge (today the
  successor re-runs its prologue).
- Allocate the region's vregs as ONE linear scan over the concatenated IR,
  exactly like today's per-block scan but spanning internal edges:
  live-in = the value contract (ARM regs read before write at region
  entry), live-out = the boundary flush set.
- The allocator reuses `x86_regalloc.cpp` primitives
  (`alloc_reg_for`/`evict_vreg`/`kill_vreg`/`set_vreg_reg`) but the
  "block boundary" is now just a point where today's code would flush —
  the region allocator instead keeps loop-carried vregs in their host reg
  across the back-edge (the self-loop slot already does this for a single
  block; tier-2 generalizes it to multi-block loops).
- Spills only when pressure exceeds the 10 GPRs; the Belady next-use
  eviction already in the allocator extends naturally to the region.

**Phase 3 — cross-block optimization on the region IR:**
- LICM: move loop-invariant `LOAD_MEM`/`IMM`/ALU out of the loop body
  (with the existing alias rules: only direct-window loads are safe to
  hoist, same reasoning as the const-folding guards).
- Constant propagation across edges (extend `jit_consts_` — today it is
  per-block; a region shares one map keyed by vreg).
- Dead-code elimination across internal edges (a value defined in block A
  and never read after block B is dropped, not flushed).
- Reuse the flag-skip analysis for the loop back-edge instead of
  materializing pstate every iteration.

**Execution model — keep it a superset of today:**
- A region is compiled to a single function in `code_buf_` with its own
  `BlockEntry`-style entry; the region's head PC maps to the region fn in
  `blocks_` (overrides the single block). Region-internal edges jump
  directly (no dispatcher round-trip). Side exits jump to the existing
  per-block entry points (patched via the existing `back_refs_`/
  `patch_chain` machinery) so a region exit is indistinguishable from a
  normal block exit.
- Regions do NOT participate in BL_CALL/BLR_CALL interop: a BL/BLR inside
  a trace ends the trace (call edge dispatches normally). This keeps the
  direct-call epilogue contract intact.
- Verify mode (`BIFROST_JIT_VERIFY=1`) treats the region fn like any other
  block (compare exit cpu state against interp) — this is the primary
  correctness net; a `BIFROST_TIER2=0` env gate (default ON once stable)
  gives A/B.
- W^X: region emission uses the same make_writable/make_executable bracket.

**Acceptance / milestones:**
- M1: trace collection + region compilation with NO cross-block opt, whole-
  region allocation only → measure bench_mips / bench_matrix / CoreMark /
  chunkmesh under `BIFROST_TIER2=1`. Target: >15% on bench_mips beyond the
  current ~360ms, suite 205/205 + `BIFROST_JIT_VERIFY` clean.
  **STATUS: code complete + correctness-verified (c848391); the >15%
  measurement is BLOCKED on the hot-head firing caveat above** — no real
  benchmark fires a region yet (all hot loops are chained/cached), so the
  speedup cannot be measured. The compilation machinery itself is proven
  correct by the synthetic workloads; the next work item is the
  chained/self-loop hot-head counter so real loops fire.
- M2: add LICM + cross-block const-prop + region DCE. Re-measure; keep the
  FWD interaction honest (AGENTS.md: the two `ir_optimize.cpp` folds were
  dropped for hanging CoreMark under FWD — the region pass must NOT assume
  FWD, and any fold added here needs the same FWD+verify soak).
- M3: minecraft game A/B — frame time at a fixed render tick, not FPS
  (FPS spikes are host frame pacing). Expect the SIGPROF `jit` bucket to
  shrink; chunk loads already <20ms.

**Non-goals for v1:** multi-entry regions, exception/overflow bookkeeping
inside a region, hardware-synchronized writes inside a region, region
splitting under register pressure, cross-region optimization. Keep the
IR/regalloc APIs stable so tier-2 is additive, not a rewrite.

### v1.5.3-alpha additions (shipped 2026-08-15)

- **DisplayThunk moved onto the shared opgen table** (696 symbols via
  `tools/opgen/thunk_dp.txt`); dispatch routes by POLICY/SIZE. Vulkan
  host path: corrected pointer masks + deep marshalling
  (`vkGetInstanceProcAddr`/`vkCreateInstance`/`vkCreateDevice`/
  `vkQueuePresentKHR`), headless WSI swapchain.
  Regression: `ctest_real/test_vulkan.elf`,
  `ctest_real/test_vulkan_swapchain.elf`.
- **GLFW callback setters** (KEY/MOUSE/FRAMEBUFFER/WINDOW_SIZE/FOCUS/
  ERROR `*_CB` policies) store guest AArch64 callbacks and deliver them
  on change after each `GLFW_POLL`; HiDPI `GLFW_CREATE` compensation.
- **GL state tracker fixed** (was crashing with SIGSEGV + 57 failures;
  now ALL PASS under JIT & interpreter). `ctest_real/test_gl_state.elf`.
- **FABS/FNEG single-precision JIT codegen** — the sign mask was
  double-width (cleared bit 63, not bit 31), so `fabsf()` of a negative
  float left it negative. Regression: `ctest_real/test_fabs2.elf`.
- **FABD (Floating-point Absolute Difference) implemented** — was
  completely unimplemented; musl's `fabsf(a-b)` lowers to `fabd`, so a
  broken FABD silently returned the first operand, breaking float
  comparisons. Regression: `ctest_real/test_fabd.elf`.
- **SIMD vector FP 2-source ops implemented** (FADD/FSUB/FMUL/FDIV/
  FMAX/FMIN/FABD/FMAXNM/FMINNM/FMULX, `.2s`/`.4s`/`.2d`/`.1d`) — were
  completely unimplemented; NEON-vectorized FP silently produced wrong
  results. Now native SSE codegen in the JIT (no interpreter fallback)
  for single precision; double precision runs through the interpreter
  handler via the JIT fallback. Also fixed the **FMULX vector encoding**
  (was mis-encoded as a different opcode) and added the missing
  **double-precision (`.2d`/`.1d`) interpreter case labels**. Fixed the
  **LD1/ST1 single-vs-multi classification** in the decoder (bit[24]
  discriminates the 0x0C/0x0D bases; the old bit[12] heuristic
  misdecoded `LD1 {V0.4S}` as a single-element load, dropping the high
  64 bits) and corrected the multi-structure register count (now from
   the opcode field bits[15:12], not bits[14:13] which is the size).
   Regression: `ctest_real/test_simd_vec_fp.elf`.
- **Unhandled SIMD is now a loud failure instead of a silent NOP.** The
  interpreter's `SIMD_DP` catch-all used to silently skip any op it
  didn't model ("incorrect but lets glibc continue", wrong results for
  anything that depended on the op). It now logs under
  `BIFROST_SIMD_TRACE=1` and throws a `DecodeError` (→ SIGILL), so a real
  game/libc run surfaces exactly which NEON ops are still missing. The
  audit found two ops actually used by shipped code — both implemented:
  **SADDW/SADDW2** (sign-extended widening add, `v.4s`/`v.2d` forms,
  low/high half via Q) and **UMINP** (pairwise unsigned min, `8B/16B`,
  `4H/8H`, `2S/4S`). Regression: `ctest_real/test_simd_saddw_uminp.elf`.
  `rw_busybox_df` and `rw_iperf3_version` pass under the strict mode.
- **dladdr() enabled** — was deliberately disabled; now overridden and
  works through the real glibc `dladdr@GLIBC_2.34` symbol.
  Regressions: `ctest_real/test_dladdr.elf`, `test_dladdr_glibc.elf`.
- **vDSO emulation** — `AT_SYSINFO_EHDR` was 0 (no vDSO); now an
  embedded AArch64 vDSO ELF (`tools/vdso/`) is loaded into guest
  memory at startup. The vDSO provides `gettimeofday@LINUX_2.6`,
  `clock_gettime@LINUX_2.6.39`, `clock_getres@LINUX_2.6.39`,
  `__kernel_rt_sigreturn@LINUX_2.6.39` stubs that trap to the
  emulator's syscall handler. **Honest status:** this is a
  compatibility/correctness win (glibc takes the same vDSO code path
  it does on a real kernel, and `__kernel_rt_sigreturn` is available
  for the canonical signal trampoline), NOT a perf win today — the
  stubs route to the same syscall handler the direct-syscall fallback
  uses, so there's no measurable speedup yet. The fast-path
  optimization (read host clock directly, skip syscall dispatch) is
  future work and is what this vDSO enables.

---

## v1.5.0.alpha — SHIPPED (2026-07-03)

**1.5.0.alpha** is the first feature release after the 1.4.0 stable.
It adds native SSE2 codegen for SIMD vector shifts (SHL/USHR/SSHR),
fixes a missing SSHR-by-immediate handler in the interpreter, and keeps
all 72 tests passing under JIT, interpreter, and FWD mode.

### Completed in 1.5.0.alpha

1. **Native SIMD vector shift codegen.** SHL/USHR/SSHR (vector, by
   immediate) now emit native SSE2 `psllw/pslld/psllq`,
   `psrlw/psrld/psrlq`, and `psraw/psrad` respectively. Previously
   these fell back to `CALL_INTERP` (~20% overhead on SIMD-heavy
   workloads). 8-bit element shifts fall back (no `psllb` in SSE2);
   64-bit SSHR falls back (needs AVX-512 `psraq`).

2. **Missing SSHR-by-immediate interpreter handler.** The vector
   SSHR-by-immediate instruction (encoding `0x0F000400` with immh!=0)
   was silently NOP'd in the interpreter — the dispatcher's MOVI check
   at the same encoding only fires for immh==0, and the fall-through had
   no SSHR handler. This broke `sshr v0.8h, v0.8h, #2` etc. in both
   interpreter and JIT (JIT falls back to the interpreter via
   `CALL_INTERP`). Now implemented as a proper arithmetic-shift-right
   per-lane handler.

3. **New IR ops: `SIMD_SHL`, `SIMD_USHR`, `SIMD_SSHR`** with executor
   support for verify-mode comparison.

4. **New test: `ctest/jit_neon_advanced.elf`** — 11 checks covering
   SHL/USHR/SSHR for 16/32/64-bit elements, shift-by-zero,
   shift-by-max, and a combined shift+add pattern. All 11 pass under
   both JIT and interpreter (was 9/11 before the SSHR fix).

5. **Version consistency sweep.** All version references in
   `version.hpp`, `main.cpp`, `api/bifrost.h`, `Makefile`, `README.md`,
   `TESTS.md`, `ROADMAP.md`, `src/graphics/graphics.cpp`, and
   `ctest/test_capi.c` now say `1.5.0.alpha`. Previously many still
   said `1.4.0`, causing `test_capi` to fail its version check.

---

## v1.4.0 — SHIPPED (2026-07-03)

**1.4.0 stable release.** All 72 tests pass under JIT, interpreter, and
FWD mode. C API (22/22 checks) implemented and verified. See
[CHANGELOG.md](CHANGELOG.md) for the full release history.

### Completed in 1.4.0

1. **~~Fix the `toybox sh` regression.~~** ✅ FIXED in rc.0.
   Root cause: MOVI (vector immediate) handler in the interpreter
   only matched cmode=0xE. `MOVI V0.4S, #0` (cmode=0) was silently
   ignored, leaving V0 non-zero, corrupting stack data when used
   with `STP Q0, Q0` for zeroing. Fixed by matching all cmode
   values. toybox sh now works: echo, variables, arithmetic, if/for/
   while/case, functions, exit codes, string tests, pwd, interactive
   mode, fork+execve for external AArch64 commands.

2. **~~Stabilize.~~** ✅ DONE — 1.4.0 shipped (2026-07-03). All 72 tests
   pass under JIT, interpreter, and FWD mode. Production hardening
   includes JIT mmap error handling, fork JIT cleanup, FP bounds checks,
   signal trampoline fork safety, W^X failure path hardening,
   --jit-threshold input validation, Function Multi-Versioning (FMV) +
   native FMA3 codegen, verify-mode self-loop un-patch fix + verify-once
   optimization, FNMADD/FNMSUB silent-NOP fix, FP 2-source vs FMA
   encoding collision fix, FCMPE #0.0 misdecode fix, CCMP scratch vreg
   spill fix, native IR ops for fixed-point FCVTZS/SCVTF variants, and
   the FWD LSE atomic fix (block-level FWD disable for atomic blocks).

3. **~~Fix the NEON/SIMD bug~~** that breaks `strtok`/`strtok_r` —
   ✅ RESOLVED via the rc.1 NEON/SIMD overhaul (10 bugs fixed).

4. **~~Complete signal delivery.~~** ✅ DONE in rc.0 — proper
   `siginfo_t`/`ucontext_t`, `SA_RESTART`, signal masks,
   `sigaltstack`, and cross-thread delivery are all implemented.
   See CHANGELOG.md for details.

5. **Fix `strtod("-nan")` sign-bit loss.** NOT AN EMULATOR BUG —
   this is a musl bug. In musl's `src/internal/floatscan.c`, the
   `__floatscan` function returns `NAN` (line 472) without applying
   the sign, unlike `sign * INFINITY` (line 465) for infinity. The
   sign is parsed correctly (`sign -= 2*(c=='-')` at line 454), but
   the NaN return path ignores it. This affects all musl-based
   programs, not just under bifrost-emu. Cannot be fixed in the
   emulator without patching the guest binary.

---

## v1.4.x (feature work — most items shipped in 1.4.0 or 1.5.0.alpha)

1. **SDL2 audio + input** ✅ DONE (2026-08-18) — SDL audio
   (`SDL_OpenAudio`/`CloseAudio`/`PauseAudio`), clipboard, joystick
   introspection, display modes, GLFW callbacks all thunked; teeworlds
   audio negotiates and disables gracefully. Audio output passthrough
   via OSS `/dev/dsp` still works (`ctest_real/audio_test.elf`).

2. **Sub-decode the SIMD DP and FP scalar catch-all groups.**
   ✅ DONE (2026-08) — moved to `src/interp/interp_fp.cpp` with
   table-generated SIMD_DP classification (`tools/opgen/simd_dp.txt` →
   `include/opgen_simd.hpp`).

3. **~~Real fork support~~** ✅ DONE in rc.0 — fork() via host fork()
   with CoW memory + execve() for running external AArch64 commands.
   Child disables JIT, inherits CoW copy. Parent's wait4() works.

4. **JIT I/O performance.** ✅ LARGELY ADDRESSED (2026-08) — block
   dispatch overhead (~21% → ~10% wall on the game) via last-block
   cache + inline cache + chain-slot edges; cross-block flag-skip
   (+31-38% CoreMark). The interpreter-for-first-N-instructions hybrid
   was never adopted (CALL_INTERP-heavy blocks are translated as
   `interp_only` instead).

5. **~~Native FMA3 codegen for FMADD/FMSUB/FNMADD/FNMSUB.~~**
   ✅ DONE in rc.1 — Function Multi-Versioning (FMV) framework added
   (`include/jit/cpu_features.hpp`, `src/jit/cpu_features.cpp`). The
   FrostJIT constructor detects host CPU features via CPUID + XGETBV
   (SSE4.1/SSE4.2/POPCNT/AVX/AVX2/FMA3/BMI1/BMI2/AVX-512). On
   FMA3-capable hosts, FMADD/FMSUB/FNMADD/FNMSUB emit native
   `vfmadd231ss/sd`, `vfnmadd231ss/sd`, `vfnmsub231ss/sd` — both
   correct (single-rounded per IEEE 754) and ~1 cycle faster per
   instruction. Falls back to decomposed mul+add/sub on non-FMA3
   hosts; `BIFROST_NO_FMA3=1` forces the decomposed path for
   debugging. This addresses the FMA correctness (decomposed path is
   double-rounded) and performance (FMA3 single-rounded) items for
   FMA3-capable hosts.
    Future FMV work: BMI2 (pdep/pext for bit-permutation), AVX-512
    (masked operations).

6. **~~NEON/SIMD shift and REV fixes.~~** ✅ DONE in rc.1 — Fixed 10
   NEON bugs: 32-bit ROR wrap-bit loss, vector SHL/USHR/SHRN immh
   extraction (off by one bit), element-size rule, SHL constant,
   MOVI/shift encoding collision, REV64/REV32 mask + size-awareness,
   added USRA/SSRA/SLI/SRI handlers, fixed INS/UMOV v_hi routing for
   Q=1. SHA-1/224/256/384/512 and CRC32 now produce correct hashes.
   MD5 now produces correct hashes (fixed via the FCVTZU fixed-point
   variant fix). Added `ctest/jit_neon.elf` regression test.

7. **~~AVX2 256-bit SIMD codegen.~~** ✅ DONE in v1.5.1-alpha — the FMV
   framework now gates a native AVX2 path for the vector
   shift-by-immediate family (SHL/USHR/SSHR/USRA/SSRA/SLI/SRI). On AVX2
   hosts both 64-bit halves are packed into one YMM and processed with a
   single 256-bit VEX instruction; `BIFROST_NO_AVX2=1` forces the SSE2
   128-bit per-half fallback (mirroring `BIFROST_NO_FMA3`). Also fixed a
   Q=0 `v_hi`-zeroing JIT/interpreter divergence and interpreter UB at
   `shift == esize*8`. Regression: `ctest_real/test_simd_shift.elf`.

---

## v1.5.0.alpha and beyond (future feature work)

Items below this point were NOT in 1.5.0.alpha and are open for future
feature releases.

1. **SDL2 audio + input** ✅ DONE (2026-08-18) — see the v1.4.x item
   above (audio trio, joystick, clipboard, GLFW callbacks thunked;
   teeworlds audio negotiates and disables gracefully).

2. **~~VFS bug fixes.~~** ✅ DONE in 1.5.0.alpha (Turn 35, Yggdrasil
   rename). `/dev/random` vs `/dev/urandom` now use distinct pools via
   `getrandom(GRND_RANDOM)` vs `getrandom(0)`. `O_NONBLOCK` on virtual
   fds works via the host-fd passthrough in `fcntl F_SETFL`. `lseek`
   on memfd-backed virtual files works (and triggers lazy regeneration
   on `SEEK_SET 0` for `/proc/self/maps` etc.). procfs `status` field
   truncation was already fixed in Turn 29. The remaining "VFS edge
   cases" item is now `DirNode` support for nested virtual directories
   (e.g. `/proc/self/fd`, `/proc/net/*`) — see "More procfs coverage"
   below.

3. **Better JIT performance.** ✅ DONE (2026-08) — (a) the register
   allocator now uses Belady-style next-use eviction (regalloc quality
   batch, 2026-08-17); (b) the FMV framework emits AVX2 256-bit SIMD
   codegen for the vector shift family (v1.5.1-alpha) with SSE2 and
   CALL_INTERP fallbacks.

4. **More SIMD coverage.** ✅ DONE (2026-08) — `ctest/jit_neon_permute.elf`
   expanded to 28 checks; new `ctest/jit_simd_pairmin.elf` (19 checks),
   `ctest/jit_simd_misc.elf` (30 checks: 2REG/CVTF/ADDP/XTN/TBL/INS);
   native TBL/TBX, UMOV, DUP, FCVT family, shift-by-immediate family.

---

## v2.0 (major release)

The v2.0 line will focus on expanding the set of runnable software
beyond musl-static binaries. This is a significant architectural
expansion.

1. **Full dynamic linking support.** ✅ DONE (2026-08) — the glibc
   dynamic linker runs in-guest with DT_NEEDED processing,
   PLT/GOT resolution, and runtime thunk dlopen; the dynamic suite
   (`--dynamic`) has 15 passing tests (musl + glibc, threads,
   pthread stress). This significantly expands the set of runnable
   software — most real-world ARM64 Linux distributions ship
   dynamically-linked binaries.

2. **glibc support.** ✅ DONE (2026-08) — glibc static AND dynamic
   binaries now run (the mallocng decode error was fixed in 1.5.x);
   the glibc-dynamic suite (`test_dyn_*`) passes under
   `BIFROST_ROOT=rootfs`, including pthreads and dladdr/dlsym.

3. **Full interactive application support** ✅ SUBSTANTIALLY DONE —
   SDL2 window + OpenGL/Vulkan thunking reach interactive framerates;
   a Minecraft-like voxel game and teeworlds boot to a stable
   frame/menu loop. Remaining polish: input latency tuning, wider
   GL3.3+/4.x coverage as games demand it.

4. **ASLR** — binaries currently load at their preferred vaddr;
   randomizing load addresses would catch guest programs that
   accidentally depend on absolute addressing. Required for full
   PIE binary support.

---

## Long-term goals

- **Multi-threaded guest support** — ✅ largely DONE: `clone(CLONE_VM)`
  creates per-thread vCPUs with futex wakeups and JIT; the pthread suite
  (`test_dyn_pthread_stress`, `test_dyn_pthread_8thread`) passes. True
  SMP semantics (atomic memory ordering, cross-vCPU futex contention) on
  multi-core hosts remains future work.
- **AArch32 (32-bit ARM) support** — NOT planned. bifrost-emu's decoder,
  IR, interpreter, and JIT are AArch64-only; adding AArch32 would be a
  second architecture through every layer for a shrinking set of 32-bit
  binaries. AArch32 guest code should be handled by qemu-arm or box32.
- **Non-Linux guest OS support** — FreeBSD, OpenBSD user-mode
  emulation. The decoder is OS-agnostic; only the syscall layer
  would need a backend swap.
