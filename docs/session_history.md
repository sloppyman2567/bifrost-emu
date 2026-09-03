# bifrost-emu session history

moved out of AGENTS.md to keep the agent context lean. live rules stay in AGENTS.md.

## Session History (2026-08-21) — graphics-thunk review pass (bounce padding + depth range)

- **Review of the uncommitted gl_state/thunk/memory.h diff found THREE real
  issues; all FIXED in-place.**
  1. **Pixel bounce sizing ignored row alignment (host heap overflow).**
     The diff added `GLStateTracker::pixel_store_unpack/pack_alignment()`
     getters with a comment announcing a fix ("the host overreads/overwrites
     the bounce") but NEVER WIRED THEM IN — zero callers, dead code, bug
     unfixed. All four pixel SizeKind paths (TEX2D/TEXSUB/TEX3D/READPIXELS)
     sized bounces to packed `w*h*channels*type_sz` while the host driver
     accesses `(h-1)*stride+row` bytes (rows strided to GL_UNPACK/
     GL_PACK_ALIGNMENT). On the bounce path (out-of-window pointers),
     glReadPixels WROTE past our exact-sized vector (e.g. w=1 h=480 RGB_UB:
     1919 B written into a 1440 B allocation); uploads overread it.
     Fix: `pixel_extent_` lambda in translate_ptr computes the padded extent
     from the tracked alignments (fallback 4); `reserve(extent)` before
     `resize(packed)` so the ALLOCATION covers every byte the host may
     stride to while size() stays packed for an exact writeback (guest-
     visible behavior byte-for-byte unchanged). Catch path switched
     assign→std::fill (assign could reallocate below the reserved extent).
     NOTE: in-window pointers take the DIRECT-ALIAS path (no bounce) — the
     host touching spec-extent guest memory there matches real hardware;
     only the bounce path needed this.
  2. **set_depth_range stored raw values unclamped** — GL clamps to [0,1];
     unclamped storage diverged from host state AND fed potentially huge/
     NaN floats into static_cast<int32_t> (UB) at the DEPTH_RANGE integerv
     query. Now clamped (NaN→0) in set_depth_range.
  3. **glGetIntegerv(GL_DEPTH_RANGE) truncated instead of rounding** — the
     comment claimed "cast to GLint (GL spec)" but Khronos says GetIntegerv
     "rounds floating-point values to the nearest integer"; truncation
     diverges whenever frac≥0.5. Now std::lround + honest comment. Known
     residual nuance (documented, NOT implemented): DEPTH_RANGE strictly
     belongs to the linearly-mapped normalized family (1.0→INT_MAX scale);
     the float query form is what engines actually use — revisit only if a
     guest compares int-form output against host.
- **Verified correct by review (no action)**: glBindBufferRange/Base
  args[2]=buffer fix (old args[4] read the SIZE — real bug fixed);
  MAP_BUFFER placeholder reservation + drop_placeholder_ atomicity;
  GLFW deliver_glfw_callbacks_ snapshot + by-value key/mouse state copies
  (cursor/fb/winsz/focus iterators are never dereferenced after runner
  calls); cache_host_bytes_ wrap clearing BOTH live_/freed_; limits
  forwarding (MAX_*/BITS removed from tracker → host authoritative).
- **Skipped as pre-existing (not introduced by this diff)**:
  find_allocation TOCTOU double-free window (shared-lock lookup then unique-
  lock untrack — needs a locked lookup-and-remove API if ever hardened);
  glfw last-state maps not erased when glfw_cbs_[window] drops at line ~933
  (bounded, cleared at shutdown); UNMAP racing an in-flight MAP can leak a
  bounce until glDeleteBuffers (racy multi-threaded guest UB);
  GL_UNPACK_ROW_LENGTH/PACK_ROW_LENGTH/SKIP_* untracked so stride modeling
  is incomplete for guests that use them with out-of-window pixels (needs
  new tracker state).
- Verified: build clean (0 warnings), quick suite **201/201**, and on live
  DISPLAY=:0 / RADV: test_sdl_gl_triangle ALL PASS exit 0 (30 frames),
  test_sdl_gl_mapbuffer ALL PASS (21 checks), test_sdl_gl_modern ALL PASS
  (17 checks).

## Session History (2026-08-21) — full ASLR (exe bias + stack jitter)

- **ASLR was heap-only since 1.5.4; the main ELF and stack sat at FIXED
  addresses every run.** `PIE_BASE = 0x400000` hard-coded in
  elf_loader.cpp and `STACK_TOP = 0x3F000000` constant meant code gadgets
  and stack frames were ROP-addressable across runs. Now randomized in
  the Memory constructor through one `/dev/urandom` helper
  (`Memory::random_offset(range)` — page-granular, falls back to host
  stack-address entropy, then 0):
  - **ET_DYN load bias** = `PIE_BASE_MIN(0x400000) + rand(≤128 MiB)`
    (~15 bits), exposed as `Memory::pie_base()` and used by
    ElfLoader::load for e_type==3. Band ends at 132 MiB — well below
    MMAP_BASE_MIN (256 MiB) so brk keeps ≥124 MiB headroom before the
    mmap floor; zero-page guard preserved (first PT_LOAD lands ≥ 4 MiB).
  - **Main-stack top** = `STACK_TOP − rand(≤16 MiB)` (~12 bits),
    exposed as `Memory::stack_top()`. Stack bottom stays ≥ 928 MiB >
    MMAP_BASE_MAX. All five STACK_TOP consumers converted: emulator.cpp
    initial-stack build + /proc/maps [stack] entry (which now also uses
    a new `exe_base_` member for the image line instead of hardcoded
    0x400000), mem.cpp brk guard (`stack_top() - STACK_SIZE`),
    threads.cpp execve zero-scan bound + sp reset.
  - ET_EXEC keeps bias 0 (preferred vaddr) per kernel semantics.
    BIFROST_NO_ASLR=1 pins heap+exe+stack (single cached getenv in
    Memory::aslr_disabled). Randomization is CONSTANT across execve
    within one process (ELF reload reuses the same Memory; re-randomizing
    would require remap + JIT flush — documented limitation).
  - Do NOT widen the entropy bands without re-checking the window
    layout contract (heap 256..768 MiB, stack 944..1008 MiB, ELF low).
- Verified: probe binary (musl-dynamic PIE) prints dladdr fbase + heap +
  stack addrs — all three differ per run under default env; NO_ASLR pins
  exactly (fbase=0x400000, top=0x3efff…); quick suite **201/201**, build
  0 warnings.

## Session History (2026-08-21) — tier2 default ON

- **`BIFROST_TIER2` now defaults ON** (frostjit.cpp `tier2_enabled()`:
  enabled unless the env var is set AND starts with '0'). Opt-out is
  `BIFROST_TIER2=0`; `BIFROST_TIER2=1` still means on. Rationale: the
  feature has been verified correct across months of opt-in runs (suite
  200/205 under tier2 both modes, JIT_VERIFY clean, region pin bugs all
  fixed), loops-only compilation keeps it to genuine back-edge regions,
  and neutralize-after-fire bounds the counter tax — the documented
  "do NOT ship a default that leaves eligible blocks un-neutralized"
  condition holds at HITS=1000. Stale comments updated
  (jit_dispatch.cpp, jit_translate.cpp ×2, jit_glue.cpp,
  frostjit.hpp — including the stale HITS default: it is 1000, not
  10000). Verified: bench_mips acc `0xf800800a2c4ff835` with default and
  TIER2=0; minecraft live run shows `tier2: hot_heads=864 regions=69`
  in periodic stats by default; quick suite **201/201**.

## Session History (2026-08-23) — FCVTN/FCVTXN interp fix + rudolf-cart audio (leak + queue semantics)

- **FCVTN/FCVTXN in `interp_fp.cpp` were BROKEN three ways; all FIXED and
  verified against the ARM ARM (DDI 0487) via the Stanford aarchmrs mirror +
  cross-assembler ground truth:**
  1. **Case labels unreachable**: the ops live in the SIMD_DP `sub_noq`
     switch, but their opcode uses bits[20:16] (=00001) which the
     `0xFFE0FC00` sub mask CLEARS — `fcvtn v4.2s,v2.2d` (0x0E616844) masked
     to 0x0E606800 matches NOTHING → silent NOP in interp, DecodeError via
     JIT CALL_INTERP. They are now an if-handler BEFORE the switch matching
     on `op` directly with mask `0xBFFFFF00` (strips ONLY Q; U=bit29 picks
     FCVTN vs FCVTXN) against 0x0E616800 / 0x2E616800.
  2. **Q semantics inverted**: narrowing ops ALWAYS convert BOTH source
     doubles (`bits(2*datasize) operand = V[n, 2*datasize]`); Q only picks
     the destination half — Q=0 → LOW half (FCVTN zeroes high; FCVTXN
     leaves high UNCHANGED), Q=1 (*2 variants) → HIGH half, low preserved.
     The old code treated Q as source width and wrote v_lo for both.
  3. **Round-to-Odd wrong**: FPRound(FPRounding_ODD) = round_up FALSE
     (truncate toward zero), then if INEXACT force mantissa LSB to 1. A
     nearest-even cast followed by `|1` is WRONG when the cast rounded AWAY
     from the value (0x3FEFFFFFFFFFFFFF must give 0x3F7FFFFF, not
     1.0f+LSB). Overflow saturates to FPMaxNormal (never ±inf). Shared
     helper `fp_to_f32_rto()` next to fp_to_*_sat.
  - **SCALAR `fcvtxn s,d` (0x7E616800/0xFFFFFC00) also implemented** before
    the FP-NOP fallback — note it is ALSO matched by the scalar int↔FP
    group `(op & 0xDF3E0C00) == 0x5E200800` (opcode 22 falls through that
    group without returning, so ordering works, but any new opcode there
    must not return for unknown opcodes). Scalar writes only Sd; rest of
    Vd unchanged.
  - Probe: `/tmp/opencode/fcvt_probe.c` (vector Q=0/Q=1 halves, RTO
    truncate/overflow/exact/negative, both modes + BIFROST_JIT_VERIFY).
    Inline-asm probe gotcha: EVERY asm block writing memory through "r"
    pointers needs the "memory" clobber or GCC folds stale constants over
    the guest's stores and the probe lies.

- **rudolf-cart severe-lag regression ROOT-CAUSED to the audio rewrite;
  three defects fixed in `audio_thunk.cpp`/`audio.cpp`:**
  1. **Unbounded heap leak**: `Audio::write_unlocked_` accumulated EVERY
     pushed byte into `buffer_` "for potential WAV dump" but `dump_to_wav`
     has ZERO callers. Guests re-queuing multi-MB music per frame grew the
     heap GBs/min → memcpy churn (SIGPROF dispatch ~98%) → memory-pressure
     death spiral ("freeze": zero syscall progress while dispatch samples
     continue). Now gated behind `BIFROST_AUDIO_DUMP=1` (default OFF).
  2. **Queue-mode semantics**: SDL_GetQueuedAudioSize returned the shared
     256 KiB SPSC ring level instead of the device backlog, so guests with
     real-SDL refill logic (`queued < len/2`) re-queued every frame and
     overflow drops caused crackle. New model: per-device `pending`
     byte buffer in PumpStream (real-SDL semantics — buffer everything,
     cap 256 MiB defensive), drained to the engine ring by `top_up_queue_`
     from audio-thunk dispatches ON THE GUEST THREAD (no pump threads / no
     async guest callbacks — the FIXME ban holds). QueueAudio appends to
     pending; GetQueuedAudioSize returns pending bytes; ClearQueuedAudio
     clears it. Verified: 34 MB wav queued once, backlog drains smoothly
     (34.5MB→0), RSS flat, clean exit.
  3. **SDL_PauseAudioDevice now honored** for queue devices (paused gates
     top_up_queue_; unpause kicks a drain). Previously pause only set a
     flag read by the DISABLED pump — music could never be muted.
- Profiling recipe that found it all: `BIFROST_PROF=1 BIFROST_STATS_PERIOD=
  10` → syscall histogram showed 99.6% of syscalls = 0x1000 thunk SVCs at
  132K/s, then counter FROZE with dispatch still sampling = host-side
  death spiral, not guest slowness. `BIFROST_THUNK_TRACE=1` named the hot
  pair (GetTicks + GetQueuedAudioSize→constant).
- Thunk review fixes en route: `thunk.cpp` FB_DUMP scoping build error;
  mmap_alloc-failure guards (display-mode cache arm wrote to guest 0 on
  alloc failure; audio SDL_GetCurrentAudioDriver returns -ENOMEM).

## Session History (2026-08-23) — audio-thunk review pass (mixer + inline AAudio callbacks + handles)

- **Review of the uncommitted audio diff found THREE real defects; all
  FIXED:**
  1. **mix_interleaved was a silent NO-OP** (audio.cpp): it saturating-added
     onto `[tail, tail+to_mix)` — free space BEYOND the write cursor — and
     never advanced `ring_tail_`. The consumer plays only `[head, tail)`, so
     callback-mixed SFX was never audible, and the next plain write appended
     at tail and overwrote the mixed bytes byte-for-byte. Fix: overlap-add
     onto the ACTUAL queued region `[head, head+min(n, queued))` without
     touching tail (that region is already accounted), then APPEND any
     remainder at tail with a release store (bounded by free space). The
     overlap portion can tear one sample against the concurrent SDL callback
     reader — inherent to lock-free mixing, worst case a click.
  2. **AAudio data-callback streams still used the banned host pump
     threads**: `AAudioStreamBuilder_openStream` spawned a std::thread that
     fired the GUEST callback through the borrow-CPU runner on the main CPU
     ASYNC while guest code ran — the exact corruption pattern that forced
     disabling SDL pumps. Now both SDL and AAudio use the same inline
     deferral model (`cb_scheduled`/`next_cb_us`, fired from audio-thunk
     dispatches via run_due_callbacks, which iterates BOTH device maps;
     `pump_loop` deleted). AAudioStream_close's join remains as a no-op
     guard.
  3. **Handle minting from map size() collides after close/reopen** (open A,
     open B, close A, open C ⇒ `.size()==1` ⇒ C aliases B's LIVE slot via
     `map::operator[]` — leaked bounce, clobbered schedule). All families now
     mint from monotonic counters (`next_sdl_dev_`, `next_alsa_pcm_`,
     `next_pulse_`, `next_al_buf_`, `next_al_src_`, `next_aa_builder_`,
     `next_aa_stream_`). SDL devices use real-SDL-style ids STARTING AT 1,
     which also fixes the legacy `SDL_PauseAudio`/`SDL_CloseAudio` arms:
     they look up literal dev 1 but the old scheme minted `0xA5000001`, so
     they could never find the global device.
- Hardening en route: `pump_due` re-finds the device after each fired guest
  callback — a callback that closes its OWN device mid-burst would otherwise
  leave the `PumpStream&` dangling for `s.next_cb_us += period_us` (UAF;
  pre-existing shape, closed since the function was rewritten anyway).
  Fidelity: obtained-spec `samples` now reports the configured
  frames_per_cb instead of hardcoded 1024.
- Verified: build clean (0 warnings), test_linux_audio 16/16 +
  test_android_audio 21/21 under JIT, quick suite **206/206** ×2.

## Session History (2026-08-24) — vkQuake RADV crash ROOT-CAUSED + fixed; remaining pc=0 JIT bug characterized

- **The original "RADV worker-thread crash" is FIXED.** It was OURS, not
  RADV's: two thunk-layer defects, both fixed and verified (quick suite
  208/208, swapchain/mambo still pass):
  1. **AAPCS64 count masking (display_thunk.cpp VK_CMD_DEEP IN/OUT
     refs)**: element counts arrive in registers/stack slots read as full
     64-bit words, but callers commit uint32_t args with 32-bit stores —
     bits [63:32] are legal stale-frame junk (vkQuake's
     vkCmdPipelineBarrier imageMemoryBarrierCount slot carried a guest
     code address, e.g. 0x00535b9f_00000001). The old
     `cnt_arg > kVkDeepMaxElems` u64 compare silently staged **0
     elements** while the host still received count=1 (low 32 bits) →
     RADV read uninitialized reused-arena bytes as VkImageMemoryBarrier[0]
     → chased garbage pNext (the 0x3ba38c32 =
     VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR magic-check loop
     in the crash backtrace) → SIGSEGV at varying sites. FIX: mask to u32
     BEFORE any compare; if the masked count still exceeds
     kVkDeepMaxElems, FAIL THE PLAN (ok=false → full sweep → generic
     bounce fallback) — never silently under-stage. One-shot
     `vk_deep_oversize_count_once` diagnostic. Same treatment on the OUT
     copyback path; byte counts (count_in_bytes) keep u64 width.
  2. **Embedded-struct flattening (vkmarshalgen/vkxml →
     opgen_vkmarshal.hpp)**: `vk.xml` members that are structs BY VALUE
     (VkComputePipelineCreateInfo.stage!) exposed NO pointer rows, so
     stage.pName/pSpecializationInfo reached the driver as RAW guest
     pointers (RADV memcmp'd pName against "main" → SIGSEGV on guest
     .rodata). GraphicsPipelineCreateInfo worked only because pStages is
     a pointer. FIX: vkxml.layout_struct now FLATTENS single by-value
     struct members — interior pointer fields appended with adjusted
     offsets and dotted count paths ('member:stage.dataSize');
     vkmarshalgen member_offset resolves dotted paths; arrays-of-struct
     by value are NOT flattened (WARN). Descriptors 1479 → 1551
     chainable; regen via `python3 tools/opgen/vkmarshalgen.py`.
  - With both fixes vkQuake boots PAST Vulkan init, pipelines, barriers,
    swapchain — it RUNS (user saw the SDL window + a real Quake error
    dialog) — then hits the NEXT bug below. "AllocBlock: full" (lightmap
    atlas) is believed to be the SAME corruption wearing another face.
- **REMAINING BUG (next session): guest pc=0 DecodeError —
  _mi_malloc_generic's epilogue executes a SECOND time for an
  already-returned call.** Full characterization (all verified with
  env-gated probes + gdb watchpoints, repro ~100% under
  `rootfs/vkquake` with `-nosound`):
  - Death: epilogue block 0x521080 (`ldp x29,x30,[sp],#96; ret`) reads
    x30=0 from its saved-LR slot (frame_base+8 — NOTE: slot = post-push
    sp + 8, NOT sp-88; that arithmetic error cost a false lead).
  - Hardware watchpoint on the exact slot (armed via BIFROST_TRAP_MALLOC
    trap + direct-window host alias = window_base + guest_addr; window
    base from the JIT dump's `movabs r10, imm` or the 4 GiB
    /proc/PID/maps region): **61 zero-writes observed, ZERO with
    cpu->pc inside malloc's body** — the slot is NEVER corrupted while
    the frame is live. Writers are legitimate stack reuse (tinfl
    decompress zeroing its own 496-byte frame, 0x526b20, etc.).
  - Therefore the epilogue RE-RUNS after the frame is dead. Nothing in
    the guest binary branches to 0x521150/0x521080 (verified by full
    objdump grep), so the re-entry is manufactured by the JIT's control
    transfer: stale chain/taken slot, wrong fall-through next_pc, bad
    back_refs/flag-materialize/tier2 repatch, or a `br x30` with a
    stale x30 (the EPI trace shows x30==0x521150 at every arrival at
    0x521150 — suspicious; a `ret` with that x30 would land exactly
    there).
  - Gates tested and NOT it: NO_CHAIN, NO_SELFLOOP, NO_FLAGSKIP,
    NO_DIRECT_CALL, TIER2=0, NO_PIN all still crash. JIT_VERIFY runs
    (slow) with divergences at 0x51d2e8/0x100035f0 (logging-only).
  - Diagnostic probes built during the hunt were REVERTED (tree clean);
    re-add cheaply if needed: jit_glue.cpp DBG3 site — BIFROST_TRAP_MALLOC
    (one-shot SIGTRAP at the 0x51dc04→0x521150 transition + slot print),
    BIFROST_EPI_TRACE (log arrivals at 0x521150/0x521080/0x520f60 with
    from/sp/x30 — MUST be outside the dbg_call_trace_ gate or set
    BIFROST_DBG_GUARD too), BIFROST_SP_DRIFT (helper-entry/exit sp
    check — CLEAN, no drift through jit_call_helper);
    emulator.cpp step() — BIFROST_TRACE_WIN (windowed pc/sp/x30 trace,
    interp-only). gdb scripts preserved in /tmp/opencode/vkq/ (gdbwp7.py
    = the working watchpoint loop reading cpu->pc at rbx+0x108, sp at
    +0x100 per frostjit.hpp REGS_OFF=0/SP_OFF=256/PC_OFF=264).
  - Next-step plan: trap the fatal re-entry — log (a) every chain that
    ENDS at 0x521150 together with whether that chain contained a
    malloc entry (compare against entries counted at 0x520f60), or
    (b) instrument try_chain_block/back_refs/pending_flag_mat patches to
    log every slot patch (src block, slot off, target) and catch the one
    targeting 0x521080/0x521150. The 206-vs-1 asymmetry (chains ending at
    0x521150 vs at 0x520f60) is expected (prologue chains smoothly,
    0x521150 breaks before 0x521080 is chained) — do not treat it alone
    as the hijack count.

## Session History (2026-08-24) — vkQuake AllocBlock: full ROOT-CAUSED; mmap allocator hardened

- **The `pc=0` DecodeError is FIXED** (root cause was NEVER the JIT):
  `Memory::mmap_alloc`'s bump allocator had NO upper bound — vkQuake's
  mimalloc marched `mmap_next_` from 0x10000000 into the fixed main-stack
  band (944–1008 MiB), got a "4 MB segment" at 0x3edc0080, and its page-
  zeroing memset wiped live stack frames (saved LR = 0 → `ret` → pc=0).
  FIX: the bump SKIPS OVER the stack region (Linux-like: base jumps to
  `stack_top_`), the first-fit path skips stack-overlapping free ranges,
  and mremap grow-in-place refuses to extend into the stack. Verified:
  pc=0 gone, quick suite 208/208.
- **Above-window fallback added**: when the 4 GiB window is exhausted,
  mmap_alloc hands out addresses above DIRECT_WINDOW_SIZE using the
  sparse pages_ storage (new `above_window_next_` cursor) instead of
  failing — vkQuake's lightmap appetite exceeds the window, and failure
  cascaded into NULL-data chaos. JIT window fast-path bounds checks fall
  back automatically. execve zero-scan (threads.cpp) extended to cover
  allocations above the stack.
- **`untrack_allocation` hardened (Linux-like VMA split)**: the old code
  did `allocations_.erase(addr)` (exact match) + `add_free_range` UN-
  CONDITIONALLY — an interior/partial/oversized munmap put a range
  overlapping a LIVE allocation onto free_ranges_, and mmap_alloc's
  first-fit + MAP_ANONYMOUS zeroing destroyed live data. Now: intersect
  the munmap with tracked allocations, split the tracked entries around
  the freed sub-range, free only the tracked intersection, never add
  untracked space.
- **AllocBlock: full ROOT-CAUSED (still open — next session)**: vkQuake
  exhausts its 256-lightmap atlas (6.2 GB RSS spike) because AllocBlock
  receives **w = -911** for some surface (watchpoint on
  used_columns[0][0] caught the corrupting `+= BinToSize(i)` with a
  negative bin from SizeToBin(-911) — negative w passes the
  `extents > 2000` guard silently, and SizeToBin(w≤0) returns a NEGATIVE
  bin → OOB .bss read/write on columns[-912]/used_columns[garbage] →
  allocator state shredded (observed: used_columns[0][0]=-752,
  columns[12]=-799, all lightmap_idx/shelf_idx zeroed) → runaway
  allocation to the 256 cap. w=-911 ⇒ a stored extents ≈ -14592 ⇒ the
  BSP model data in the GUEST HEAP was stomped after load: a Python
  simulation from the on-disk e1m1.bsp (lump table verified valid —
  note vkQuake lump indices: LUMP_TEXTURES=2, LUMP_VERTEXES=3, and
  BSP29 dsface_t is 20 BYTES: short planenum/side/numedges/texinfo)
  shows all 3940 faces have sane extents (max 130 texels; total
  85,365 texels = 0.08 lightmaps — the packing fits the whole map in
  ONE lightmap, verified by simulation). So the stomp happens in guest
  memory between Mod_LoadFaces and GL_BuildLightmaps. PRIME SUSPECTS
  (next session): our Vulkan bounce/staging write paths (vkMapMemory
  bounce overlap via stale free_ranges_, deep-marshal copyback overrun,
  PCWFC push) — audit their guest-write bounds. The statics at
  lightmap_count==1 were CLEAN, so the stomp lands mid-build.
- Debug tooling preserved in /tmp/opencode/vkq/ (lmwatch*.py gdb scripts
  reading the lightmap statics at 0x14af848/0x14af860/etc. through the
  window alias; lit.pkl = the file-derived (w,h) table; the AllocBlock
  Python simulation). vkQuake lump-index lesson: LUMP_TEXTURES=2,
  LUMP_VERTEXES=3 (NOT the vanilla-Q2-style order), BSP29 dsface_t=20B.
- **AllocBlock follow-ups (same session, later)**: (1) the mmap stack-skip
  + above-window fallback + untrack VMA-split landed; AllocBlock: full
  STILL reproduces (the stomp source is elsewhere). (2) The BL_CALL gate
  "fixes" at 70s timeouts were FALSE NEGATIVES — re-tested at 200s, all
  gates still hit AllocBlock; the direct-call path is exonerated
  (default remains ON — a temporary default-OFF experiment was REVERTED:
  it fixed nothing and the helper path timed out the rw_* suite tests;
  the completion guard's tail-call collision remains unfixed but is not
  linked to this failure). (3) The vec
  cache exonerated (BL_CALL blocks never enable it — BL_CALL is not in
  vec_cache_compatible_op). (4) VK_MAP_MEMORY remap ignores the new
  offset/size (returns the first bounce — sub-allocation maps collide);
  not yet identified as THE stomp. (5) LEAD: BIFROST_JIT_VERIFY=1 logs a
  divergence at **mi_arenas_page_alloc_fresh+0x2a4** (0x51d2e8) — a
  load/add/store/cmp-max counter+high-water update inside mimalloc's
  page allocation. A JIT miscompile there misdirects page
  allocations/memsets → the guest-heap stomp → corrupted BSP extents
  (w=-911) → AllocBlock runaway. NEXT: chase that divergence (dump the
  block's IR + JIT x86 via BIFROST_JIT_DUMP/BIFROST_DUMP_PC=0x51d2e8,
  compare against the interpreter's values). (6) e1m1.bsp facts: 3940
  faces, 2862 lit, 85365 texels total (0.08 lightmaps), packing
  simulation fits everything in ONE lightmap — the guest-side explosion
  is purely corruption-driven.
- **AllocBlock follow-up 2 (same session, final)**: (7) The JIT_VERIFY
  divergence at mi_arenas_page_alloc_fresh is a FALSE POSITIVE — the
  block increments a counter (ldr/add/str at [x4+2488]); the verify
  re-run double-increments (x22 jit=0 ref=1, x25 jit=1 ref=2 — exactly
  +1). Non-idempotent blocks are a new verify false-positive class to
  remember. (8) The corrupting write's guest pc 0x4742ac is AllocBlock's
  while loop INLINED (attributed GL_SortSurfaces+0x54c — GCC placed the
  inlined body there; identified by the `cmp w5,#0x400` = the 1024
  LMBLOCK_WIDTH compare). At that stop x1=x10=-911 = BinToSize(-912) =
  SizeToBin(w=-911) — the small-size path (w-1 < 17 → return w-1).
  So a surface with w=-911 reaches AllocBlock. (9) **extents is `short
  extents[2]`** (16-bit!) — w=-911 ⇒ extents[0] = 0xC700 = -14592.
  File data has NO such value (all ≤ 2080, no inf/nan anywhere). The
  0xC700 heap scan found only float-coordinate noise. (10) Exonerated:
  tier-2, pins, self-loops, chains, flag-skip, direct-call (default OFF
  now anyway), vec-cache (scalar-FP blocks never enable it), FRINT
  XMM0-clobber (real latent bug — FRINT uses XMM0 as scratch without
  checking whether the fp cache pinned it; unreachable for scalar-FP
  blocks since vec_cache_compatible_op excludes them), the mimalloc
  verify divergence. (11) REMAINING CANDIDATES for next session:
  (a) JIT miscompile of SizeToBin/Q_log2 (integer log2) or the
  `(extents>>4)+1` computation — dump the JIT x86 for the AllocBlock
  block (BIFROST_DUMP_PC around 0x474280) and diff the SizeToBin
  sequence against C semantics; (b) a heap stomp on the model's
  surfaces array — find the surfaces array base (scan for the extents
  short pattern 0xC700 at msurface_t strides) and watch THAT address
  for the writer; (c) VK_MAP_MEMORY remap-offset bug (unfixed). The
  corrupting-write watchpoint recipe: watch *(int*)(window+0x16905e0)
  with condition < 0 — fires on the first negative BinToSize store
  with full JIT context (see lmwatch8.py).

## Session History (2026-08-17)


- **BRCOND self-loop flag-materialize skip is committed now** (the Opt-A
  batch, previously uncommitted and wiped by a `git reset --hard`; recovered
  from `/tmp/opencode/opt_backup/` — copies kept at
  `~/Downloads/bifrost_opt_backup/`). It is THE 2.5× bench_mips win
  (0.93s → 0.38s): on a self-loop back-edge with no loop-carried flag
  consumers (`flags_loop_carried_`, pre-scan in `jit_translate.cpp`), the
  taken path skips the ~60-byte pstate materialize. Disabling only that
  (`is_selfloop && !no_selfloop_ && false`) reverts bench_mips to ~0.96s —
  the win is entirely that skip, verified with clean rebuilds. The batch
  also includes the MEMFULL `has_svc`/`has_unresolved_store`/`has_call_like`
  verify skips, `BIFROST_DUMP_PC`/`force_dump_pc_`, and the
  `BIFROST_INTERP_BL_CALL`/`BIFROST_NO_BL_CALL`/`BIFROST_NO_BLR_CALL`
  bisection gates. `blr_call_disabled_` is defined in `ir_translate.cpp`
  next to `bl_call_disabled_` (the definition was in a file lost to the
  reset; it was hand-re-added).
- **Opt-B (generic codegen quality: IMM fold lookahead, dead-scratch-dest
  drop, SUBS dead-src1 evict skip, STORE_MEM cross-wire) is LOST** — the
  last three items' source was wiped and only the STORE_MEM cross-wire
  survived (stash + `~/Downloads/bifrost_opt_backup/`). DO NOT re-apply the
  cross-wire as-is: it measured as a standalone ~6% LOSS on bench_mips
  (400 vs 375 ms, correct but slower); the real wins were the other three
  (full set measured ~354 ms). The toybox-echo JIT hang that motivated the
  reset was in the unrecoverable set (lost `jit_dispatch.cpp` or Opt-B
  items 2/3/4) — the recovered Opt-A + cross-wire tree is hang-free,
  205/205.
- **Opt-B items 2/3/4 RE-IMPLEMENTED (2026-08-17, committed)**: dead-scratch-
  dest drop + fold lookahead + SUBS evict skip, rebuilt from the AGENTS.md
  contract + SESSION_SUMMARY rather than recovered source. All three share
  new liveness state: `vreg_last_use_op_[]` (per-vreg last-read op index,
  -1 if never read) and `fold_ahead_kind_[]` (per-op fold-lookahead class),
  both built in translate_block's use-scan. CRITICAL: the use-scan MUST
  cover `inst.aux` (SMADDL/SMSUBL accumulator) — the original scan only
  counted src1/src2, so a dest dropped as "dead" could still be read as an
  maddl accumulator. SIMD_INS also stores an element index in aux (< 33,
  filtered by the `> 31` check). The fold-lookahead IMM skip exact-matches
  the consumer's fold guards (jit_codegen_alu.cpp) INCLUDING the imm32
  sign-extend fit for the ALU class; a skipped mov whose consumer did not
  fold would leave the vreg unmapped and reload garbage. If you touch the
  fold guards on either side, mirror the change in BOTH places. bench_mips
  383-390 → **360-364 ms** (~6%). Verified: 205/205, REGALLOC_CHECK 200/200,
  FWD 200/200, JIT_VERIFY zero new divergences (rw_*/pthread/vulkan failures
  under JIT_VERIFY are pre-existing race/display false-positives — they fail
  identically at the parent commit).
- **Cross-block BRCOND flag-materialize skip (2026-08-18, the CoreMark win)**:
  the self-loop skip below generalizes to CROSS-BLOCK edges. The hot
  matrix_test loop is 2 blocks (0x401ea4 ↔ 0x401e90); BOTH edges ran a
  ~27-instruction dead pstate materialize per iteration (~25% of the loop
  code). CoreMark: plain 1687→**2210** iters/sec (+31%), FWD+CHAIN_SKIP
  1836→**2541** (+38%), all CRCs validated. Every BRCOND block that never
  reads pstate before its first flag write (`reads_pstate_before_set`,
  = `flags_loop_carried_` from the existing pre-scan) may skip the
  materialize on either edge. Fall-through edge: `flag_mat_decision(arm_pc+4)`
  at compile time — target not yet translated → SkipAndRecord (emit +
  record `{target_pc, code_off, code_len}` in `pending_flag_mat_`); target
  already translated AND clean → Skip entirely. Taken edge: SkipAndRecord
  at compile time (the target is usually untranslated then), and
  `chain_back_references` RETROACTIVELY patches the recorded region to a
  5-byte `jmp rel32` (rel = code_len−5) once the target translates clean —
  the JCC lands exactly on the region start, so the jmp hops past the dead
  ~89 bytes. Safety invariant: a materialize is only ever skipped when the
  TARGET provably never reads pstate, so a pstate-reading block always
  receives fresh pstate on every incoming edge. CRITICAL: the shared
  epilogue's `clobber_flags()` (jit_translate.cpp:1018) is a NO-OP for
  BRCOND blocks (`flags_in_host_ = false` already set at
  jit_codegen_branch.cpp:367), so the fall-through (line ~297) and taken
  (line ~345) materialize calls are the ONLY pstate writes on BRCOND edges.
  JIT_VERIFY's pstate compare is gated on `entry.reads_pstate_before_set`
  (a non-reading block may legitimately exit with stale pstate — the stale
  BRCOND comment in jit_dispatch.cpp was replaced with this contract).
  `pending_flag_mat_` is cleared at block start (taken_chain_target_pc_=0)
  and moved into the BlockEntry after `entry.has_svc`. Sites with
  code_len < 5 are erased without patching. Debug/bisection gate:
  `BIFROST_NO_FLAGSKIP=1`. Verified: suite **205/205**, JIT_VERIFY quick
  failure set IDENTICAL to clean HEAD (zero new failures — test_sem flips
  to passing), FWD+VERIFY bench_mips byte-identical
  (acc=0xf800800a2c4ff835, 0 divergences), FWD+CHAIN_SKIP+VERIFY
  bench_matrix 1 pre-existing v_lo[0] upper-half false-positive (identical
  at parent commit).
- **The two `ir_optimize.cpp` constant folds are BROKEN and DROPPED
  (2026-08-18)**: the commutative src1→src2 swap + ZEXT-after-LOAD_MEM→MOV
  fold HANG CoreMark under `BIFROST_ENABLE_FWD=1` (99% CPU spin, only the
  banner prints, exit 124; hangs even with `BIFROST_NO_FLAGSKIP=1`, so the
  folds alone cause it — and bifrost-emu IGNORES SIGTERM, kill with
  `timeout -s KILL` / `pkill -x -9`). Bisect: clean HEAD FWD+CHAIN_SKIP
  = 1812.91 iters/sec works; flag-skip without folds = 2496.88 works;
  folds re-added → hang. Worth only +0.4%; do NOT re-add without
  diagnosing the FWD hang. `ir_optimize.cpp` is pristine again
  (`git checkout --`).
- **`ctest/fadd_repro2.elf` is a BROKEN test, not an emulator bug**: it
  checks `g_r` (`[x19+0x158]`, a bss global never written by `main`) for
  the fadd result, so it FAILS on interp AND JIT AND real hardware. The
  store it targets (`str d8,[x19,#336]` → `[0x17150]`) is fine. No source
  file exists; it is not in the suite. Do not chase it.
- **Direct-BL-call callee-completion guard FIXED (2026-08-17)**: the guard's
  completion test must be `cpu.pc == x30 && cpu.pc == bl_pc + 4` (frostjit.cpp
  BL_CALL case), NOT the naive `cpu.pc == x30`. The naive test is fooled when
  a mid-callee block ends at its final BL: `MAX_BL_CALL_PER_BLOCK=2`
  (jit_translate.cpp:362) sets `chain_target_pc_ = bl_pc + 4`, which is
  EXACTLY x30 (the value BL just wrote to LR) — so if that continuation block
  isn't translated yet, the callee's fn returns EARLY with `cpu.pc == x30`
  and the guard falsely resumes the caller while the callee is mid-body with
  its frame still pushed. Observed: fmt_fp's 0x1dd0 frame leaked → printf_core
  exit @0x298c reads x30=[wrong sp+80]=0 → DecodeError pc=0 on jit_fcvt.elf.
  Pinning cpu.pc to the caller's actual continuation (`arm_pc + 4`) rules the
  collision out (true completion's exit block does BR x30 → cpu.pc = x30 =
  arm_pc+4; a mid-callee break returns a pc inside the callee, never the
  caller's continuation). Codegen: `cmp cpu.pc,x30; jne INCOMPLETE; mov rcx,
  arm_pc+4; cmp cpu.pc,rcx; jne INCOMPLETE; [resume: invalidate_all_vregs +
  vec_emit_prologue_loads]; jmp past; [INCOMPLETE: mov rsp,rbp; pop×6; ret]`.
  The layout REQUIRES a `jmp` over the INCOMPLETE block on the resume path —
  falling through resumes then `ret`s (pops the dispatcher's return addr).
  The temporary `dbg_guard_entry/dbg_guard_incomplete` probes were removed
  (they corrupted RBX and crashed block @0x400c5c — never trust probe output);
  `[DBG3]` call tracing in jit_call_helper is gated behind `BIFROST_DBG_GUARD=1`
  (was unconditional per-block spam). Verified: jit_fcvt 6/6 ALL PASS exit 0,
  full suite **205/205**, and `BIFROST_NO_DIRECT_CALL=1` /
  `BIFROST_CHAIN_SKIP=1` / `BIFROST_INTERP_BL_CALL=1` all exit 0 on jit_fcvt.

## Session History (2026-08-18) — teeworlds boots to the menu

- **Cross-block BRCOND flag-materialize skip COMMITTED (2026-08-18)**: the
  CoreMark work from the previous session. Both hot edges of the 2-block
  matrix_test loop (0x401ea4 ↔ 0x401e90) were running a ~27-instr dead
  pstate materialize per iteration; now the fall-through edge skips it at
  compile time when the target is already translated clean, and the taken
  edge records the emitted region and `chain_back_references` retroactively
  jmp-pasts it once the target translates (verified firing with a temporary
  `BIFROST_FLAGMAT_TRACE`: 522 jmp-pasts including src=0x401ea4
  target=0x401e90). CoreMark: plain 1687→**2210** iters/sec, FWD+CHAIN_SKIP
  1836→**2541** (+38%), CRCs validated. Full contract in Local Contracts.
- **The two `ir_optimize.cpp` folds were dropped** — they hang CoreMark
  under `BIFROST_ENABLE_FWD=1` (see Local Contracts). The flag-skip is the
  entire win; `ir_optimize.cpp` was reverted with `git checkout --`.
- **The teeworlds malloc-spin root cause was a STALE BINARY, not an emulator
  bug**: the installed `bifrost-emu` had an inverted UBFM/SBFM/BFM interp
  guard (threw `DecodeError` on every `sf=1` bitfield op), so
  `__libc_early_init` aborted mid-borrow-CPU-run, `ptmalloc_init` never
  self-linked `main_arena`, all bins stayed zeroed, and `_int_malloc`'s
  smallbin→tcache stash loop spun on `bin->bk == NULL`. A plain `make`
  rebuild from the correct source fixed the hang — no source change needed.
  Do NOT re-diagnose emulator memory-model corruption for this game.
- **Thunk gap work (all landed, suite 205/205, quick 200/200):**
  - Added 29 SDL rows + `glAlphaFunc` (GL, ARGS `if` mixed) to
    `tools/opgen/thunk_dp.txt`: audio trio (`SDL_OpenAudio` SDL_OPEN_AUDIO
    returns −1 — the `SDL_AudioSpec` embeds a GUEST callback that must
    never reach host SDL2; `SDL_CloseAudio`/`SDL_PauseAudio` generic),
    clipboard (`SDL_GetClipboardText` RET `str`, `SDL_SetClipboardText` `p`),
    display modes (`SDL_GetDesktopDisplayMode`/`SDL_GetDisplayMode` `ip`/`iip`,
    `SDL_GetNumDisplayModes` `i`), `SDL_WasInit` `i`, `SDL_GetVersion` `p`,
    `SDL_GL_GetDrawableSize` `ipp`, window (`SDL_MaximizeWindow`/`MinimizeWindow`/`SetWindowBordered`),
    joystick introspection (`SDL_NumJoysticks`/`JoystickClose`/`GetAttached`/`GetAxis`/
    `NumAxes`/`NumBalls`/`NumButtons`, `SDL_JoystickName`/`NameForIndex` RET `str`),
    and the rest (`SDL_GetRelativeMouseState` `pp`, `SDL_GetScancodeFromKey`,
    `SDL_SetHintWithPriority` `ppi`, `SDL_free`).
  - **New policies**: `SDL_FREE` (proper string-cache free: `string_cache_live_`
    offset→len map + `string_cache_freed_` first-fit reuse in
    `cache_host_string_`; never forwards guest cache pointers to host free),
    `SDL_OPEN_AUDIO` (return −1), `JOY_GUID` (16-byte `SDL_JoystickGUID`
    returned in x0/x1 via host RAX:RDX), `JOY_GUID_STR` (guid passed BY
    VALUE in x0/x1, out-buffer arg2 bounced at cbGUID size, writeback
    clamped to cbGUID so pszGUID[33] never overruns).
  - **Marshalling bugs found while booting teeworlds**: `SDL_GetDisplayBounds`
    was `ii` (guest `SDL_Rect*` passed verbatim → host SIGSEGV; fixed to `ip`),
    `SDL_GetKeyboardState` was `i` (guest `int*` verbatim → host SDL wrote
    into guest memory; fixed to `p`), `SDL_GetRelativeMouseState` fixed `-`→`pp`
    (two guest out-pointers).
  - **glTexImage3D needed a `TEX3D` SizeKind + a 10-arg call path**: the font
    atlas volume (w×h×d×bpp) dwarfed the 64 KiB default bounce (host gallium
    memcpy'd past it → SIGSEGV), AND the generic host-call ladder only went to
    Fn9 — `args[9]` (pixels) was silently dropped so host glTexImage3D read a
    garbage pixels pointer. Added `TEX3D` (validated in thunkgen.py
    `VALID_SIZE` + the header's SizeKind enum) and the Fn10/Fn11/Fn12 ladder
    in `dispatch()` (mirroring display_thunk.cpp).
- **SIMD DUP(element,vector) interp bug**: `dup v23.2s, v1.s[1]`
  (0x0E0C0437, imm5=12=(1<<3)|4) in `sha256_finish` threw DecodeError — the
  `case 0x0E000400` handler only matched imm5 ∈ {1,2,4,8} (index 0). Replaced
  the switch with the same ctz-based decode as the INS case. JIT side is
  unaffected (element DUP isn't in the simd_dp table → CALL_INTERP).
- **teeworlds now boots to the menu** (map/skins/fonts load, "No joysticks
  found", audio gracefully disabled) and runs a stable 45s+ frame loop with
  zero SIGSEGV/DecodeError under `DISPLAY=:0`. The remaining
  `incorrect data check` / `invalid distance too far back` lines are the
  datafile loader tolerating resource quirks, not emulator failures.
- **Interp FCVTZU sentinel bug FIXED (2026-08-18)**: `fcvtzu_x_d(1e19)` failed
  under `--no-jit` (interp 204/205; JIT passed). Raw `static_cast<uint64_t>(d)`
  for d in [2^63, 2^64) lowers to cvttsd2si → the 0x8000000000000000 sentinel.
  All five FP→int sites now route through `fp_to_signed_sat`/
  `fp_to_unsigned_sat` (see the FP→int contract). Interp now 205/205, matching
  the JIT. Verified with `ctest/jit_int_fp_conv.elf` (ALL PASS both modes).

## Session History (2026-08-19) — C API refinement (libbifrost)

- **`bifrost_call` (guest function invocation) landed in the C API**: new
  `Emulator::call_guest_function(CPU&, fn, iargs, n_iargs, fargs, n_fargs,
  double* fp_result = nullptr)` in `src/core/emulator.cpp` (~1519) — borrow-CPU
  pattern identical to `wire_thunk_glfw_cb_runner_`/`guest_call_args_`:
  save/restore ALL architectural state (regs/sp/pc/pstate/v_lo/v_hi/fpcr/fpsr/
  tpidr_el0/tpidrro_el0/sigmask/running), TLS init if `tpidr_el0==0` and the
  dynlinker has static TLS, per-thread scratch stack via `mem_.mmap_alloc(8192)`
  (thread_local, 0 = unallocated sentinel), iargs→x0..x7 (clamped to 8),
  fargs→d0..d7 (v_lo[i], v_hi zeroed), LR = `SENTINEL_LR` (0x1000), step() loop
  until pc==SENTINEL_LR or `CALL_LIMIT` (50M) — interpreter only, so it is
  deterministic regardless of JIT state. Returns x0; `fp_result` gets d0 as a
  double (a callee returning `double`/`float` puts its result in d0, NOT x0 —
  plain `bifrost_call` would return garbage for FP-returning functions). The C
  API surface: `bifrost_call` (int) + `bifrost_call_f` (double, new — returns
  the d0 result). Guest tests use AArch64 machine-code stubs written into guest
  memory via `bifrost_write_mem` (add/ret 0x8b010000/0xd65f03c0, fadd/ret
  0x1e612800/0xd65f03c0, getpid stub mov x8,#172 0xd2801588 + svc#0 0xd4000001
  + ret) at `sp-4096` (writable guest stack region).
- **SVC hook landed in the C API**: `Emulator::set_svc_hook(fn, ud)` +
  `svc_hook_`/`svc_hook_ud_` members (`src/core/emulator.h` ~102). Invoked at
  the TOP of `Emulator::syscall()` (`src/syscalls/syscalls.cpp:121`) BEFORE the
  vDSO clock fast-path and normal dispatch. Return 1 = handled: *result →
  x0, dispatch skipped; 0 = emulator handles. Sees interpreter AND JIT native
  svc (both funnel through `Emulator::syscall`), but NOT the thunk `0x1000`
  fast path (`jit_thunk_svc` bypasses the dispatcher — documented in the
  header). C API: `bifrost_set_svc_hook` returns int (0/-1 for NULL emu).
  Test intercepts guest getpid (syscall 172) → 0xCAFEBABE and confirms
  passthrough still dispatches.
- **C API JIT default fixed**: `bifrost_emu::jit_enabled` now defaults
  `true` in `api/bifrost_capi.cpp` (was false — the header docs claimed JIT
  was default but `bifrost_run` never enabled it). `bifrost_run` auto-enables
  JIT when `jit_enabled && !emu.jit()`; `bifrost_set_jit(1)` initializes the
  FrostJIT immediately (before run) or re-enables via `set_jit_enabled(true)`
  if already constructed. `bifrost_set_jit_verify` is no longer a no-op: it
  stores the flag and calls `apply_jit_verify_env()` (setenv/unsetenv
  `BIFROST_JIT_VERIFY`) — must be set before first block dispatch (the JIT
  reads the env at init). NOTE: `bifrost_set_jit(1)` now constructs the JIT
  eagerly; calling it before `bifrost_load_elf` is fine but the docs still say
  set it after load.
- **Real breakpoints**: `bifrost_set_breakpoint`/`bifrost_remove_breakpoint`
  now store guest addresses in a per-emu `std::vector<uint64_t>`; `bifrost_step`
  and `bifrost_step_n` check `cpu.pc` against the list BEFORE executing and
  return 1 on a hit (0 = stepped normally, -1 = error). `bifrost_run` does NOT
  honor breakpoints (runs to completion) — documented. The old stubs returned 0
  always and the header told callers to poll `bifrost_get_pc` instead.
- **`bifrost_lookup_symbol`** wraps `DynamicLinker::resolve_symbol` (dynamic
  binaries + thunk symbols; static musl has no .dynsym → 0). NULL/empty name
  and NULL emu guarded.
- **`ctest/test_capi.c` is a HOST binary** (links libbifrost.a; cannot be
  cross-compiled as a guest ELF) — extended from 22 to 54 checks covering the
  new API (JIT default, verify env, breakpoints incl. NULL-emu error paths,
  bifrost_call/bifrost_call_f via machine-code stubs, svc hook intercept +
  passthrough + clear, lookup_symbol guards, PC restoration after calls).
  Wired into the build: `make test-capi` (new Makefile target) and appended to
  `check-all`; `setup-tests` now skips `ctest/test_capi.c` in the musl
  cross-compile loop (it was silently failing there before). 54/54, suite
  205/205 both JIT and `--no-jit`.

## Session History (2026-08-19) — Android native bridge adapter (libbifrost)

- **C API dl* wrappers landed**: `bifrost_dlopen(emu, path, flags)` →
  guest base addr handle (wraps `DynamicLinker::load_library`, flags
  ignored/eager, refcount bump on re-load), `bifrost_dlsym(emu, handle,
  name)` (wraps `resolve_symbol_in`; 0 on miss), `bifrost_dlclose(emu,
  handle)` (wraps `close_library`; 0 success / -1 error; NULL-emu guards
  set the error string). In `api/bifrost.h` + `api/bifrost_capi.cpp`.
  `DynamicLinker` exists even for static ELFs (emulator.cpp:712), so this
  works after any `bifrost_load_elf`.
- **`api/native_bridge.h` + `api/native_bridge.cpp` — the thin Android
  native bridge adapter**: `api/native_bridge.h` is a clean-room ABI
  mirror of `NativeBridgeCallbacks` (21 fields, version 1→8 field order,
  `JNICallType`, `NativeBridgeSignalHandlerFn`,
  `NativeBridgeRuntimeCallbacks/Values`, `native_bridge_namespace_t`,
  `extern "C" NativeBridgeCallbacks NativeBridgeItf;`). The cpp fills it:
  `version = 4` (nb-qemu claim → `isCompatibleWith` returns
  `bridge_version <= 4`, so ART's `isCompatibleWith(3)`/`(4)` pass but
  `(7)` fails → legacy `getTrampoline` path). `getTrampoline` resolves via
  `bifrost_dlsym` and builds a **libffi closure** with the JNI native
  shape `ret f(JNIEnv*, jobject, <shorty args>)`; the closure body splits
  args into guest x-regs (iargs) / d-regs (fargs) per AAPCS (F packed
  into the low 32 bits of a double, since `call_guest_function` moves
  doubles into v_lo[]; F returns read the low 32 of the d0 double), then
  `bifrost_call`/`bifrost_call_f`. Shorty↔ffi: Z uint8, B sint8, C
  uint16, S sint16, I sint32, J sint64, F float, D double, L pointer, V
  void. `isSupported` = ELF magic + ELFCLASS64 + e_machine==183.
  `loadLibrary`→`bifrost_dlopen`; `unloadLibrary` frees the handle's
  closures then `bifrost_dlclose`; `getError`→`bifrost_get_error`;
  `getSignalHandler`→NULL (emulator manages host signals);
  `getAppEnv`/`createNamespace`/`linkNamespaces`/`getVendorNamespace`/
  `getExportedNamespace`/`getTrampolineForFunctionPointer`→NULL/false;
  `loadLibraryExt` routes to loadLibrary ignoring ns;
  `getTrampolineWithJNICallType` passes Regular, NULL for CriticalNative.
  CRITICAL: **`ffi_prep_cif` stores a pointer to the arg-types array (it
  does NOT copy)** — the array MUST live as long as the cif. A stack-local
  `ffi_type* atypes[16]` in `nb_get_trampoline` died at function return and
  segfaulted inside libffi's closure assembly on the FIRST call (the
  minimal-ffi repro in /tmp survived only because the call happened inside
  main while the array was still alive). Fix: `NbTramp` carries a
  `ffi_type* atypes[16]` member and `ffi_prep_cif` is passed `tramp->atypes`.
- **`ctest/nb_lib.c` + `ctest/nb_testlib.so` (gitignored, built by
  `make test-nb` / `setup-tests`)**: AArch64 JNI-shaped test lib
  (env/thiz prefix) — `nb_add` ("JJJ", add x0,x2,x3), `nb_fadd` ("DDD",
  fadd d0,d0,d1), `nb_gets` ("JJ", and x0,x2,#0xff), `nb_mix` ("DID",
  scvtf d1,w2 + fadd d0,d1,d0), `nb_fmul` ("FFF", fmul s0,s0,s1). Built
  with the musl cross toolchain `-O2 -shared -fPIC -nostdlib`.
- **`ctest/test_nb.c` (HOST binary) + `make test-nb`**: 61 checks —
  init/version==4/wiring, isCompatibleWith(3/4/7/100), getSignalHandler
  NULL, getAppEnv NULL, isNativeBridgeFunctionPointer false,
  isSupported(+/-), per-shorty trampoline calls incl. negatives +
  float/double edge cases, unknown symbol/bad shorty → NULL,
  CriticalNative→NULL, Regular routes, getTrampolineForFunctionPointer
  NULL, loadLibrary(nonexistent)→NULL + getError, unload/refcount
  (double unload → -1), direct C API dlopen/dlsym/dlclose round trip +
  NULL-emu error paths, shutdown → loadLibrary NULL. Wired into
  `check-all`; `setup-tests` skips `ctest/nb_lib.c` in the musl loop and
  builds `nb_testlib.so` separately. Makefile: libffi auto-detect
  (`NB_FFI_OK` probe → `LIB_SOURCES += api/native_bridge.cpp` +
  `LDFLAGS += -lffi`, else build warning).
- Verified: `make test-nb` **61/61**, `make test-capi` **54/54** (the
  libffi link didn't regress it), quick suite **200/200**,
  `opgen-check`/`opgen-thunk-check` clean.

## Session History (2026-08-19) — Vulkan command-buffer rendering

- **`vkCmd*` family (~36 functions) + 4 deep-marshal policies landed** —
  real Vulkan frames now render through DisplayThunk. The table grew
  696 → **851 symbols** (`tools/opgen/thunk_dp.txt`). New policies in
  `VALID_POLICY` (thunkgen.py): `VK_SUBMIT`, `VK_CREATE_RENDERPASS`,
  `VK_CREATE_FRAMEBUFFER`, `VK_BEGIN_RENDERPASS`; all four map to
  `THUNK_VULKAN` in `register_known_symbols_` and get dedicated arms in
  `vk_dispatch_` (display_thunk.cpp). The arms re-point nested guest
  pointer arrays into the `VkStage` bounce (submit info's
  pWaitSemaphores/pWaitDstStageMask/pCommandBuffers/pSignalSemaphores;
  render-pass attachment/subpass/dependency trees recursing into per-
  subpass reference arrays; framebuffer image-view handles; render-pass
  clear values), zero `pNext`, cap counts (≤16 submits / ≤32
  subpasses/attachments), call through typed host-fn pointers, and write
  back OUT handles (render pass/framebuffer at arg 3). Driven by
  frozen-layout structs (`VkSubmitInfoH` … `VkRenderPassBeginInfoH`)
  next to `VkPresentInfoH`, verified byte-for-byte vs the vendored
  vulkan_core.h. Flat-struct `vkCmd*` rows (barriers, clears, viewport,
  binds, draws, copies) ride the generic bounce — only structs with
  pointer members need a policy. `opgen-thunk-check` clean (851).
- **`test_vulkan_swapchain.elf` now records + submits a real clear-color
  frame** (22 checks, exit 0 / 77-skip): acquire → command pool +
  command buffer → vkCreateRenderPass (LOAD_OP_CLEAR, initial UNDEFINED
  → final PRESENT_SRC_KHR) → vkCreateImageView → vkCreateFramebuffer →
  vkCmdBeginRenderPass (red clear) → vkCmdEndRenderPass →
  vkEndCommandBuffer → vkQueueSubmit → present → wait → destroy.
  Verified **both JIT and `--no-jit` on the live RADV RX 7600**; quick
  suite **200/200**; CHANGELOG/ROADMAP/DISPLAY_THUNK/AGENTS.md updated.
- TODO (next milestone): graphics pipelines + shader modules
  (`vkCreateGraphicsPipelines`/`vkCreateShaderModule` deep marshal),
  vkCmdDraw/DrawIndexed + descriptor sets in a real frame, depth
  buffers, per-image command buffers (currently the test clears image 0
  only).

## Session History (2026-08-19) — Tier-2 Phase 1 step 1 (hot-head counters)

- **Tier-2 (ROADMAP #14) Phase 1 first step landed: per-block slow-path
  execution counters + env gates + hot-head diagnostics. NO trace building /
  region compilation yet.** `BlockEntry.exec_count` (uint32_t) + `tier2_hot_logged`
  (bool) in frostjit.hpp; `FrostJIT::tier2_enabled()` / `tier2_hits_threshold()`
  (default 10000) / `tier2_trace_enabled()` static getters in frostjit.cpp
  (read once, mirroring `chain_skip_enabled`); `FrostJIT::tier2_hot_heads`
  (std::atomic<uint64_t>) counter. `run_block`'s SLOW-PATH cache-HIT branch
  (jit_dispatch.cpp, after `cache_hits++`, under the shared `blocks_mutex_`)
  does `++it->second.exec_count` and, on crossing the threshold with
  `BIFROST_TIER2=1`, sets `tier2_hot_logged`, bumps `tier2_hot_heads`, and logs
  `[tier2] hot head pc=0x.. exec=N` under `BIFROST_TIER2_TRACE=1`. The fast
  paths (`tls_last_block_` / inline cache) and the interp_only demotion
  machinery (`tls_hot_pc_counts_` / `HOT_PC_THRESHOLD`) are untouched; with
  `BIFROST_TIER2` unset the whole block is skipped (two static-bool reads) so
  behavior is byte-identical. `dump_periodic_stats` prints
  `tier2: hot_heads=<delta>` after the block-end reasons (jit_glue.cpp,
  aggregated across per-thread JITs like the other counters). Verified:
  `make` clean, quick suite **200/200** (tier2 off), bench_mips acc
  `0xf800800a2c4ff835` identical with tier2 off/on/on+trace, and a synthetic
  261-block top-level asm loop (`/tmp/opencode/tier2_toploop.elf`, built from
  generated AArch64 asm, `BIFROST_NO_CHAIN=1 BIFROST_NO_SELFLOOP=1` +
  `BIFROST_TIER2_HITS=1000`) fired exactly **260 hot heads** (one per block,
  once each) and printed `tier2: hot_heads=260` in the periodic reporter.
- **Tier-2 trace collection contract (Phase 1 step 2, 2026-08-19):**
  `FrostJIT::collect_tier2_trace(emu, head_pc)` (src/jit/jit_tier2.cpp) walks
  guest code from a hot head and produces a `Tier2Trace` (linear block list,
  each with its `IRBlock` + `side_exits` (target_pc, ir-op-index pairs for
  taken edges leaving the trace)) — PURE COLLECTION, no code emission, no
  blocks_ writes (read-only `.find` membership checks only), and
  `ir_reset_vreg_alloc()` per block so the global thread-local allocator is
  never consumed. Triggered from the hot-head fire site in run_block
  (jit_dispatch.cpp, under the shared blocks_mutex_ — safe). Caps: 64 blocks
  or 2048 guest instructions (ROADMAP #14). Stop classification: ABORTS
  (ok stays false) = call_interp / svc / indirect_br / bl / decode_fail /
  vreg_exhaust; NORMAL ends = cold_entry / revisit / ret / b_exit /
  b_backedge / block_cap / inst_cap. `ok = !aborted && blocks.size() >= 2`
  (a 1-block trace is just the existing block JIT); `too_short` is set only
  when a non-aborted trace ends with no specific reason. `tier2_traces`
  counts ok=true traces (not yet wired into periodic stats). The walker
  follows only FALL-THROUGH edges; taken targets become side_exits /
  back-edges (target==head_pc). Same decode/translate machinery as
  translate_block (fetch_inst + decode + translate_to_ir, same branch-target
  formulas ip+d.imm), and the BL/BLR/CALL_INTERP/SVC abort rules mirror
  M1's "no calls in traces". The trigger hook is a call-only stub; the
  compilation task consumes the trace later. Measured on the synthetic
  cold-fall-through workload (`/tmp/opencode/tier2_coldchain5.elf`: head
  `subs;b.ne t1` with a never-executed 2240-add fall-through chain + 260-block
  Bcond thrash cycle): head fires as a hot head and the walker collects a
  **64-block / 2018-inst trace ending in `block_cap` with `ok=1`**, head's
  b.ne recorded as a side exit to t1; the 259 thrash blocks each yield
  1-block `cold_entry` traces (their fall-throughs are warm) and the cycle's
  `b head` block yields 1-block `b_exit`. Bench_mips acc unchanged
  (0xf800800a2c4ff835) with tier2 on; quick suite 200/200. Known caveat (not
  a walker bug): a hot head only fires when its inline-cache slot is thrashed
  — a block whose hash slot is not shared with any later-dispatching block
  self-pins in the 256-slot direct-mapped cache and never reaches the slow
  path (exec_count stays 1). On the synthetic thrash cycles the t-blocks
  fire (odd slots, each shared by a +1024-byte twin) but a head at a slot
  with no later writer does not; place the head so its slot collides with a
  later cycle block if you need it to fire.
- **Tier-2 in-code hot-head counter contract (Phase 1 step 4, 2026-08-19):**
  supersedes the dispatch-side counter placement for REAL firing. Under
  `BIFROST_TIER2=1` every block reserves 8 bytes of counter data immediately
  before the fn entry; M1-eligible heads (last IR op BRCOND/ZERO/BIT, no
  SVC/BR/BL_CALL/BLR_CALL/CALL_INTERP) also get a ~46-byte prologue sequence
  at `chain_entry_off_` (`inc/cmp/jne` against that data + a fire path that
  loads `rdi=[rbp+emu_slot_off()]`, `rsi=head_pc` and calls
  `tier2_fire_stub` via `emit_call_aligned(...,0)` — pushfq/popfq preserves
  RFLAGS and ABI alignment). Every entry (cold dispatch AND chain edge) is
  counted; self-loops excluded (their back-edge jumps to
  `block_body_start_off_`). Crossing `BIFROST_TIER2_HITS` fires
  `tier2_fire_region`, which runs under the EXCLUSIVE blocks_mutex_ from a
  JIT frame (try/catch; never let an exception cross the JIT boundary), sets
  `tier2_hot_logged` BEFORE collect/compile (one-shot: a failing shape fails
  identically every time), collects + compiles the trace, registers the
  region over `blocks_[pc]` preserving exec_count/hot_logged, and
  force-patches every trace block whose side_exit targets the head: its
  taken chain slot is overwritten with `jmp region-fn` REGARDLESS of
  patch_chain's unpatched-pattern guard (we WANT to redirect a live chain;
  blocks without a taken slot are left to try_chain_block later, which
  chains to the region). The region prologue expects exactly the entry
  state the taken-path epilogue sets (RDI=cpu/RSI=emu), so the hijack is
  transparent. After firing, `tier2_counter_disable` overwrites the 6-byte
  `inc` at `tier2_counter_off` with `E9 rel32` of `(tier2_counter_len - 5)`
  to skip the whole sequence (bench_matrix was +1.5% slower until this —
  a fired block's counter kept charging ~6 cycles/entry forever). Both
  `tier2_counter_off`/`len` are recorded at emit time (`len` varies because
  `emit_load` picks disp8/disp32 for the emu-slot load; `num_stack_slots_`
  is finalized at jit_translate.cpp ~751 so `emu_slot_off()` is stable).
  Gating: `!wex_enabled_` (counter writes the code page) and
  `!chain_skip_enabled_` (regions don't compose with chain-skip) — data
  bytes are reserved for all blocks but only eligible heads get the code.
  Walker relaxations that made real firing possible: the `cold_entry` stop
  is GONE (real loops have all blocks translated; the region re-compiles
  from IR anyway) and a cond-branch whose TAKEN target == head ends the
  trace as `b_backedge` so a natural loop's taken back-edge becomes the
  region's Lback. M1 pays off only on 2+ block natural loops (~8% on the
  20M-iter synthetic); bench_matrix/bench_sort are neutral because their
  dominant loops are self-loops (unfusable at M1) or blr-heavy. Do NOT
  re-add a per-dispatch atomic or a fast-path counter — the in-code
  prologue counter is the only place that sees CHAINED execution.
- **IMPORTANT FINDING for the next tier-2 step:** user hot loops are entered
  via BL/BLR from `_start`/`__libc_start_main`, so they run INSIDE
  `jit_call_helper`'s dispatch loop (`lookup_call_target`, which has its OWN
  thread-local last-block + inline caches) or inside direct `call rel32`
  chains — they NEVER reach `run_block`'s slow path. `run_block` slow-path
  dispatches are essentially only the libc `_start`/startup blocks (31 on a
  simple static musl binary), so `exec_count` stays ~1 for every real hot
  block and hot heads will NOT fire on games/benchmarks with this placement.
  The counter only grows when the 256-slot inline cache thrashes on a
  top-level loop (>256 distinct blocks in the cycle). The next step should
  ALSO increment in `lookup_call_target`'s slow path (it shares the same
  thread-local caches and `blocks_` map) or the feature stays inert on real
  workloads. Documented here so it's not re-discovered; the env gates +
  counter plumbing is exactly what the spec asked for and is a correct seed.
   **SOLVED (commit `ad5098a`):** this exact gap is why Phase 1 step 4 added
   the IN-CODE hot-head counter — see the Local Contracts section below.
- **Loop-carried arch-GPR pinning contract (Phase 2 step 1, 2026-08-19):**
  4 pins {R12,R13,R14,R15} hold loop-carried arch GPRs across a tight
  self-loop (or tier-2 region Lback). The pin set is computed per-block in
  `translate_block` and `compile_tier2_region`: eligible = arch vreg read
  (LOAD_REG src1 / STORE_REG src1) before its first write, or never written;
  written ONLY via STORE_REG (the direct writers are the EXPLICIT list
  CSEL/CSINC/CSINV/CSNEG/UBFM/SBFM/FP_F2I/FP_F2I_FIXED/FMOV_F2G/FMOV_FHI2G/
  SIMD_UMOV — do NOT use a dest<=30 catch-all: branch ops carry a DUMMY
  dest=0 and would flag x0 as directly written, killing bench_mips's x0
  pin); no SVC/BR/BL_CALL/BLR_CALL/CALL_INTERP in the block. The prologue
  preloads pins (after chain_entry_off_, before block_body_start_off_).
  STORE_REG of a pinned vreg moves src1 into the pin and DEFERS the
  cpu.regs[] store (dirty); the back-edge (self-loop slot / region Lback)
  preserves the pin; the exit flushes once. The general keep (unpinned dest)
  may only leave dest in a reg the BRCOND* term does not clobber — the term's
  flag-prep + mov-imm clobber RAX/RCX/RDX/R8 (FLAGS3) every iteration, so a
  kept dest there is read back garbage (first version HUNG bench_mips); safe
  = R9/R11/R12-R15 and not another vreg's pin. Written-then-read vregs are
  NOT pinned (measured ~6% slower — the pin starves LOAD_MEM's spare reg).
  Pinning requires the tight self-loop slot, which now exists for BRCOND_ZERO/
  BRCOND_BIT self-loops too (CBZ/CBNZ/TBZ/TBNZ while-loops); WITHOUT the tight
  slot the re-entered prologue preloads STALE cpu.regs for deferred pins
  (infinite loop). Deferral is disabled under `BIFROST_NO_SELFLOOP=1` AND
  `BIFROST_JIT_VERIFY=1` (verify un-patches the slot). Measured
  performance-neutral so far (bench_mips ~1%, CoreMark 0 — its hot loop is a
  2-block cross-block loop; pinning does not cross blocks yet).


## Session History (2026-08-19) — Tier-2 Phase 1 step 4 (in-code hot-head counter)

- **In-code hot-head counters + back-edge region firing landed (`ad5098a`).**
  Dispatch-side counters are dead on real workloads (see the finding above);
  the fix counts CHAINED execution from inside the block. Every block's
  prologue reserves 8 bytes of counter state immediately before the fn entry
  and (for M1-eligible heads only) emits a ~46-byte sequence right after
  `chain_entry_off_`: `inc dword[rip+disp]` + `cmp dword[rip+disp],imm32` +
  `jne skip` + fire path (`mov rdi,[rbp+emu_slot_off]`; `movabs rsi,head_pc`;
  `emit_call_aligned(&tier2_fire_stub,0)` — pushfq/popfq preserves RFLAGS and
  restores ABI alignment; the call is relocatable via movabs+call because
  code_buf_ ↔ binary-text distance isn't known). The RIP-relative disp32 to
  the counter is known because the 8 data bytes were emitted first
  (block_start-8). Both cold dispatches AND chain edges run the prologue, so
  every entry counts. Self-loops are naturally excluded (their loop-back
  jumps to `block_body_start_off_`, past the counter). Head eligibility
  (`tier2_head_eligible`, computed in translate_block): last IR op must be
  BRCOND/BRCOND_ZERO/BRCOND_BIT AND no SVC/BR/BL_CALL/BLR_CALL/CALL_INTERP
  anywhere in the block (walker/region compiler abort on those). Gated on
  `!wex_enabled_` (counter writes the code page) and `!chain_skip_enabled_`
  (regions don't compose with chain-skip). Data bytes are still reserved for
  all blocks under tier2 (8 dead bytes, no runtime cost).
- **`tier2_fire_region` (jit_tier2.cpp) runs under the EXCLUSIVE lock from a
  JIT frame** (the stub is called mid-block, so exceptions must never escape
  — try/catch + `try_lock` unlock). Bails on `wex_enabled_ || !tier2_enabled()`.
  One-shot per block: `tier2_hot_logged` is set BEFORE collect/compile (same
  block → same trace shape → a failed attempt fails identically every time).
  Registers the region over `blocks_[pc]` preserving exec_count/
  tier2_hot_logged; then force-patches every trace block whose side_exit
  targets the head: its taken chain slot (set up by the taken-path epilogue
  with RDI=cpu/RSI=emu) is overwritten with `jmp region-fn` regardless of
  patch_chain's unpatched-pattern guard (patch_chain REFUSES already-patched
  slots; here we WANT to redirect a live chain). Blocks with no taken chain
  slot yet are left to `try_chain_block` later (it will chain to
  `blocks_[pc].fn` = the region now). The region prologue expects exactly
  the same entry state, so the hijack is transparent.
- **Counter neutralization (`tier2_counter_disable`)**: after a block fires,
  its counter would keep charging ~6 cycles per entry forever — bench_matrix
  went 249-250ms (off) → 252-254ms (on) until this was added. The fix
  overwrites the 6-byte `inc` at `tier2_counter_off` with `E9 rel32` of
  `(tier2_counter_len - 5)` so the whole counter+fire sequence is skipped by
  a single taken branch. `tier2_counter_len` must be recorded at emit time
  (it varies: `emu_slot_off()` uses disp8 vs disp32 on the `mov rdi,[rbp+]`
  depending on the block's stack-slot count — `num_stack_slots_` is finalized
  at line ~751 before the prologue, so the offset is stable). Called on ALL
  three fire paths (in-code, run_block slow path, lookup_call_target) under
  their exclusive-lock sections. With this, bench_matrix tier2-on returns to
  249-251ms (parity).
- **Walker relaxations (needed for real firing):** (a) the `cold_entry` stop
  was REMOVED — real loops have all blocks already translated (they ran long
  enough to be hot), so stopping at the first cached block aborted every real
  trace at 1 block; the region compiler re-compiles from IR regardless of
  whether a standalone block exists. (b) a conditional branch whose TAKEN
  target == head_pc now ends the trace with `stop_reason="b_backedge"` and
  `trace_ends_after_block=true` — without it, a natural loop whose back-edge
  is the taken path (`b.ne .L3` at the loop bottom) walks OUT on the
  fall-through and follows the exit.
- **Measured:** 2-block natural loop (20M iters, `/tmp/opencode/tier2_fire/
  bigloop.elf`) 272-273ms → 249-251ms = ~8% win (the region replaces 2
  prologues + 2 epilogues + 1 dispatcher per iteration with 1 flush_all_vregs
  + 1 jmp). bench_matrix neutral (outer k-loop fires a 2-block/13-inst region
  at 0x40050c but the inner 256³ self-loop dominates and M1 can't fuse a
  1-block loop); bench_sort neutral (qsort's compare calls are blr-heavy and
  the partition inner loop is a self-loop); bench_mips acc
  `0xf800800a2c4ff835` unchanged (self-loop, zero tier2 activity). Quick
  suite 200/200 tier2 OFF (49s) and ON (55s); JIT_VERIFY zero NEW
  divergences (bigloop exits 239 / matrix 1 / sort 3 — all IDENTICAL without
  tier2, pre-existing verify-mode artifacts). Takeaway: M1 pays off on
  2+ block natural loops only — the dominant hot loops of most benchmarks
  are self-loops, which need either self-loop region support or a larger
  loop-fusion unit (Phase 2).

## Session History (2026-08-19) — Tier-2 Phase 1 step 2 (trace walker)

- **`collect_tier2_trace` landed and VERIFIED with an `ok=1` multi-block
  trace.** Implementation + contract in the Local Contracts section above.
  Working state: `make` clean, quick suite **200/200**, bench_mips acc
  `0xf800800a2c4ff835` unchanged with tier2 on, synthetic workload
  demonstrates the full 64-block/2048-inst walk ending in `block_cap` with
  the head's conditional branch recorded as a side_exit. Committed: NOT yet
  (leave for the compilation task).
- **The hot-head firing condition is the inline-cache slot**, not the slow
  path itself: `BlockEntry.exec_count` only increments on a slow-path cache
  HIT (`blocks_.find` succeeds), and a block whose 256-slot direct-mapped
  inline-cache slot (`((pc>>2)^(pc>>17)) & 255`) is never overwritten by a
  later-dispatching block SELF-PINS and never fires (exec_count stays 1).
  On the coldchain thrash cycles the t-blocks fire (each odd slot is shared
  by a +1024-byte twin) but a head at an unshared slot (or a slot shared
  only by other fast-path-pinned blocks) does not. Placement rule: put the
  head so its slot collides with a block that dispatches AFTER it each
  cycle. Unresolved micro-mystery (not blocking): on coldchain4 the slot-0
  trio (head 0x400080 / t31 0x402480 / t159 0x402880) never dispatches via
  the slow path at all (0 DBG_PC entry probes, absent from the hot-head
  list) yet still executes — the fast-path mechanics behind that asymmetry
  were not fully explained and were not needed once the head was moved to a
  shared odd slot (coldchain5). Workloads: /tmp/opencode/tier2_coldchain
  {1,2,3,4,5}.s/.elf (coldchain5 = head at 0x400084 slot 1, 2239-add
  fall-through chain → fires + walks 64 blocks).

## Session History (2026-08-19) — Tier-2 Phase 1 step 3 (region compiler)

- **`compile_tier2_region` (M1) landed and VERIFIED — commit `c848391`.**
  `src/jit/jit_tier2.cpp` now contains the whole-region compiler: one x86
  function per trace, ONE regalloc pass over the concatenated block IR
  (vregs remapped to a single region vreg space), inline prologue (RBX/RBP/
  R12-R15, lazy R10 window), per-block body via `compile_ir_inst`, manual
  BRCOND/BRCOND_ZERO/BRCOND_BIT terms (flag-prep + `cmc` for HI/LS +
  `emit_jcc_rel32_placeholder`), inlined cold exits with per-edge
  `RegionSnapshot` restore (flags/dirty/reg_vreg_/vreg_home_/vreg_dirty_),
  an optional `Lback` jmp to body_start when the last block's taken target
  is the head (back-edge / loop region), and JCC `0F 8x rel32` patching.
  Registration is done by the CALLERS (run_block fire site +
  `lookup_call_target`), which compile, then write a fresh `BlockEntry`
  (fn=rfn, instr_count=total_insts, ends_with_branch, chained, verified_once
  — M1 skips region verification). `dump_periodic_stats` prints
  `tier2: hot_heads=<delta> regions=<delta>` (new `tier2_regions` atomic).
- **M1 validation (final):** chain_skip disabled → nullptr; trace.ok;
  nblk≥2; every block exactly 1 side_exit at last op index; term ∈
  {BRCOND, BRCOND_ZERO, BRCOND_BIT}; NON-last block may not target the head
  (mid-trace back-edge rejected); **last block's taken target == head_pc →
  Lback, else → ordinary cold exit (LINEAR regions supported)**. This is
  relaxed from the original "back-edge required" design: the walker follows
  only fall-through, so a trace's last block is RET/B/0-side-exit unless the
  64-block cap lands on a cond-branch block — natural backward-branch loops
  therefore produce traces ending on the exit block and were being rejected.
  M1 regions form realistically as LINEAR fall-through chains (taken edges =
  cold exits); loop regions only at cap coincidence.
- **Verified:** synthetic `/tmp/opencode/tier2_region*.elf` — a 300-block
  fall-through chain `[subs x3,#1; b.eq x_exit]` entered via BLR 500 times
  (x3=300 → all 300 run; the fn body CHAINS so only the entry block ever
  hits the dispatcher). With `BIFROST_TIER2_HITS=1` the entry block fires on
  its first slow-path dispatch and the walker collects the 64-block cap
  trace → the region compiles (16752 B for 128 insts) and runs on every
  call; the t64 fall-through cold exit + t1 re-entry taken branch both
  exercise the deferred-epilogue snapshot restore. Results byte-identical
  to baseline across x3 ∈ {10, 300} (exit status 244 = x1=500), JIT_VERIFY
  clean (the region itself is unverified but the follow-on blocks catch any
  corruption), quick suite **200/200** tier2 OFF and ON, bench_mips acc
  `0xf800800a2c4ff835` identical, bench_matrix 656.5 MFLOPS unchanged.
- **Hot-head firing CONFIRMED the step-2 caveat (blocks NEVER reach the
  slow path twice):** on the BLR workload, `exec=1` fires at
  `BIFROST_TIER2_HITS=1`, `exec=2` at HITS=2, and then it STOPS — the fn
  body runs as one chained jmp sequence (fall-through chain slots) so only
  the entry block is dispatched, and its inline-cache slot self-pins after
  two dispatches (nothing else ever writes that slot). Regions only formed
  because HITS=1 fired on the very first slow-path dispatch. With HITS≥5 no
  fire, ever. The x3=300 variant forms FOUR regions (t1..t64, t65..t128,
  t129..t192, t193..t256 — the 44-block tail can't form one: the last block
  is an unconditional `b t1`, term class B ∉ {BRCOND, ZERO, BIT} → rejected)
  because each region's L_exit lands on a freshly-translated head. The
  M1 speedup (>15% on bench_mips) CANNOT be measured yet: no real benchmark
  fires (all hot loops chained/cached, exec_count ~1-2). **Next work item:
  a hot-head counter that counts CHAINED/self-loop execution without a
  per-dispatch atomic** (the documented fast-path rule) — tier-2 stays inert
  on real workloads until that exists.
- Region code size is ~260 B per 2-instr block (vs ~60-80 B for the same
  block standalone) — the inlined cold exits + per-edge snapshot restores
  are the bulk. 64-block traces fit in `code_buf_` fine; a trace budget
  (bytes, not just block/inst caps) belongs in the next step if traces grow.

## Session History (2026-08-19) — Tier-2 Phase 2 step 1 (loop-carried arch-GPR pinning)

- **Loop-carried arch-GPR pinning (Phase 2, ROADMAP #14) landed — VERIFIED CORRECT
  but PERFORMANCE-NEUTRAL.** `PIN_REGS[4]={R12,R13,R14,R15}` (+ `NUM_PIN_REGS`,
  `int8_t arch_pin_[32]`, `uint16_t pinned_host_regs_`, `bool keep_store_dest_`
  in frostjit.hpp): for self-loop blocks and back-edge tier-2 regions, loop-
  carried arch vregs (READ before first WRITE, or never-written loop-invariant
  reads) are pinned to fixed callee-saved regs. The prologue preloads them from
  cpu.regs[] once (cold/chain entry, AFTER chain_entry_off_ but BEFORE
  block_body_start_off_ so the back-edge skips them); STORE_REG refreshes the
  pin and DEFERS the cpu.regs[] store (marks dest dirty in its pin / transferred
  reg); the back-edge jmp to body (self-loop slot / region Lback) preserves the
  pins; the loop exit (self-loop fall-through shared epilogue / region cold+side
  exits) runs flush_all_vregs and writes them back ONCE. The region Lback's
  `flush_all_vregs()` was REMOVED (regions remap scratch vregs to disjoint
  ranges, so at the back-edge only arch GPRs are live). Pins are excluded from
  the allocator pool (alloc_reg/alloc_reg_excluding skip pinned_host_regs_).
  Per-iteration this replaces a store→cpu.regs→load round trip with two movs
  for each carried vreg.
- **CRITICAL prerequisite: BRCOND_ZERO/BRCOND_BIT self-loops now emit the
  tight 5-byte self-loop slot (mirroring BRCOND)** — previously only BRCOND
  got it, so a CBZ/CBNZ/TBZ/TBNZ while-loop (bench_mips's hot counter loop,
  GCC vectorized memchr/strchr) returned to the dispatcher and re-ran the
  FULL prologue every iteration. The deferred pins are UNSAFE without the
  tight slot: the re-entered prologue's preloads read STALE cpu.regs[] for the
  deferred values (the taken path never stored them) → infinite loop. The
  taken path now emits `E9 rel32` (patched to jmp body_start at block end)
  + the dead epilogue (store PC, restore regs, ret — never executed). BRCOND's
  own self-loop dead epilogue matches. The slot change alone is a real feature
  (tight CBZ/TBZ loops are ~2.7× vs dispatcher per NO_SELFLOOP on bench_mips).
- **The STORE_REG keep is gated on the kept register surviving the term:**
  the BRCOND* term's flag-prep + mov-imm clobber RAX/RCX/RDX/R8 (FLAGS3) EVERY
  iteration, so a kept dest left in a FLAGS3 reg is read back garbage by the
  next iteration's LOAD_REG (compiled as a direct reg→reg mov). Only R9/R11/
  R12-R15 survive the term. Pinned dests always go to their pin; the general
  keep (transfer dead src1's reg / self-store) requires s ∉ FLAGS3 AND s not
  another vreg's pin, else eager store + kill. The first version without this
  gate HUNG bench_mips.
- **Deferral safety invariants:** keep_store_dest_ is enabled ONLY for tight
  self-loop blocks / back-edge regions (whose exits always flush); call-like
  ops (SVC/BR/BL_CALL/BLR_CALL/CALL_INTERP) exclude pinning entirely (they
  read cpu.regs[] directly and would see the stale deferred value);
  `BIFROST_NO_SELFLOOP=1` disables pinning (no tight slot); `BIFROST_JIT_VERIFY=1`
  ALSO disables pinning — verify un-patches the self-loop slot, so the taken
  path re-enters the prologue and reads stale deferred cpu.regs (JIT_VERIFY
  reported a NEW divergence at pinned block 0x4007a4 until this gate).
- **Direct-write detection uses an EXPLICIT op list, NOT a dest<=30 catch-all:**
  branch ops (BRCOND/BRCOND_ZERO/BRCOND_BIT/BRCOND_FALLTHRU/BRCOND_SKIP/BR/
  BL_CALL/BLR_CALL/CALL_INTERP/SVC) carry a DUMMY dest=0, so a catch-all
  flagged x0 as directly-written and killed bench_mips's x0 pin (the loop-
  invariant address base whose pin is the whole point). The real direct arch-
  GPR writers are CSEL/CSINC/CSINV/CSNEG/UBFM/SBFM/FP_F2I/FP_F2I_FIXED/
  FMOV_F2G/FMOV_FHI2G/SIMD_UMOV (store_reg_to_vreg/set_vreg_reg bypassing
  STORE_REG). Keep this list in sync with the codegen.
- **Written-then-read vregs (pass 2) are NOT pinned — measured ~6% SLOWER**
  on bench_mips (376-379 vs 354-357 ms): the extra pin occupies a scratch
  register for the whole loop and the body's LOAD_MEM needs the spare; one
  extra spill per iteration costs more than the store→load round trip it
  removes. Pass-1 only (carried + invariant).
- **Measured:** bench_mips 354-357 ms vs HEAD 357-359 (~1%, noise); CoreMark
  3469-3490 vs HEAD 3483-3495 (neutral — the hot matrix loop is a 2-BLOCK
  cross-block loop, which pinning does not cover yet). NO_SELFLOOP baseline
  is 1018 ms. The pin machinery is a correct, verified stepping stone for
  cross-block pinning (Phase 2 next: carry pins across a 2-block chained loop
  via the chain edge, which today re-runs the successor's prologue).
- Verified: suite 200/200 (50s), REGALLOC_CHECK bench_mips clean, JIT_VERIFY
  failure set IDENTICAL to HEAD (the one "extra" line test_pthread_mutex also
  fails at HEAD — flaky timeout rc=137 vs abort 134, same pre-existing racy
  pthread/sem/sig/toybox-rw/GL/vulkan set), bench_mips acc
  `0xf800800a2c4ff835`. Bisection gates kept: `BIFROST_NO_PIN=1` (pins off,
  slot on), `BIFROST_NO_SELFLOOP=1` (both off).

## Session History (2026-08-20) — Tier-2 Phase 2 step 2 (region pin correctness fix)

- **Two pin bugs found via CoreMark bisection; both FIXED — all three CRCs
  now correct (crclist 0xe714, crcmatrix 0x1fd7, crcstate 0x8e3a, crcfinal
  0x25b5) under `BIFROST_TIER2=1`, matching NO_PIN.** The pre-fix pinned
  run was deterministic-but-wrong (crclist 0xfdcc, crcstate 0x2812).
  Diagnosed with a temporary per-region pin gate (pin-only / skip-pin,
  REMOVED after bisection): **region 0x401010's pins ALONE corrupted crclist**
  (a list-pointer chase loop); crcmatrix's 0x401e44 needed the CSEL fix.
- **BUG 1 (crcmatrix, region 0x401e44) — CSEL flag-materialize dropped pin
  mappings.** CSEL codegen (jit_codegen_alu.cpp:405) runs
  `flush_all_vregs()` + `invalidate_all_vregs()` when `!flags_in_host_`
  (pstate→RFLAGS materialize). The flush stored only compile-time-DIRTY pins
  (r13/r14/r15); the CLEAN pin r12 (x0, first write later in the block) was
  NOT stored, then `invalidate_all_vregs()` dropped ALL pin mappings. A later
  `LOAD_REG x0` compiled to `mov (%rbx),%rax` (stale cpu.regs[0]) instead of
  reading r12 → loop-carried x0 lost. FIX: `invalidate_all_vregs()`
  (x86_regalloc.cpp ~592) RE-ESTABLISHES the pin mappings after the clear —
  `vreg_home_[a]=arch_pin_[a]`, `reg_vreg_[r]=a`, `vreg_dirty_[a]=true`,
  `dirty_host_regs_|=1<<r`, LRU bump. Mapping-only (NO reload — cpu.regs[a]
  may be stale; a reload would destroy the loop-carried value). Safe because
  the CSEL materialize clobbers only FLAGS3 and call-like ops are excluded
  from pinned blocks (has_call eligibility), so the pin registers always hold
  the current arch values. **Do NOT "optimize" this to only re-pin dirty
  vregs — the whole point is that a clean pin at the CSEL is still the
  loop-carried value at runtime.**
- **BUG 2 (crclist, region 0x401010) — cold exits returned STALE cpu.regs[]
  for loop-carried-deferred pins.** Region exits flush only the pins DIRTY at
  the compile-time snapshot. A pin deferred in block 1 (STORE_REG → pin reg)
  on a prior Lback iteration is "clean" at block 0's snapshot (block 0's IR
  never writes it), so the block-0 cold exit (`x26==x4` "found" path at
  0x1cc) returned without storing x2 (r12) → the caller at 0x400f70 read the
  pre-loop list pointer → wrong list navigation → wrong CRC. Same class as the
  fp-cache pre-call "loop-carried dirtiness" bug: the snapshot's clean/dirty
  is a per-block compile-time lie for pins written in a later block of the
  SAME loop. FIX: `emit_flush_all_pins()` in `compile_tier2_region` — every
  exit (L_exit AND every cold side-exit) unconditionally emits
  `emit_store_arm(a, arch_pin_[a])` for ALL pins, after flush_all_vregs.
  Over-flushing is safe (pins always hold the current arch value; the extra
  store to an already-current cpu.regs[a] is redundant). The Lback still does
  NOT flush (that's the perf point). Note region 0x401cb8 had the SAME latent
  bug (x0/x1 deferred in block 1, cold exit at block 0 flushed only x2/x3)
  but happened to pass because the exit targets don't read x0/x1.
- **Defensive: LOAD_MEM and ATOMIC added to the direct-write list** in BOTH
  pin scans (jit_tier2.cpp + jit_translate.cpp): they write the dest arch
  vreg via `set_vreg_reg`/`store_reg_to_vreg` bypassing STORE_REG, so a pinned
  vreg written by one would land in a scratch reg the next iteration clobbers
  → abandoned pin. NOT the actual CoreMark bug (region arch writes all went
  via STORE_REG) but correct defensive coverage; keep the EXPLICIT-list rule
  (branch dummy dest=0 must never flag x0).
- **Performance:** pinned ≈ NO_PIN ≈ +~1% guest-measured over NO_PIN on
  CoreMark (3275 vs 3243, noisy guest clock), correct in both. Region exits
  are cold paths — the extra pin stores cost nothing measurable. Wall-clock
  tier2-ON vs OFF on auto-scaled CoreMark ~16s vs ~21s (auto-scale makes this
  noisy). Verified: full suite **205/205** (tier2 OFF default), quick
  **200/200** tier2 ON, bench_mips acc `0xf800800a2c4ff835` both modes,
  REGALLOC_CHECK clean, JIT_VERIFY quick failure set IDENTICAL to tier2-OFF
  (the only diffs are failure FLAVOR of the same pre-existing racy tests:
  test_pthread_cond rc=137 vs 134, vulkan 139 vs 134).
- Bisection gates `BIFROST_SKIP_PIN_PC`/`BIFROST_PIN_ONLY_PC` and the
  `BIFROST_PIN_TRACE`/`[rbin]` region dumps were TEMPORARY and are REMOVED.

## Session History (2026-08-20) — M2: region DCE + LICM + cross-block const-prop

- **M2 (ROADMAP lines ~272-275) landed in `src/jit/jit_tier2.cpp`, uncommitted.**
  Three region-level optimizations over the concatenated back-edge region IR,
  all gated (OFF by default under the tier2 machinery; the whole file is only
  active under `BIFROST_TIER2=1`):
  1. **Region DCE** (`BIFROST_NO_RDCE=1` disables): removes pure-GPR scratch
     ops (dest>32, `is_m2_pure_gpr` whitelist: IMM/MOV/ADD/SUB/MUL/AND/OR/XOR/
     SHL/SHR/SAR/ROR/NOT/NEG/SEXT/ZEXT/CLZ/CLS/RBIT/REV16/32/64/UBFM/SBFM/
     EXTR/BFM/UDIV/SDIV/SMADDL/UMADDL/SMULH/UMULH/SMSUBL/UMSUBL) whose dest is
     never read. One pass suffices (removal only removes definitions). The
     dest>32 guard auto-protects FP/SIMD (dest=FP index 0-31) and arch writes.
  2. **LICM** (`BIFROST_NO_LICM=1` disables): hoists loop-invariant pure GPR
     ops + GPR LOAD_REG of never-written, NON-pinned archs into a preheader
     emitted once per region entry (the Lback jumps to body_start, past it).
     Forward pass with `hoisted_vreg[]`; scratch sources must be hoisted,
     arch sources must satisfy `!arch_written[s] && arch_pin_[s] == -1`.
     CSEL family excluded (reads flags). IMM is hoistable (chains like
     `add x12,x8,x8,lsl#1` = IMM(1);SHL;ADD need the IMM hoisted or the chain
     strands in the body — excluding it measured ~1% SLOWER on m2loop3 because
     only the bare LOAD_REGs hoisted and the body reloaded them from slots),
     then REFINED: un-hoist an IMM with no hoisted consumer (prevents `mov
     wN,#imm`→slot-load regression). Preheader emission: after pin preloads,
     before body_start; `jit_consts_.clear()` moved up before it; preheader
     ops compile with `cur_op_index_ = fold_ahead_kind_.size()` (no fold-ahead
     skip); then `flush_all_vregs(); invalidate_all_vregs();` so block0's body
     reloads hoisted results from their pre-assigned stack slots each iter.
     `rblocks` start/term rebuilt after the transforms by scanning for
     BRCOND/ZERO/BIT in order.
  3. **Cross-block const-prop (fold-ahead extension)**: the fold-lookahead
     pre-scan now marks an IMM fold-ahead-skippable when its dest has EXACTLY
     ONE consumer ANYWHERE later in the region (`vreg_uses_[dest].size()==1`,
     use index > i), not just the adjacent `i+1` op. The consumer must be a
     folding ADD/SUB/AND/OR/XOR (kind 1) or SHL/SHR/SAR/ROR (kind 2) with the
     standard guards (`src2==dest`, `dest!=src2`, `src1!=src2`). Safety: vregs
     are unique per def; single-use ⇒ dead at the consumer (the fold's
     `vreg_last_use_this_op` holds at op j); jit_consts_ survives (line 1000
     only erases for non-IMM dests, and nothing re-defines the const between
     i and j); kind-1's imm32 sign-extend fit is re-checked at the IMM codegen
     (emits the mov on mismatch, so a non-folding consumer still finds the
     vreg mapped); kind-2 shift fold has no extra guards.
  **CRITICAL LICM BUG FIXED (the game_demo hang):** the `s==0` "no source"
  shortcut in `src_invariant` was checked FIRST, so `LOAD_REG x0` (src1=0 =
  arch reg 0, a REAL operand) returned "no source" → the LICM hoisted x0's
  reads even when x0 was WRITTEN in the loop (STORE_REG x0) AND/OR pinned. On
  the minecraft game's 0x400658 loop this froze the game forever (the body
  read the stale pre-loop x0). The `s==0` shortcut must apply ONLY to
  non-LOAD_REG ops (LOAD_REG's src1 is always the arch index 0-30). Diagnosed
  by bisecting game_demo tier2-on hangs: `NO_LICM=1` passed, `NO_RDCE=1` hung
  → LICM; the `[m2]` region-IR dump (temporary `BIFROST_M2_DUMP`) showed six
  hoisted `LOAD_REG s1=0` alongside a body `STORE_REG d=0`. LOAD_REG x0
  hoists are now blocked by both `arch_written` and `arch_pin_`.
- **Pin analysis moved BEFORE the LICM pass** so LICM can consult `arch_pin_`
  (a pinned-arch LOAD_REG is already free in the body — hoisting it to a
  slot is a per-iteration regression). Computing pins pre-LICM is safe: LICM
  only hoists NON-pinned reads, so a pinned arch's read stays in the body and
  the pin stays live; an unpinned arch whose read is hoisted simply has no
  pin. The pin scan itself is unchanged (same rules, explicit direct-write
  list, first_read/first_write ordering).
- **Measured (raw-asm 2-block natural loops, /tmp/opencode/m2loop*.S):
  m2loop4** (heavy invariant chain on an UNPINNED arch — the 4 pins go to
  x8/x9/x10/x11, so x15's chain is hoistable): tier2 OFF 7.49-7.53s, M1-only
  (NO_LICM) 5.32-5.35s (~28% win from the region alone), M2 full
  **5.05-5.06s (~5% LICM over M1)**, NO_RDCE ≈ full (DCE neutral on this
  workload). **m2loop2** (light invariant): M2 4.32 vs M1 4.33 (LICM neutral —
  the hoisted 2 ADDs ≈ the added slot load). **m2loop3** (heavy chain on a
  PINNED base): M2 5.60 vs M1 5.54 — ~1% REGRESSION, because the pinned base
  (x8) makes the chain non-hoistable AND LICM hoisted only the bare non-pinned
  LOAD_REG (x14) which then read from a slot instead of the cheap pin; the
  `arch_pin_`-gating fix does NOT fully cure it (the chain itself is
  correctly left in the body, so m2loop3's body is unchanged — the ~1% is
  the lone hoisted x14 slot-load). Lesson: LICM pays off when the invariant
  chain rests on an unpinned arch (or more invariants than pins); with the
  pins already covering the invariants, LICM is neutral-to-slightly-negative.
  Workload-shaping notes: if/else arms need an unconditional `b` (else the
  walker treats the fall-through as `b_exit` → no region); the loop head must
  be its OWN block (`b .Lloop` boundary) else the back-edge lands mid-block;
  the early-exit sentinel must be a rare value (a `tst i,#3;b.eq` exits on
  iteration 0); and `movz` only loads 16 bits — a `movz x14,#0x4000` sentinel
  is 16384, not 0x40000000 (the first m2loop timing runs were 16K iterations,
  hence LICM-neutral).
- **Verification:** acc byte-identical across interp/JIT/tier2 on all three
  m2loops (m2loop2 `9c36d0c9c94272e2`, m2loop3 `7fe59a7b0607e75e`, m2loop4
  `b30ae040b4e1553d`; quick n=20M variants for the interp run, which is slow
  on 1e9 iters). `BIFROST_TIER2=1 BIFROST_JIT_VERIFY=1` on m2loop2_quick/
  m2loop4_quick: no divergence lines. bench_sort/bench_matrix tier2+verify
  divergence sets IDENTICAL to the tier2-OFF baseline (0x4024a4/0x40533c/
  0x408148 sort, 0x405ec8 matrix — all pre-existing logging-only false
  positives). Quick suite **200/200** with tier2 OFF AND ON (the game_demo
  tier2 hang is fixed; a second region 0x40064c now fires cleanly). The
  game_demo 0x400658 region's bytes dropped 1433 → 1253 once the bogus x0
  hoists (and the chains they enabled) were gone.
- Env gates: `BIFROST_NO_RDCE`, `BIFROST_NO_LICM`, plus the existing
  `BIFROST_NO_PIN` / `BIFROST_NO_SELFLOOP` / `BIFROST_CHAIN_SKIP` (regions
  decline under chain-skip). All M2 work is uncommitted; the diff is confined
  to `src/jit/jit_tier2.cpp` (~309 insertions / ~100 deletions, mostly the
  moved pin block). NOT committed — tree is at HEAD `85a09e9` + this diff.

## Session History (2026-08-20) — M2b: unconditional-branch region terms

- **BRCOND_FALLTHRU term support landed in `compile_tier2_region`
  (jit_tier2.cpp, uncommitted)** — regions now accept a LAST block that ends
  in an unconditional direct `b target` (encoded as `IROp::BRCOND_FALLTHRU`,
  ir_translate.cpp:672-678), which the M1 validation previously rejected as
  "term class B ∉ {BRCOND, ZERO, BIT}". This unlocks the standard GCC loop
  idiom `top: cmp; b.hs exit; body; b .loop` (test-at-top cond + UNCOND
  back-edge), the shape behind the 44-block-tail rejection from Phase 1 step
  3. The walker already produced it (InstClass::B → side_exit + b_backedge /
  b_exit); only the compiler dropped it.
  - **Back-edge case** (`b head`, last_is_backedge): emit the Lback INLINE at
    the term — `materialize_flags_to_pstate()` iff `region_flags_loop_carried`,
    then `jmp body_start` (rel32 known at that point). `skip_exit_sections`
    suppresses the fall-through L_exit AND the deferred Lback section (both
    dead — nothing falls into them; the last body op falls straight into the
    inline jmp). `emit_flush_all_pins` was hoisted OUT of the L_exit block
    because the cold exits use it too.
  - **Linear case** (`b target != head`): the region just falls through into
    the L_exit whose `exit_pc` is set to the branch target
    (`rblocks[nblk-1].side_pc`, not pc+inst_count*4); the JCC patch loop skips
    the last block (`last_term_uncond`, no JCC exists).
  - Validation allows BRCOND_FALLTHRU ONLY at `i == nblk-1` (the walker ends
    the trace at every `b`, so a mid-trace one is impossible — reject
    defensively). The rblocks term scan (line ~646) recognizes it too.
- **CRITICAL: the back-edge repatch in `tier2_fire_region` must rewrite the
  MAIN chain slot, not just the taken slot.** BRCOND_FALLTHRU records
  `chain_target_pc_` and uses the epilogue's MAIN chain slot
  (`chain_patch_off`) for its jump — `has_taken_chain_slot` is false, so the
  old force-patch skipped the back-edge block entirely and the loop kept
  jumping to the OLD head fn forever (the region compiled but never ran →
  tier2 measured NEUTRAL on the new shape). Fix: the fire loop now uses
  `taken_chain_patch_off` when `has_taken_chain_slot`, else
  `chain_patch_off` when the block's term is BRCOND_FALLTHRU, else skips
  (try_chain_block later chains to `blocks_[pc].fn` = the region). The main
  slot of a COND block must NOT be repatched (it is the fall-through into the
  next trace block).
- **Measured (m3loop.S, /tmp/opencode, 536,887,296 iters, test-at-top +
  `b .Lloop` bottom): tier2 OFF 3.71s → M2 **2.25s ≈ 39% faster** (the
  region replaces 2 prologues + 2 epilogues + 1 dispatcher round-trip per
  iteration with 1 inline jmp). m2loop2/3/4 (cond back-edge) unchanged
  (m2loop4 5.05-5.09s). Region: 453 B, blocks=2 insts=12, back_flags_carried=0.
- **Verified:** m3loop/m3loop_quick acc `0x00100bf1c2ed524b` /
  `0x00100b214f490000` byte-identical across interp/JIT/tier2 (full-run interp
  is too slow — compare JIT vs tier2 on the full n, interp on the _quick
  n=131072 variant); m4lin.S linear-uncond smoke (`b .Lskip` cond + `b
  .Lexit2` uncond last block) MATCH exit 0; game_demo passes tier2 ON;
  m2loop2_quick/m2loop4_quick tri-mode MATCH; `BIFROST_JIT_VERIFY=1`+
  tier2 zero divergence lines on m3loop_quick/m2loop4_quick; bench_sort
  {0x4024a4,0x40533c,0x408148} / bench_matrix {0x405ec8} / bench_mips clean —
  IDENTICAL to baseline. Quick suite **200/200** with tier2 OFF and ON.
- Still unsupported (future work): RET-ending regions (a `br x30` term,
  IROp::BR, would need pc=x30 load + exit), and BL/BLR inside traces (the
  worldgen noise path — needs call-aware regions). Uncommitted — tree is at
  HEAD `85a09e9` + M2 + this diff.

## Session History (2026-08-20) — self-loop regions: preheader liveness + chain repatch fixes

- **Self-loop region fusion (the whole Phase-2 goal): the head block of a
  tight self-loop is now fused into a tier-2 region with a LICM preheader.**
  The in-code hot-head counter counts CHAINED execution (the dispatch-side
  counters never see real hot loops — see the Phase-1 finding), so a
  self-loop head fires, `collect_tier2_trace` walks it, and the region
  compiles with the loop's back-edge as an inline Lback. m5self/m5big
  harness (synthetic raw-asm loops, /tmp/opencode/m5*.S): the region runs
  and **m5big is 38% faster (1.553s → 0.961s JIT vs tier2)** with a
  byte-identical acc.
- **Region reachability REQUIRES the `back_refs_` chain repatch.** A region
  replaces `blocks_[head]`, but the head is reached via chain slots in
  OTHER blocks (`jmp old-head-fn`) or via a BL_CALL direct `call rel32`
  (frostjit.cpp:846) — neither goes through the dispatcher, so cache
  invalidation alone leaves the region dead (compiled, never entered).
  `tier2_fire_region` now repatches (a) every trace block whose side-exit
  targets the head (the back-edge, via its taken slot, or the MAIN slot for
  BRCOND_FALLTHRU) and (b) every block in `back_refs_[head]` — blocks whose
  chain/taken slot targets the head — to `jmp region-fn` (same RDI=cpu/
  RSI=emu entry contract the taken-path epilogue sets). This is what makes
  the region reachable at all.
- **CRITICAL: the back_refs_ chain repatch MUST select the slot by the
  recorded target, not by `has_taken_chain_slot`.** back_refs_[T] conflates
  referrers whose MAIN slot targets T with referrers whose TAKEN slot
  targets T (jit_translate.cpp registers both). The old heuristic
  (`has_taken_chain_slot ? taken_chain_patch_off : chain_patch_off`)
  hijacked the WRONG edge for a block whose fall-through targets the head
  but which also has a taken slot for a DIFFERENT target: block 0x40064c
  (`b.eq 0x688`, MAIN→0x658=head, TAKEN→0x688) in test_game_demo got its
  TAKEN (loop-EXIT) slot repatched into the region, scrambling the
  standalone-block graph so the hot loop bounced through the region
  prologue once per iteration — **2.1e9 region entries = 56s vs 2.2s** (the
  temporary region-entry lock counter made it worse; the bounce was real).
  Fix (mirror try_chain_block, jit_cache.cpp): repatch `chain_patch_off`
  when `chain_target_pc == pc`, `taken_chain_patch_off` only when
  `taken_chain_target_pc == pc`; both if both edges target the head. Also
  added `BlockEntry.is_region` and skip repatching any block that is now a
  DIFFERENT region's head (overlapping traces share blocks — regions
  contain the same head block; repatching a region head's stale standalone
  slots cross-wires regions). game_demo: **56s → 2.2s** (parity with JIT),
  region stays correct.
- **LICM preheader liveness bug (m5 corruption):** the preheader compiled
  with `cur_op_index_ = fold_ahead_kind_.size()` (out of range, so hoisted
  IMMs never skip their mov) made EVERY preheader vreg look dead to the
  Belady allocator (`next_use_after` returned -1 for all), so `alloc_reg`'s
  eviction picked the FIRST allocable reg — RAX — clobbering a just-loaded
  operand mid-expression (H9's `v44 = v41 + v43` loaded v41 into RAX, then
  v43 into RAX → `add rax,rax` = 2*v43; m5 acc came out 0x33333333 short
  per call). Fix: inject SYNTHETIC preheader use positions
  (`PREHEADER_BASE + k`, 40000 + k — fits uint16_t) into `vreg_uses_` for
  each preheader op's sources (src1/src2/aux, v>32) BEFORE the preheader
  loop, and compile preheader op k with `cur_op_index_ = PREHEADER_BASE +
  k`. Fold-ahead stays off (its bounds check `cur_op_index_ <
  fold_ahead_kind_.size()` is false for the synthetic cursor); body
  liveness unaffected (body indices all < PREHEADER_BASE, so the body's
  next_use_after still returns the correct next body use; preheader-defined
  vregs with only preheader uses are never mapped in the body). v47 (used
  in the body) is evicted right after its preheader def — its slot holds
  the value and the body reloads it — correct, one cold spill. Do NOT
  revert the preheader to compile with the body index; that is the bug.
- **Verification:** game_demo rc=0 at 2.2s tier2 ON (suite 15s timeout
  passes); m5one/m5n/m5big/m3loop/m4lin/m2loop2/3/4 tri-mode byte-identical
  (interp on the _quick variants); bench_mips acc `0xf800800a2c4ff835`
  unchanged both modes; JIT_VERIFY+tier2 zero divergences on
  m5n/m5big/m3loop_quick/m2loop4_quick; bench_sort clean, bench_matrix
  {0x405b98, 0x405ec8} identical to tier2-OFF baseline; quick suites
  **200/200 tier2 OFF and ON**. Temporary diagnostics (region-entry lock
  counter, `BIFROST_T2_DUMP` region bytes dump, `tier2_region_entries`
  stats) REMOVED; the gated `[tier2]` trace lines and `back_refs_` repatch
  remain. Uncommitted — tree at HEAD `85a09e9` + M2/M2b + this work.

## Session History (2026-08-20) — Track 1: call-aware regions (BL/BLR in traces)

- **Call-aware regions landed (plan.md Track 1, uncommitted at `2c04f77` +
  this diff, confined to `src/jit/jit_tier2.cpp` +85/−21).** The walker no
  longer aborts on calls: **BL** is fused when the callee is ALREADY
  translated (`lookup_only(ip + d.imm) != nullptr` — the direct-call slot
  patches immediately; an untranslated callee would bounce jit_call_helper →
  INCOMPLETE unwind → dispatcher every iteration, worse than no region →
  aborts as `bl_untranslated`); **BLR** is allowed UNCONDITIONALLY (dynamic
  target, no pre-translation check possible — BLR_CALL codegen routes through
  jit_call_helper which runs the whole callee and returns the continuation;
  the worldgen noise path is blr-heavy). Both translate to BL_CALL/BLR_CALL
  body ops (ir_translate.cpp:656-698 — they do NOT end the block), the trace
  continues at ip+4 (the caller's continuation stays INLINE in the region),
  and the callee is never part of the trace. `BIFROST_NO_CALLREGION=1`
  restores the M1 aborts. BR/SVC/CALL_INTERP still abort.
- **Why the region frame tolerates calls (verified by reading sources, not
  the summary):** the region prologue is byte-identical to a standalone
  block's (push rbx/rbp/r12-r15, mov rbp,rsp, sub rsp,stack_bytes with
  stack_bytes%16==0) so body-entry RSP%16==8 and BL_CALL's alignment sequence
  (push WIN_REG; sub rsp,8; pushfq; call) is valid; the completion guard's
  INCOMPLETE path (`mov rsp,rbp; pop×6; ret`) correctly unwinds the REGION
  frame to the C dispatcher; `emu_slot_off()` = -8*(num_stack_slots_+1) is
  set up by the region (num_stack_slots_ = region_max_vreg - 32, +64-byte
  cushion). `optimize_ir` (run per walker block with force_fwd) is already
  call-safe: BL_CALL/BLR_CALL clear arm_reg_cache (ir_optimize.cpp ~675),
  invalidate consts (~102), and DSE preserves preceding STORE_REGs incl. the
  x30 link store (~301, special case ~310). The head block must be CALL-FREE
  for the in-code counter (tier2_head_eligible excludes BL_CALL/BLR_CALL), so
  real call regions are ≥2 blocks with the call in a LATER block.
- **LICM arch-load hoisting gated on `!has_call`:** a region-wide
  `const bool has_call` (scan for SVC/BR/BL_CALL/BLR_CALL/CALL_INTERP over
  region_ir) is computed once after concatenation and consulted by BOTH the
  pin scan (replacing its local scan — pins stay disabled for call regions:
  a callee can write any cpu.regs[] entry, so a deferred pin could go stale)
  and `src_invariant`'s LOAD_REG branch (`return !arch_written[s] &&
  !has_call`). Pure scratch IMM/ALU chains remain hoistable — they live in
  the region's own frame slots, which the callee (its own frame) never
  touches. Do NOT re-enable arch-load hoisting under has_call even for
  "never-written-in-region" archs — the callee writes them mid-loop.
- **New harnesses `/tmp/opencode/m6bl.S` / `m6blr.S` (+ .elf):** 2-block
  natural loop (head `subs;b.eq` call-free → fires; body `add;bl/blr
  .leaf;add;b .Lhead`) calling a leaf via BL / function-pointer BLR. Trace
  log confirms fusion: `blocks=2 insts=6 ok=1 stop=b_backedge`, region 457 B
  (BL, direct call+guard) / 387 B (BLR, helper), back-edge + chain-in
  repatched. m5one's `bl` is in the OUTER loop (outside the traced self-loop)
  so it does NOT exercise this path — don't reuse it as a call-region test.
- **Verification:** m6bl/m6blr tri-mode byte-identical
  (`0x9999999999999992`, matches closed form acc_{n+1}=6·acc_n+38 ×262144);
  JIT_VERIFY + REGALLOC_CHECK clean on both; all m5/m3/m2 harnesses tri-mode
  MATCH; game_demo rc=0 tier2 ON+OFF; bench_mips acc `0xf800800a2c4ff835`
  both modes + REGALLOC_CHECK; quick suite **200/200** tier2 ON, full suite
  **205/205** default. Micro-harness timing NEUTRAL (~200ms both modes — the
  leaf's own standalone prologue/epilogue dominates a 6-inst loop; the payoff
  question is worldgen's fat noise loops → plan.md Track 0 minecraft A/B).
- Not committed alongside: `.gitignore` (+rules.md personal file) and
  untracked `plan.md` (Track 0-5 roadmap + do-not-regress list).

## Session History (2026-08-20) — Track 1 committed + minecraft A/B methodology

- **Track 1 COMMITTED as `054a4d5`** ("tier2: call-aware regions — fuse
  BL/BLR into traces (Track 1)"; jit_tier2.cpp + AGENTS.md, +142/−21). The
  commit message carries the full summary; the contract is in the Session
  History entry above. Left uncommitted on purpose: `.gitignore`
  (+rules.md personal file) and untracked `plan.md`.
- **Minecraft tier2 A/B measured — PARITY, with three hard-won methodology
  lessons (full protocol in plan.md Track 0):**
  1. The game's whole loop (ticks + chunkgen) is chained to HOST VSYNC —
     run with `__GL_SYNC_TO_VBLANK=0` or you measure the monitor.
  2. **Chunk-generation counts measure PLAYER INPUT**: `[DBG-GEN] done`
     lines follow the player (still → ~91-chunk ring; walking → hundreds).
     An interactive user on DISPLAY=:0 poisons every run silently (produced
     a fake 485-chunk outlier and a fake −13% "regression" that was briefly
     misdiagnosed as in-code-counter tax — retracted).
  3. `BIFROST_STATS_PERIOD` prints only when the main run loop spins; this
     game blocks it in GL/thunk calls most of each frame → ONE dump per run,
     `0.0 MIPS` (huge dt). Not a usable meter here.
  Clean protocol: host-timestamp every output line, time
  `GAME-ENTER-LOOP` → chunk #80 `[DBG-GEN] done` (inside the no-input ring),
  hands off input for the first ~20 s. Result: OFF avg 2.81 s / ON avg
  2.80 s (2 reps each) = parity at ±7% resolution. tier2 stays opt-in.
- Harness note: m5one/m5n/m5big's `bl .inner` is in the OUTER loop, outside
  the traced self-loop — they do NOT exercise call regions. Use
  `/tmp/opencode/m6bl.S`/`m6blr.S` (2-block loop, BL/BLR inside block 2,
  head call-free so the counter fires; acc `0x9999999999999992` matches the
  closed form acc_{n+1}=6·acc_n+38 ×262144).

## Session History (2026-08-20) — tier2 counter "optimization": measured, rejected

- **Wrap-based 2-instruction hot counter REJECTED after measurement — do NOT
  re-attempt without a µop-level argument.** Hypothesis: the tier-2 in-code
  counter (`inc dword; cmp imm32; jne`, jit_translate.cpp ~838) looks like 3
  instructions, so a `inc byte[rip+bctr]; jne` wrap form (slow path every
  256th entry bumps a dword wrap-counter + `cmp ceil(threshold/256)`) should
  halve the per-entry tax. Measured on m3loop (536M head entries,
  `BIFROST_TIER2_HITS=99999999` so nothing ever fires/neutralizes), old vs
  new via git-stash A/B: OLD tax ≈162 ms (3880−3718), NEW tax ≈195 ms
  (3915−3719) — the new form is ~20% WORSE plus 8 extra code bytes.
  Root cause: x86 MACRO-FUSION — cmp/jne fuses to ONE µop, so the old
  sequence was already ~2 µops like the new one; the byte-store form adds a
  store-forwarding quirk for nothing. Also fixed en route: `FE /0` is the
  BYTE inc, `FF /0` is DWORD (a FF-encoded "byte" counter never sets ZF and
  never fires — caught by disassembling BIFROST_JIT_DUMP output). Change
  reverted (`git checkout --`); tree at HEAD `ebd2764`. The exact-form
  requirement for tiny thresholds (HITS≤255) is moot with the revert.

## Session History (2026-08-20) — Track 4: regions compose with chain-skip

- **Regions now run under `BIFROST_CHAIN_SKIP=1` (committed `b84cc22`, 4
  files +91/−20).** compile_tier2_region: allocates the unified
  `kChainSkipFrameBytes` frame under chain-skip (vreg ceiling −32512 <
  32 KiB, so slot layouts are identical in any chain root's frame), records
  a region chain_entry label AFTER the emu stash and BEFORE the R10 window
  load / pin preloads / preheader (mirroring translate_block's
  chain_entry_off_), and publishes it via a new `chain_out` out-param
  (frostjit.hpp declaration + all THREE registration sites: tier2_fire_region,
  lookup_call_target frostjit.hpp ~217, run_block slow path jit_dispatch.cpp
  ~191 — each sets `region_entry.chain_entry` when `chain_skip_enabled() &&
  chain_fn`). Chain edges are bare jmps carrying the ROOT's rbp/rsp/rbx, so
  they MUST enter past the frame allocation; cold dispatches enter at fn.
  Region exits keep `mov rsp,rbp; pop×6; ret` — with rbp = the root frame
  they unwind the whole chain to the dispatcher, same as a chained block's
  cold exit. tier2_fire_region's two repatch loops (trace back-edges +
  back_refs_ chain-ins) target `enter_target` = chain_fn under chain-skip,
  rfn otherwise. BL_CALL's INCOMPLETE unwind inside a region is safe for the
  same reason (unwinds whatever frame rbp names).
- **CRITICAL: call-containing regions still DECLINE under chain-skip**
  (`if (chain_skip_enabled() && has_call) return nullptr;` right after the
  has_call computation). Reason: under chain-skip a chain-entered region's
  rbp is the CHAIN ROOT's frame, so the BL_CALL completion guard's
  INCOMPLETE path (`mov rsp,rbp; pop×6; ret`) would discard the live host
  return addresses — the exact documented busybox do_wait hang that forced
  `bl_call_disabled_ = chain_skip_enabled()` (jit_translate.cpp:283-292).
  Guest BL cannot appear in traces under chain-skip anyway (it lowers to a
  block-ending BRCOND_FALLTHRU there), but BLR_CALL could — this guard
  covers it. Do NOT re-enable without redesigning the unwind contract.
- **LANDMINE (caused an ASLR-flaky SIGSEGV mid-surgery): the 8-byte counter
  data reservation at jit_translate.cpp ~351 carried its own
  `!chain_skip_enabled()` gate.** Opening only the counter-emission gate
  made counters RIP-relative-increment the PREVIOUS block's last 8 code
  bytes → corrupted code → crashes that vanished under gdb (ASLR off) and
  moved with env size. Symptom pair to remember: crash ONLY with
  TIER2+CHAIN_SKIP together + normal-looking single runs + gdb-clean. Fix:
  reservation gate is now `tier2_enabled() && !wex_enabled_` only.
- **Walker trace shape differs under chain-skip for call loops:** m6bl's
  trace becomes 3 blocks with a side_exits=0 middle block (the BL ended the
  block as BRCOND_FALLTHRU) — the compiler's `side_exits != 1` validation
  rejects it, so BL-heavy loops simply don't fuse under chain-skip (correct,
  no fusion). Cond-branch loops (m5/m3/m2 shapes) fuse fine.
- **Measured: composition is CORRECT but PERF-NEUTRAL** — a region's
  internal Lback already subsumes what chain-skip saves inside fused loops
  (m5big: chain-skip 1584ms / tier2 966ms / both 968ms; m3loop: 2760 /
  1863 / 1853ms). The win is flag compatibility + ground for future work
  (region cold-exits chaining OUT lease-style instead of reting to the
  dispatcher — not implemented).
- Verified: 11 harnesses × {jit, tier2, chain-skip, both} byte-identical;
  JIT_VERIFY+tier2+chain-skip clean on m5big/m3loop_quick/m2loop4_quick;
  bench_mips acc `0xf800800a2c4ff835` in all 4 modes; game_demo rc=0 ×4;
  quick suite 200/200 under both flags; full suite 205/205 default.
  m3loop_quick fires NO regions in either mode (pre-existing quirk, parity
  confirmed) — use full m3loop for back-edge-region fire testing.

## Session History (2026-08-20) — loop regions only (CoreMark +3.5%); Track 2 superseded

- **LINEAR REGIONS DECLINED BY DEFAULT (committed `0c518c5`).** compile_
  tier2_region now returns nullptr unless `last_is_backedge` (the trace is a
  real loop); `BIFROST_T2_LINEAR=1` restores all-region behavior. Reason: a
  linear region over a walked-out loop body re-pays a full region entry
  (prologue + cold-exit ret) EVERY iteration — strictly worse than the
  chained standalone blocks it replaces. Measured on CoreMark (guest binary
  at `/home/gamingpc/Downloads/reviewing:/bifrost-emu-1.4.5-alpha/coremark/
  coremark.exe`): plain **3479** iters/s, tier2 all-regions **3239** (−7%),
  tier2 loops-only **3602** (+3.5%), "Correct operation validated". Genuine
  loops are unaffected: a back-edge that is a TAKEN conditional or an
  unconditional `b head` ends the trace with stop=b_backedge and still
  fuses (m3loop/m5/m6 shapes all verified). Only truly linear runs — and
  walks that exit a loop through an OUTER edge — are declined.
- **Counter tax quantified on billion-entry workloads:** CoreMark with
  `BIFROST_TIER2_HITS=99999999` (counters never neutralize) drops to
  **894 iters/s** (~4x slower than baseline). The per-entry inc/cmp/jne
  toll across ALL M1-eligible blocks is enormous when never fired;
  neutralization-after-fire is what makes the default viable. HITS=1000 ≈
  default (3264 vs 3239 pre-loops-only). Do NOT ship tier2 defaults that
  leave large numbers of hot blocks un-neutralized.
- **Track 2 (cross-block pinning) SUPERSEDED, not implemented.** The premise
  ("pins can't cover CoreMark's 2-block loop") is stale: the pc-hist shows
  this build's hot loops at 0x400f7c/0x401020/0x400f58/0x401f84/0x401de0/
  0x402848 (NOT the old 0x401e90/0x401ea4 pair), and tier2 already fuses
  them — e.g. an 11-block/45-inst region at 0x400f7c. A region's single
  regalloc pass carries registers across blocks strictly better than pins
  could. Cross-block pinning remains relevant ONLY for loops regions can't
  capture (side-exit-heavy shapes); revisit if such a workload appears.
- Profiling recipe that found all this: `BIFROST_PROF=1 BIFROST_PC_HIST=1
  BIFROST_STATS_PERIOD=5` prints a sampled guest-PC histogram (top 20) —
  use it to find REAL hot PCs before assuming old ones; block starts may
  sit a few bytes before the sampled PCs (samples land mid-body).
- Verified for `0c518c5`: harnesses tri-mode byte-identical (m4lin now
  forms no region — falls back to chained blocks, acc unchanged),
  JIT_VERIFY clean on m5big/m3loop_quick, game_demo rc=0, bench_mips acc
  `0xf800800a2c4ff835`, quick 200/200 tier2 ON, full suite 205/205.

## Session History (2026-08-20) — Track 3: shared exit tail (cold-exit dedup)

- **Shared exit tail landed (committed `10b0b4d`).** Every region cold exit
  (L_exit + one deferred epilogue per non-last block) ended with the same
  ~30-byte fixed sequence: `mov [rbx+PC_OFF], rax; mov rdi,rbx; mov
  rsi,[rbp+emu_slot]; mov rsp,rbp; pop x6; ret`. Each exit now ends with
  `movabs rax, exit_pc` + `jmp rel32` to ONE shared tail emitted after the
  deferred-exit loop (patched via `shared_exit_patch_offs`); only the
  per-edge VARIABLE part stays inline — cmc / flag materialize /
  flush_all_vregs of that branch point's dirty vregs / emit_flush_all_pins.
  The variable part CANNOT be shared without runtime metadata (each branch
  point has a different live set). Lback unchanged (no tail). Skipped when
  no exits exist (1-block uncond-back-edge region).
- **Measured (CoreMark regions, git-stash A/B):** 9-block/32-inst region
  2995 → 2818 B, 6-block 2344 → 2244, 5-blocks ~−100 each (~5%); savings =
  (N_exits−1) × (tail−5 B). Single-exit regions (self-loops,
  skip_exit_sections shapes like m3loop's uncond back-edge) pay exactly
  +5 B — dedup needs ≥2 exits. Harness regions are tiny (1 exit each), so
  CoreMark is the place to see the effect.
- **MEASUREMENT TRAP:** harness region-byte A/Bs mislead — m5big/m3loop
  showed +5 B and looked like a regression until the stash A/B revealed
  both have only ONE exit. Always compare on a many-exit region.
- Verified `10b0b4d`: harness tri-mode byte-identical, JIT_VERIFY clean,
  CoreMark 3613 iters/s CRCs validated (loops-only baseline 3602), m5big
  963 ms / m3loop 1853 ms hold, game_demo rc=0, bench_mips acc
  `0xf800800a2c4ff835`, quick 200/200 tier2 ON, full suite 205/205.

## Session History (2026-08-20) — Track 5 re-measured: closed without changes

- **Track 5 (LICM/pin refinement) CLOSED — premises stale, no code change.**
  Re-measurement on the current tree (post loops-only + shared-tail):
  - **LICM is strongly POSITIVE now**: m2loop3 LICM 3878ms vs NO_LICM
    4688ms (+17%), m2loop4 3873 vs 4624 (+16%). The old ~1% m2loop3
    regression (hoisted bare LOAD_REG reading a slot instead of the cheap
    pin) is GONE — cured by the arch_pin_-gating fix + refine pass +
    loops-only default. Tier2 beats plain jit 1.6-2x on all three m2loops.
  - **Pins earn their keep**: m3loop PIN 1853ms vs NO_PIN 1963ms (+6%),
    CoreMark 3604 vs 3550 iters/s (+1.5%), m5big neutral.
  - Pin expansion 4→6 (R9/R11) REJECTED without experiment: removes two
    regs from the allocatable pool; the closest historical measurement
    (written-then-read pinning) was ~6% SLOWER on bench_mips. Risk > reward.
  - Register-resident invariant chains: LICM's slot-based preheader already
    wins big; complexity unjustified by any measured deficiency.
  Lesson: re-measure old bruises before operating — several earlier fixes
  had already healed this one.

## Session History (2026-08-20) — review pass: arch-only exit flush measured ~zero, reverted

- **"Dead scratch stores at region exits" investigated and CLOSED as a
  non-issue (`git checkout --`, nothing committed).** Hypothesis: region
  cold exits call `flush_all_vregs()`, which spills dirty scratch vregs
  (v>32) to stack slots nobody reads after the exit — dead bytes + stores.
  Measured (CoreMark, git-stash A/B): TOTAL region bytes across all ~61
  regions changed by **4 bytes** (29898 → 29894); top fat regions
  byte-identical; CoreMark iters/s within noise. Root cause of the null
  result: at branch points there IS no dirty scratch in practice — the
  Belady allocator evicts scratch to slots aggressively (clean loads at
  next use), optimize_ir's FWD store-load forwarding drains expression
  chains into eager arch STORE_REGs, and a term's branch operand is loaded
  fresh by the term's own flag-prep (ensure_vreg from slot). So
  `flush_all_vregs()` at exits was already near-optimal. Corollary: if a
  future change increases REGISTER RESIDENCY at exits (e.g. keeping LICM
  results in regs across the Lback), re-measure this before assuming the
  flush is free. Also verified en route: `emit_mov_imm_to_rax` already
  emits the 5-byte zext form for guest PCs <4 GiB (no win there either).
- Review-pass ideas still open: register-resident LICM results across the
  Lback (P2 — medium risk, LOAD_MEM-starvation history), IRBlock caching
  in BlockEntry to shorten exclusive-lock fire pauses (P3).

## Session History (2026-08-20) — review pass: P2 attempted, LATENT ALU-EMITTER BUG found instead

- **P2 (register-resident LICM results) REVERTED — but the attempt exposed a
  REAL latent miscompile in the ALU emit path. Do NOT re-attempt P2 until
  the bug below is fixed.**
- **THE BUG (disassembly-confirmed, m3loop region preheader):** with the
  preheader allocator steered away from FLAGS3 (temporary
  `pinned_host_regs_ |= FLAGS3` experiment), a hoisted `SHL` result landed
  in R11; the following `ADD`'s emitter then emitted
  `mov %r9,%r11 ; add %r11,%r11` — ensure_vreg(src1) EVICTED src2 (Belady:
  src2 dead-after-this-op → legal eviction) and reused its register R11 for
  src1, while the emitter's cached "src2 is in r11" reg number went STALE.
  Result: add reads src1 twice. Silent wrong answer whenever allocation
  produces dest/src2 collision with a stale cached operand reg. The
  standard pattern (`d = alloc_reg_excluding(s1,s2)` after BOTH ensures)
  should prevent this — some op(s) in jit_codegen_alu.cpp capture an
  operand's reg, then run an ensure/alloc that can evict it, then use the
  stale number. AUDIT REQUIRED: every compile_ir_inst ALU path's
  ensure_vreg/alloc ordering (ADD/SUB/AND/OR/XOR/SHL/SHR/SAR/ROR at minimum;
  likely copy-pasted). Triggered tonight ONLY because the FLAGS3 exclusion
  changed allocation; any future allocator-pressure change can hit it.
- **P2 mechanics that DID work (for the retry):** keep-candidate selection
  (hoisted dests with real body uses, term-safe home reg ∉ FLAGS3, not arch
  pins), post-flush remap + pinned_host_regs_ protection + invalidate-tail
  replication preserving keeps and re-establishing pins dirty. All verified
  correct when keeps were empty. What does NOT work: steering preheader
  allocation via the pin mask (triggers the bug above), and opportunistic
  keeps without steering (roots always land in FLAGS3 first — ALLOC_REGS
  order — so zero keeps fire). Fix the emitter bug first; then P2 = the
  FLAGS3-exclusion variant, which was byte-diffable via a temporary
  BIFROST_T2_REGIONDUMP hexdump of the published region.
- Verified revert: m3loop tri-mode MATCH restored, tree clean at HEAD.

## Session History (2026-08-20) — emitter bug variant-1 FIXED (alloc_reg_excluding fallback)

- **Variant-1 of the stale-operand-reg bug FIXED (committed `afab35d`,
  x86_regalloc.cpp + jit_codegen_alu.cpp).** `alloc_reg_excluding`'s
  terminal fallback (`return alloc_reg()` when every non-pinned reg is
  excluded) silently violated the exclusions: alloc_reg could evict an
  excluded still-live operand and hand its register to the caller as d,
  after which the caller's canonical `mov d, excl1 ; op d, excl2` read
  excl2 from the clobbered reg. Fix: desperate pass re-runs Belady
  excluding ONLY excl2 (+pinned), evicting the winner (value preserved to
  slot/home), returning it; last resort is excl1's own reg. This is
  correct by caller contract: excl1 is everywhere the IN-PLACE-SAFE
  operand (its mov degenerates to no-op; later uses reload from the fresh
  slot). Audited all 6 call sites against the contract; swapped
  jit_codegen_alu.cpp:223 to (-1, RCX) (that site's tmp must AVOID RCX,
  so RCX belongs in excl2 — behavior identical non-desperate).
- **Variant-2 remains OPEN but unreachable in production:** with a ≤2-reg
  pool, `ensure_vreg(src2)` can still evict an already-ensured src1
  (Belady: dead-after-op) and the emitters' cached s1 number goes stale —
  observed as `xor %r9,%r11` reading the wrong operand after EOR's ensure
  chain. Production pools never exhaust (9 alloc regs, ≤2 exclusions);
  ANY future feature steering allocation into small pools MUST fence
  itself to ≥3 remaining non-pinned regs (fence pattern demonstrated in
  the P2 attempt, since removed with it). Proper fix design (memory-
  operand last resort, or allocator op-awareness) is future work.
- The m3loop corruption repro now emits CORRECT code under the original
  trigger (FLAGS3-excluded preheader): p4's ADD computes in place into
  R9 after spilling v46 — verified instruction-by-instruction.
- Verified `afab35d`: 11 harnesses tri-mode MATCH, JIT_VERIFY clean,
  REGALLOC_CHECK bench_mips acc `0xf800800a2c4ff835`, game_demo rc=0,
  CoreMark 3750 iters/s CRCs validated, quick 200/200 tier2 ON, full
  suite 205/205.

## Session History (2026-08-20) — Android surface layer Phase 1 (ANativeWindow + EGL)

- **ANativeWindow shim + EGL window-surface interception landed (committed
  `9f3d5cd`, 11 files +652/−19).** Guest apps dlopen("libandroid.so"),
  get an ANativeWindow shim from ANativeWindow_fromSurface (JNIEnv/jobject
  ignored — v1 single-surface singleton), and eglCreateWindowSurface
  through it creates a REAL host EGL window-surface: pixels present via
  the DisplayProxy host SDL window. Test:
  `ctest_real/test_android_surface.elf` (24 checks, "ALL PASS", exit 77
  skip without DISPLAY/GL; 8/8 stable). Suite now **206/206**.
- **Architecture:** new ANDROID family (libandroid.so) registered under
  DISPLAY_THUNK (it owns the DisplayProxy; GraphicThunk never sees it).
  8 ANativeWindow_* rows with new Policy::ANDROID_WINDOW +
  THUNK_ANDROID_WINDOW flag; dispatch arm lazily inits the proxy and
  handles each symbol by name. Window handles are GUEST shim integers
  (0xA90000000000+1) — declared 'i' in the table so they are NEVER
  pointer-translated. AndroidSurfaceManager singleton
  (src/frost_graphics/android_surface.cpp): geometry/format state, host
  native window resolution — X11 Window id on X11 hosts,
  wl_egl_window* via libwayland-egl on Wayland hosts.
- **GraphicThunk EGL interceptions** (consult the manager singleton):
  eglGetDisplay(EGL_DEFAULT_DISPLAY) wraps SDL's wl_display on Wayland
  hosts — the surface's wl_surface and the EGLDisplay MUST share a
  connection or Mesa rejects creation cross-connection;
  eglCreateWindowSurface substitutes the host native window for the shim
  handle. eglSwapBuffers needs no interception.
- **EGL ROW SIGNATURES WERE WRONG — fixed:** eglInitialize was `iii`
  (major/minor are out-pointers!), eglChooseConfig was `iiiii`
  (attribs/configs/num_config are pointers; config_size is the ONLY int
  besides dpy → correct row is `ippip`), eglQuerySurface/
  GetConfigAttrib were `iiii` (value out-pointer → `iiip`),
  eglCreateContext/CreateWindowSurface attrib_list → `iiip`,
  CreatePbufferSurface → `iip`, eglQueryString RET str. With 'i'
  pointer args, host Mesa dereferenced RAW guest addresses → SIGSEGV
  (the guest direct window is NOT identity-mapped in the host address
  space; only thunk-translated aliases are valid host pointers).
  No existing test called guest-side EGL directly, which is why this
  survived — test_sdl_gl_triangle uses SDL_GL_CreateContext instead.
- **EGL spec gotcha that cost an hour:** eglChooseConfig's *num_config
  receives the TOTAL matching-config count (96), NOT min(count,
  config_size=4) — a `ncfg <= config_size` sanity check rejects valid
  answers. Also Mesa refuses REPEATED eglChooseConfig calls on a foreign
  wl_display with EGL_BAD_CONFIG — the first query must be accepted.
- **Wayland connection identity:** host EGL on Wayland consumes a
  wl_egl_window* (libwayland-egl, created from the SDL window's
  wl_surface), not the raw wl_surface; set_buffers_geometry resizes it
  via wl_egl_window_resize. X11 path returns the X11 Window id.
- **Phase 2 (not started): NativeActivity lifecycle** — synthesize
  ANativeActivity in guest memory, fire onStart/onResume/INIT_WINDOW via
  the borrow-CPU callback runner so android_native_app_glue mains proceed.
  **Phase 3: touch** — SDL mouse → AInputQueue MotionEvents (input.cpp
  has no multitouch yet; SDL2 has it unplumbed).

## Session History (2026-08-20) — Neverball (Debian arm64) boots and runs

- **Neverball 1.6.0 (Debian bookworm arm64 .debs) RUNS under bifrost-emu** —
  SDL2 window + GL context via thunks, menu loop stable 45s+ at ~27% CPU,
  zero SIGSEGV/DecodeError. Suite quick **201/201** after all changes.
  Run recipe:
  ```
  DISPLAY=:0 BIFROST_ROOT=$PWD/rootfs \
      LD_LIBRARY_PATH=$PWD/rootfs/usr/lib/aarch64-linux-gnu \
      ./bifrost-emu rootfs/usr/games/neverball
  ```
- **Rootfs additions** (extracted from debs into rootfs/, Debian pool
  main/<src>/ paths — `lib*` sources live under `main/lib<x>/`):
  neverball + neverball-data + **neverball-common** (themes/gui live in
  common, NOT data — "Failure to open classic theme file" means it's
  missing), libsdl2-image, libsdl2-ttf, libvorbis{,file}, libogg,
  libtiff6, libjbig0, libLerc4, libdeflate0, libwebp{,demux,mux},
  libopenhmd0, libhidapi-libusb0, libusb-1.0-0. hidapi source package is
  `hidapi` (main/h/hidapi), NOT libhidapi; deb.debian.org pool dir
  listings are flaky — packages.debian.org/trixie/arm64/<pkg>/download
  gives authoritative mirror URLs.
- **rootfs/usr/lib/disabled-mesa/**: the rootfs shipped a full AArch64
  Mesa stack (libGL/libGLX/libGLdispatch/libEGL/gbm/drm). Per the dynlink
  contract, an on-disk AArch64 lib is mapped as GUEST code and shadows
  the thunk → guest Mesa does DRM ioctls our VFS can't serve. libGL.so.1*
  moved to that subdir (off the search path) so libGL resolves through
  GraphicThunk. Restore if a guest ever needs real guest GL.
- **LD_LIBRARY_PATH is REQUIRED for Debian layout**: find_library searches
  $BIFROST_ROOT/{lib,usr/lib} but NOT usr/lib/aarch64-linux-gnu (the
  Debian multiarch dir). Point LD_LIBRARY_PATH at it (searched AFTER
  BIFROST_ROOT dirs, BEFORE host paths).
- **Thunk table grew 851 → 932 symbols** (`tools/opgen/thunk_dp.txt`,
  `make opgen-thunk`): ~75 rows covering neverball's needs — fixed-function
  GL (glClipPlane/glColor4ub/glLightModel*/glNormalPointer/glTexGeni/
  glPointParameterf/fv/glStringMarkerGREMEDY), SDL math wrappers
  (SDL_{acosf,atan2f,ceilf,cosf,fabs,fabsf,floorf,fmodf,pow,roundf,sinf,
  sqrt,sqrtf,tanf}), SDL mem/str wrappers (SDL_{memcmp,memcpy,memset,
  strcmp,strncmp,strncasecmp,strchr,strrchr,strstr,strlen,strlcpy,strtol,
  strtoll,...}), surface ops (SDL_FillRect/ConvertSurface*/UpperBlit/
  CreateRGBSurface*From/SetSurfaceBlendMode...), misc (EventState,
  JoystickEventState, SetWindowGrab, text input, RWops extras).
  **New Policy::SDL_ALLOC** (thunkgen VALID_POLICY + dispatch arm in
  thunk.cpp): SDL_malloc/calloc/realloc allocate from Memory::mmap_alloc
  (guest direct window → guest-derefable pointers); realloc copies old
  contents using allocations_snapshot() for the size. **SDL_FREE extended**:
  first checks allocations_snapshot() for an mmap'd block (untrack), else
  falls back to string-cache reclaim. Stubs returning 0 (varargs/callback
  traps): SDL_Log (fmt would hit host printf with guest ptrs), SDL_qsort
  (guest compar callback), SDL_sscanf (varargs), SDL_LoadFile_RW (host-alloc
  return), SDL_ShowSimpleMessageBox. If a game needs these properly:
  qsort needs a borrow-CPU callback runner (mirror GLFW *_CB pattern).
- **GET_PROC ARB-suffix fallback** (thunk.cpp): games built against
  GL_ARB_* extensions fetch "glBindBufferARB" while the table registers
  core names. The GET_PROC arm now strips a trailing "ARB" and retries the
  symbol lookup before returning 0. A NULL here = guest calls through a
  zeroed glext function-pointer table = DecodeError pc=0.
- **Interpreter SIMD ops implemented (interp_fp.cpp)** — neverball's real
  AArch64 libs (libpng NEON filters, libjpeg-turbo IDCT/color) exercised
  five unimplemented groups; each was a hard DecodeError before:
  1. **SABDL/UABDL (+2 variants)** sub3_noq {0x0E,0x2E}207000 and
     **SABAL/UABAL (+2)** {...}207400 (bit10 = accumulate). Widening
     absolute difference; png_write_filter_row does uabdl+uabal.
  2. **Integer by-element (vector x indexed element)** — the bits[28:24]=
     01111 space, DISTINCT from three-same 01110. Index bits: H=bit11,
     L=bit21, M=bit20; Rm register = bits[19:16] (M excluded). Index
     formulas verified against the cross assembler: .h[idx] = H:L:M,
     .s[idx] = H:L, .b[idx] = H:L:M:Rm<3>. Opcodes (bits[15:12]):
     0000 MLA(U=1), 0001 MLS(U=1), 0010 SMLAL/UMLAL, 0110 SMLSL/UMLSL,
     1000 MUL(U=0), 1010 SMULL/UMULL, 1100 SQDMULH, 1101 SQRDMULH,
     1110 SQDMULL (all saturating variants U=0 signed). SQDMULH/SQRDMULH
     use __int128 doubling products. TRAPS: MLA/MLS are U=1 NON-widen —
     a naive `!U && !widen` branch sends them to a dead r=0 (silent IDCT
     corruption → wild jumps); Q=0 same-width ops must ZERO v_hi (stale
     upper halves leak into later Q=1 reads).
  3. **ADDHN family CORRECTED + completed**: legacy cases used base
     0x0E204000 which IS correct (see below) but only covered size=00;
     added RADDHN/SUBHN/RSUBHN and sizes 01/10. sub_noq = op & 0xFFE0FC00
     KEEPS size(bits[23:22]) AND bit21(L), so every lane width is its own
     case value: ADDHN/RADDHN {0x0E,0x2E}{20,60,A0}4000, SUBHN/RSUBHN
     {…}6000. esize_in = 1<<(size+1); rounding adds 2^(esize_out*8-1)
     before the high-half shift. glibc strlen's addhn v2.8b = 0x0E204000.
  4. **Saturating narrowing shift-by-immediate**: SQSHRN/UQSHRN (opc6
     100101), SQRSHRN/URQSHRN (100111), SQSHRUN (100001), SQRSHRUN
     (100011) — opc6 = bits[15:10], imm space shares bits[28:24]=01111
     with by-element so this block MUST run first (SQSHRUN's bits[15:12]
     =1000 collides with by-element MUL). shift = esize_src*16 −
     (immh:immb); round adds 2^(shift-1); sat signed/unsigned per variant.
     libjpeg IDCT tail: sqrshrn v1.8b, v1.8h, #2.
  5. **SADDW family widened to UADDW/SSUBW/USUBW**: sub3_noq
     {0x0E,0x2E}{10,30}1000 (bit13 = subtract, bit29 = unsigned).
     libjpeg color conversion: uaddw v4.8h, v6.8h, v0.8b.
- **Debug lessons**: (a) objdump on a raw `.word` file shows ".word" even
  for valid instructions — ALWAYS disassemble in context from the real
  .so at (pc − lib_base); (b) lib bases come from BIFROST_DYNLINK_TRACE=1
  ("'libX' base=0x…"), correlate with the [DECODE] pc from
  BIFROST_DBG_GUARD=1 (which also walks guest FP-chain backtraces);
  (c) DecodeError thrown inside jit_call_helper CANNOT unwind through JIT
  frames (std::terminate, no catch runs) — catch inside the helper loop
  or read the [DECODE] dump instead; (d) pool.debian.org 404s are often
  wrong source-package names, not missing files.
- Known cosmetic issues (non-blocking): glGetString returns the extensions
  string for vendor/renderer/version queries (pre-existing thunk string-
  cache behavior, neverball tolerates it); "Corrupt JPEG data" lines are
  benign libjpeg warnings on some texture files; audio gracefully disabled
  (no host device open). TODO if gameplay needs them: SDL_qsort/sscanf
  native arms, SDL_LoadFile_RW, joystick event delivery.

## Session History (2026-08-21) — csel-cmov-repair: the game assert FIXED

- **Root cause of the minecraft assert at tick ~15 (and the ground
  clipping)**: `emit_load_flags_from_pstate` used **R8 as scratch** for
  the from_sub extraction while the cmov-CSEL emitter (and CCMP,
  BRCOND_SKIP, ADCS/SBCS) flush only FLAGS3 (RAX/RCX/RDX) around the
  loader. FCMP materializes NZCV to pstate (`flags_in_host_=false`), so
  every conditional select after an `fcmp` ran the loader; the CMOVcc
  ELSE-value staged in R8 was physically destroyed while the regalloc
  still mapped it there → every `cset` after an `fcmp` returned 0.
  Diagnosis chain: game printf instrumentation (`_ivec3s2dir` dumping the
  bad vector → `(0,0,0)`; `ray_block` dumping `dir=(0,0,1) step=(0,0,0)`)
  → minimal repro `/tmp/csel_fp.c` (sign() miscompiled under JIT only) →
  `BIFROST_JIT_DUMP` of the block → read the emitted x86: `mov r8d,1`
  (else-value) … `mov r8,rcx; shr r8,27` (loader scratch) … `mov rcx,r8`
  (garbage else). The single-block integer-producer fuzz never caught it
  because SUBS/ADDS/ANDS leave flags IN HOST (no loader runs).
- **Fix**: RDX-only C^from_sub extraction in the loader via
  `X = pstate ^ (pstate << 2); (X >> 29) & 1` — bit 29 of `(pstate << 2)`
  is pstate's bit 27 (from_sub). First attempt used `>> 2` which XORs C
  with **N** instead — caught because musl printf's own fcmp+carry-
  consuming code garbled `%f` digits ("42.250", "1.\0\0\0"); only
  carry-condition tests expose it (the sign test passed despite the
  wrong CF since PL/LE don't read C). Dropped the now-unneeded R8 from
  the flush masks at BRCOND (branch.cpp), tier2 BRCOND (jit_tier2.cpp),
  and FP_CSEL (fparith.cpp); removed a dead `test ecx,1<<27` in the
  loader. Full contract documented in Local Contracts.
- **Regression coverage**: FCMP-producer section in `ctest/jit_csel.c`
  (fcmp→cset,cset→sub sign interleave, cs/cc/hi csel after fcmp incl.
  NaN, csinc/csinv/csneg after f64 fcmp); scratch fuzz
  `/tmp/csel_fp_fuzz.c` (all 14 conds × f32/f64 × NaN/equal/less/
  greater, ARM FP-flag model in C).
- **Verified**: `/tmp/csel_fp.c` byte-identical JIT vs interp; torture +
  integer fuzz JIT==interp (the torture's own 5 `addhi`/`wform` model
  failures are mode-independent — test-side, not emulator); jit_csel
  (with new section) + jit_carry + jit_ccmp pass under
  `BIFROST_JIT_VERIFY=1`; quick suite **201/201**; game 75s run **zero**
  asserts/signals, world tick 4488. **Merge gate MET.**

## Session History (2026-08-21) — Vulkan graphics pipelines + descriptor sets

- **ROADMAP #12 landed**: `vkCreateShaderModule`,
  `vkCreateGraphicsPipelines`, `vkCreatePipelineLayout`,
  `vkCreateDescriptorPool/SetLayout`, `vkAllocateDescriptorSets`,
  `vkUpdateDescriptorSets` all deep-marshal now (seven new policies in
  thunk_dp.txt + thunkgen VALID_POLICY; arms in `vk_dispatch_`).
  `test_vulkan_swapchain.elf` draws a real triangle (72 checks, JIT +
  interp, RADV RX 7600): shader modules from embedded glslc SPIR-V,
  full pipeline state tree, UBO via descriptor set, D32 depth buffer,
  per-image framebuffers/cmdbufs, 3-frame draw loop, full teardown.
- **Two crash bugs found by the test**: (1) descriptor-type misroute in
  the update arm (is_img range included UNIFORM_BUFFER=6 → all info
  pointers nulled → RADV segfault); (2) `vkCmdUpdateBuffer`/
  `vkCmdCopyBuffer` ARGS had six tokens for 5-param functions → pointer
  mask off by one arg → guest .rodata pointer passed verbatim to host
  memcpy (dmesg: segfault at 0x40c038 in libc). Fixed both rows to
  `iiiip`; audited the rest of the vkCmd* family against real
  signatures (all others correct).
- **VkStage discipline learned**: the staging vector's pointers dangle on
  any post-allocation growth — arms with big variable payloads (shader
  code, pipeline trees) MUST `st.buf.reserve()` up front.
- **NEXT for Vulkan**: vkMapMemory guest-window bounce (mirror GL
  MAP_BUFFER + PCWFC persistent-coherent writeback), then real titles.

## Session History (2026-08-21) — vkMapMemory guest-window bounce

- **The Vulkan PCWFC landed**: vkMapMemory/vkUnmapMemory/Flush/
  Invalidate + vkAllocateMemory size tracking, with push-before-submit
  and pull-after-wait coherence (see Local Contracts). The swapchain
  test now writes its UBO through a mapped guest pointer inside the
  direct window (asserted) — 76 checks, JIT + interp, 3 repeat runs
  stable (bounce alloc/free cycles through mmap_alloc/untrack).
  Quick suite 201/201, opgen-thunk-check in sync (932).
- Remaining Vulkan gaps for real titles: sparse bindings, external
  memory, vkCmdBindTransformFeedbackBuffers etc. — all table rows
  already; nested-pointer shapes beyond these are additive arms.

## Session History (2026-08-22) — vkQuake (real Vulkan game) bring-up, PARTIAL

- **Goal: run a real Vulkan game, not the e2e test.** vkQuake 1.33.1
  cross-built AArch64 (glibc-dynamic) at `~/Downloads/vkquake-src/build-aarch64/vkquake`,
  staged at `rootfs/vkquake/{vkquake,id1/pak0.pak}` (shareware pak). Run recipe:
  ```
  cd rootfs/vkquake && DISPLAY=:0 BIFROST_ROOT=<repo>/rootfs \
      LD_LIBRARY_PATH=<repo>/rootfs/usr/lib/aarch64-linux-gnu \
      timeout -s KILL 120 <repo>/bifrost-emu --no-jit ./vkquake -basedir .
  ```
  (bifrost-emu ignores SIGTERM — always `timeout -s KILL`. Guest stdout is
  buffered and LOST on SIGKILL/SIGSEGV — redirect to a file AND check stderr.)
- **FIXED this session (all verified: objdump encodings / API docs / suite):**
  1. **Scalar SIMD ADD/SUB decode was WRONG** (interp_fp.cpp): old masks
     `0x?EC08{4,C}00` matched UNDEFINED encodings; real `add d2,d0,d1` =
     0x5EE18402 → masked(0xFFE0FC00)=0x5EE08400, `sub d`=0x7EE18402 →
     0x7EE08400. Scalar three-same uses bits[11:10]=01 (vector uses 10);
     bits[11:10]=11 is CMTST/CMEQ — do NOT "fix" masks to those patterns.
     Every scalar add became a silent NOP → mimalloc bitmap corruption → crash.
  2. **SDLVK_EXT arm misread SDL ABI** (thunk.cpp): pNames is the CALLER'S
     ARRAY to fill directly, NOT a slot receiving a new char** — old code
     wrote its mmap_alloc'd array address into names[0] (clobbering entry 0,
     leaving entry 1 uninitialized → garbage extension strings at
     vkCreateInstance).
  3. **vkCreateBufferView row arity** `iiip`→`ippp` (thunk_dp.txt) — same
     off-by-one class as the earlier vkCmdUpdateBuffer bug; pCreateInfo was
     reaching host RADV as a raw guest pointer.
  4. **pNext-chain deep marshal added** (display_thunk.cpp):
     `vk_pnext_size()` sType→size table + `vk_marshal_pnext_chain()` +
     `vk_writeback_pnext_chain()`; used by NEW arms for
     vkGetPhysicalDeviceProperties2/Features2 (+KHR aliases) and inside
     vkCreateDevice. LESSON learned TWICE: after marshalling you MUST
     re-point the parent's pNext (and BeginInfo's pInheritanceInfo) at the
     HOST copy — leaving the raw guest pointer in place crashes identically
     to no marshal. Sizes verified vs vendored vulkan_core.h (Properties2=840,
     DriverProps=536, SubgroupProps=32, SSCtrlProps=32, AccelProps=64,
     Features2=240, SSCtrlFeats=24, BDA=32, AccelFeats=40, RayQueryFeats=24
     [sType 1000348013 per vendored header], PresentId/Wait[2]=24,
     CommandBufferInheritanceInfo=56).
  5. **vkBeginCommandBuffer arm**: VkCommandBufferBeginInfo is 32 BYTES
     {sType,pNext,flags,pInheritanceInfo} — secondary command buffers
     (multithreaded recording) chain a guest InheritanceInfo (56 B); both
     pNext chains deep-marshalled now.
  6. **vkCreateComputePipelines arm**: mirrors graphics-pipeline stage
     marshalling (pName strings + SpecializationInfo map entries/data blob);
     vkQuake builds all shaders via compute pipelines.
  7. **RT extensions hidden from vkEnumerateDeviceExtensionProperties**
     (VK_KHR_ray_query / VK_KHR_acceleration_structure): guests that see them
     demand vkCmdBuildAccelerationStructuresKHR via GetDeviceProcAddr and
     Sys_Error when missing; RT build-geometry deep-marshal doesn't exist
     yet. REMOVE this filter once RT marshalling lands.
  8. **Display-mode getters fixed**: SDL_GetCurrentDisplayMode /
     SDL_GetDesktopDisplayMode take ONE int and RETURN const SDL_DisplayMode*
     (rows were `iip`/`ip` with phantom out-param); new name-based arm copies
     the 24-byte mode struct into a cached guest block (guest cannot deref
     host static memory).
  9. **SDL symbols resolve through dlopen("libSDL2-2.0.so.0") handle** instead
     of RTLD_DEFAULT (thunk.cpp register_known_symbols_).
- **HOST ENVIRONMENT LANDMINE**: this Arch box runs sdl2-compat over
  libSDL3.so (BOTH in the process). dlsym(RTLD_DEFAULT) can pick SDL3's
  same-named exports with DIFFERENT ABI (SDL3 SDL_GetDesktopDisplayMode(
  displayID, SDL_DisplayMode* out) is out-param flavor). Even with the
  specific handle, deep compat paths crashed: guest SDL_InitSubSystem(VIDEO)
  died writing through a stale register as an out-param INSIDE compat
  internals (pc libSDL2 vaddr 0x28fa2, rbx=bifrost .rodata spec-string
  "SDL_GetDesktopDisplayMode" — our own strcmp literal leaked into a stale
  reg that an SDL3-flavored callee used as its out-param). Pre-init video +
  intercepting InitSubSystem(VIDEO) made things WORSE (crash moved earlier)
  — BOTH REVERTED.
- **Current state**: boots through FULL Vulkan init (instance; RADV RX 7600
  detected with correct Vendor/Driver strings via Properties2 chain;
  swapchain setup; compute pipelines; multithreaded command-buffer recording;
  QueueSubmit frame loop observed for ~90 s in the best run), then hits one
  remaining SIGSEGV tied to the sdl2-compat environment. Suite quick 206/206,
  vulkan_swapchain + mapbuffer + modern GL ALL PASS on live RADV — zero
  regressions from this session's changes.
- **NEXT STEPS**: (a) root-cause the sdl2-compat interaction — candidate:
  pin the guest to REAL SDL2 (check whether rootfs/Debian libs provide one),
  or call SDL_DYNAPI_entry explicitly; gdb two-stage breakpoints (break
  SDL_Init first, then raw addrs); libSDL2 text mapping file-offset 0xd000
  ≠ vaddr 0x10000-ish — compute offsets via info proc mappings, NOT naive
  subtraction. (b) RT support needs vkCmdBuildAccelerationStructures
  deep-marshal if a game hard-requires ray query.

## Session History (2026-08-22) — vkQuake in-game + SDL audio routing fix (CONTINUATION)

- **vkQuake now reaches IN-GAME state** (client parsing server messages,
  CL_ParseLocalSound) after these additional fixes; remaining crash is a
  nondeterministic RADV worker-thread fault (0x48-stride descriptor walk,
  pc libvulkan_radeon+0x3e303) — needs its own focused session.
- **NEW FIXES this continuation (all suite-verified):**
  1. **SDL_GetMouseState/SDL_GetGlobalMouseState rows `-` → `pp`** — real
     signature is (int* x, int* y) out-pointers; raw guest addrs reached
     host SDL (crash writing coords).
  2. **fcmgt/facge/facgt/fcmeq/fcmge (register) implemented in interp**
     (interp_fp.cpp, sub_noq cases 0x0E20E400/0x2E20E400/0x2EA0E400/
     0x2E20EC00/0x2EA0EC00): per-lane all-ones-or-zero, NaN→false for
     ordered compares, FA* use absolute values. Verified encodings via
     cross-assembler. vkQuake's math does `fcmgt v28.2s,v19.2s,v31.2s`
     (0x2EBFE67C). JIT falls back via CALL_INTERP (classify UNKNOWN).
  3. **SDL_RWFromMem/ConstMem UAF fixed** (thunk.cpp name-arm): SDL keeps
     the mem pointer in the RWops beyond the dispatch call — a temporary
     bounce buffer freed at return = heap corruption anywhere later.
     Stable direct-window alias when possible, else persistent tracked
     host copy (freed at thunk shutdown, impl_->rw_kept_).
  4. **SDL audio symbols moved to AudioThunk registry** — REMOVED 13 rows
     from thunk_dp.txt (OpenAudio[Device], QueueAudio, PauseAudio[Device],
     CloseAudio, Lock/UnlockAudio[Device], GetAudioDeviceName, GetCurrent-
     AudioDriver, Get/ClearQueuedAudio). WHY NOT -ENOENT fall-through:
     the SVC chain passes the SAME symbol_id to each thunk and AudioThunk
     only accepts its own ID range — GraphicThunk ids can't re-route.
     First-definition-wins in dynlinker means graphic rows shadowed the
     AudioThunk's working callback-mode arms, so SDL_OpenAudio always
     returned -1 ("audio unavailable") → vkQuake ran with NULL sound fns.
  5. **AudioThunk gained legacy arms**: SDL_LockAudio/UnlockAudio (no-op),
     SDL_CloseAudio (closes implicit device 1), SDL_GetCurrentAudioDriver
     (returns "bifrost"), + REG entries for all four.
  - GOTCHA: removing the last row using a policy drops it from the
    generated Policy enum — delete any code referencing it (SDL_OPEN_AUDIO
    arm had to go too).
- **Verified**: quick suite ALL PASS; vulkan_swapchain / linux_audio 16/16 /
  android_audio / android_activity 29 / sdl_gl_triangle ALL PASS on live
  DISPLAY=:0. vkQuake best run: full Vulkan init → in-game server-message
  parsing; then nondeterministic aborts (RADV worker crash OR DecodeError
  pc=0x0 null fn-ptr call — possibly audio-adjacent, needs investigation).
- **NEXT**: (a) RADV 0x48-stride walk crash — get exact symbol attribution
  with TID-tagged trace (vk_dispatch_ lines now carry T%lx); suspect our
  UpdateDescriptorSets staging or an unmarshalled nested struct. (b) The
  null-call may be vkQuake's snd path still half-initialized — check
  whether SNDDMA_Init succeeds end-to-end now that OpenAudio works.


## Session History (2026-08-22) — minecraft/neverball regression hunt (CONTINUATION 2)

- **REGRESSION FOUND AND ROOT-CAUSED**: after the audio-routing fix,
  BOTH minecraft_weekend AND neverball died seconds in with NULL-call
  DecodeErrors (pc=0x0). Root cause: **the AudioThunk SDL pump thread
  fired the GUEST audio callback through the borrow-CPU runner on the
  MAIN CPU concurrently with the running guest** → state corruption →
  arbitrary delayed crashes. The AAudio/OpenSL pump threads (Android)
  never showed it because those tests are single-threaded/simple.
  FIX (interim): SDL callback-device pump threads DISABLED — SDL_OpenAudio
  succeeds but stays silent. Audio-callback correctness requires a
  deferred-execution model (e.g. run pending callbacks inline inside
  frequent SDL dispatches like PollEvent/Delay on the calling thread,
  or a dedicated guest CPU context). The borrow-CPU pattern IS safe when
  invoked from the syscall path of a suspended guest thread — the bug is
  only ASYNC invocation from a host thread while guest code runs.
- **With the pump disabled**: minecraft 30s+/42-47K frames stable ×4,
  neverball 45s+ to menu stable ×2, vkQuake progressed past its null-call
  into real rendering commands.
- **fcvtn/fcvtxn implemented in interp** (sub_noq 0x0E616800 FCVTN /
  0x2E616800 FCVTXN, d→s): FCVTN zeroes v_hi at Q=0; FCVTXN PRESERVES it
  (ARM ARM; that's why FCVTXN2 exists). FCVTXN uses round-to-odd
  approximation (sticky LSB). Encodings verified via cross-assembler
  (fcvtn v5.2s,v3.2d = 0x0E616865).
- **Verified**: quick suite ALL PASS; linux_audio 16/16 (queue-mode tests
  unaffected); minecraft/neverball/vkQuake runs above.
- **vkQuake remaining crash**: RADV worker-thread fault (0x48-stride walk,
  pc libvulkan_radeon+0x3e303, `mov 0x40(%rcx),%rdx` iterating 72-byte
  structs reading ptr@+0x40) — nondeterministic, needs TID-attributed
  trace in a focused session.


## Session History (2026-08-22) — working audio path (Linux + Android)

- **The audio thunk was rewritten from "forward to host libs" to
  "convert to AudioEngine pushes"** (plan:
  `~/.opencode/plan/android-linux-audio.md`). The old approach stubbed
  everything because host libasound/libpulse deref opaque structs with
  HOST pointers. Every arm now converts its call to plain interleaved
  sample pushes on the shared `Audio` ring via the new
  `Audio::write_interleaved(fmt, rate, ch, data, bytes)` (U8/S16/S24-in-
  32/F32 conversion + linear resample + mono→stereo dup; opens the device
  on first use with the PUSH format so WAV-dump mode records guest
  layout). `bytes_pushed()` is the headless test counter.
- **CRITICAL thunk-dispatch contract discovered**: the shared SVC chain
  (misc.cpp:961 / jit_interp.cpp:122) treats a thunk's `return 0` as
  "handled, do NOT write x0" — successes that legitimately return 0 left
  STALE x0 in the guest (guest saw a trampoline address for snd_pcm_open).
  The AUDIO branch now always writes x0 unless dispatch returns -ENOENT
  ("not my symbol"). AudioThunk arms return the REAL guest x0 value; only
  -ENOENT means fall-through to the next thunk. Do not "restore" the old
  r==0 fast path on the audio branch.
- **Arms implemented** (name-keyed in AudioThunk::dispatch, host audio
  libs never called): SDL2 (SDL_OpenAudio/OpenAudioDevice deep-translate
  of the guest SDL_AudioSpec — freq@0 i32, format@4 u16, channels@6,
  samples@8, callback@16, userdata@24, size 32 — queue mode via
  SDL_QueueAudio/GetQueuedAudioSize/ClearQueuedAudio); ALSA subset
  (snd_pcm_open writes a fake handle into *pcmp; hw_params setters
  record fmt/rate/ch; writei → engine push returning frames);
  Pulse simple (pa_sample_spec {u32 fmt,u32 rate,u8 ch} at New);
  OpenAL buffer/source state machine (push-on-play approximation;
  AL_FORMAT tags 0x1100-0x1103 + float 0x10010/0x10011).
- **Android**: AAudio builder pattern is flat-scalar rows recorded
  host-side; openStream allocates a direct-window bounce and starts a
  per-stream PUMP THREAD (10 ms tick) that fires the GUEST data callback
  through the borrow-CPU runner (`wire_thunk_audio_runner_` →
  `call_guest_function`, main CPU) and pushes bounce→ring. Blocking-mode
  AAudioStream_write pushes directly. OpenSL ES uses SYNTHETIC VTABLES
  built in guest RAM: objects are `[itf_word]→[vtable of __osl_*
  trampolines]`; SL_IID_* symbols are registered rows whose TRAMPOLINE
  ADDRESSES double as IID identity tags (guests pass them by pointer to
  GetInterface — pointer equality, never dereferenced). BufferQueue
  Enqueue pushes immediately then fires the registered callback INLINE
  on the calling guest thread (streaming-player approximation).
- Guest callbacks NEVER cross to host libs (GLFW *_CB rule). Pump
  threads join at Emulator teardown (~Emulator → athunk->shutdown();
  also ~AudioThunkImpl defensive). Bounce buffers free via
  untrack_allocation on close (glDeleteBuffers pattern).
- `is_thunk_supported_lib_` gained libaaudio/libOpenSLES (dlopen filter).
- Tests: `ctest_real/test_linux_audio.c` (16 checks: ALSA subset + SDL2
  queue + Pulse simple + /dev/dsp) and `ctest_real/test_android_audio.c`
  (21 checks: AAudio builder/open/state/write/close + full OpenSL object
  walk incl. callback-fire count). Both HEADLESS-SAFE (WAV backend counts
  bytes). Registered in run_tests.sh as linux_audio/android_audio.
- Guest-test gotcha re-confirmed: the internal dlopen svc number is
  0x1002 (NOT 0x1001 — that's TLS alloc); dlsym is 0x1003.
- Verified: build clean, quick suite **205/205**, FULL suite **210/210**,
  both new tests ALL PASS under JIT.

## Session History (2026-08-22) — SMOV + saturating-int SIMD family (interp)

- **New interpreter ops landed in `interp_fp.cpp` (+ test `ctest/test_simd_sat.c`,
  21 checks, ALL PASS under JIT and `--no-jit`; quick suite 202/202):**
  SMOV (`sub_noq 0x0E002C00`, ASIMDINS group bits[15:12]=0010 — do NOT match
  UMOV's 0011 or SMOULDN'T-be-matched 0001/0010 INS forms; esize==8 (.D) is
  UNALLOCATED and must fall through to DecodeError), SQADD/UQADD,
  SQSUB/UQSUB, SQSHL/UQSHL (register), SQRSHL/UQRSHL, SRSHL/URSHL
  (three-same, inner switch on `sub3_noq`), and SQABS/SQNEG/SUQADD/USQADD
  (two-reg misc). None are in the simd_dp table yet → JIT CALL_INTERPs them
  (classify UNKNOWN), so interp is the only semantics today.
- **THREE bugs found while validating (all fixed):**
  1. **`static_cast<__int128>(uint64_t)` ZERO-extends.** The sat helpers'
     `sext_lane` returns the correct BIT PATTERN as uint64_t, but casting it
     straight to `__int128` converts the unsigned VALUE (+1.8e19 for -128)
     → every signed saturating op clamped to +max. Fix: route through
     `static_cast<int64_t>` BEFORE widening to __int128 (sat_add_s/sat_sub_s/
     sat_neg_s + the SUQADD site). Host repro: `sat_add_s(0x80,0x80,8)`
     returned 0x7f at ANY optimization level — this was never a miscompile.
     Rule: uint64 bit patterns must be reinterpreted SIGNED (int64_t) before
     value-converting to a wider type.
  2. **The neverball ABDL block's discriminator was too crude**: it tested
     `(sub3_noq & 0x7000)` ∈ {0x5000,0x7000} (bits[14:12] only) which ALSO
     matched SRSHL/SQRSHL (opcode 010101/010111) and SQABS/SQNEG
     (011110/011111), silently routing them into widening-abs-diff logic
     (nondeterministic garbage output, no crash). Fixed to exact opcode6
     matches: SABDL/UABDL = 0x1C, SABAL/UABAL = 0x14. When matching inside
     the sub3_noq region ALWAYS use the full 6-bit opcode — bits[14:12]
     alone collide across encoding families.
  3. **Test expectations violated ARM spec** (test-side, emulator correct):
     `sqshl` of `1<<63` SATURATES to INT64_MAX (not INT64_MIN);
     SUQADD saturates to the SIGNED range (dest signed, Vn unsigned;
     −1 + 0xFFFFFFFF → INT32_MAX); USQADD clamps negative sums to 0
     (dest unsigned, Vn signed; 0 + (−1) → 0).
- **JIT MIGRATION (same session, later): SMOV + SQADD/UQADD/SQSUB/UQSUB
  (B/H lanes) are now NATIVE via the table pipeline.** Rows added to
  `tools/opgen/simd_dp.txt` → `make opgen` (105→110 ops):
  - SMOV row: mask `0xBFE0FC00` match `0x0E002C00`, guard admits imm5 ∈
    {1,2,4} only (B/H/S; the .D form is UNALLOCATED and must stay UNKNOWN
    → interp → DecodeError).
  - SATADDSUB rows: SQADD/UQADD/SQSUB/UQSUB matches `0x{0E,2E}200C00` /
    `0x{0E,2E}202C00`, subop {0,1,2,3}, guard **`size < 2`** — byte and
    halfword lanes ONLY. Rationale: SSE2 has EXACT saturating instructions
    for those widths (PADDSB EC / PADDSW ED / PADDUSB DC / PADDUSW DD /
    PSUBSB E8 / PSUBSW E9 / PSUBUSB D8 / PSUBUSW DA); 32/64-bit lanes have
    no compact pre-AVX512 form and stay on the interpreter.
  - New IR ops `SIMD_SMOV` / `SIMD_SATADDSUB` (ir.hpp), translator cases in
    ir_translate_fp.cpp's SIMD_DP family switch, codegen in jit_codegen_simd.cpp
    right after SIMD_UMOV: SMOV = UMOV's zero-extending element load +
    `shl d,N; sar d,N` sign-extension (clobber_flags() first — shifts kill
    RFLAGS; kind 4=SHL/7=SAR). SATADDSUB = vec_cache_active_ guard →
    CALL_INTERP fallback, load_vec(0/1) + one `sse2_op(kOp[subop][esize2],
    0, 1)` + store_vec (memory-path style; NOT vec-cache compatible; no GPR
    flush needed — XMM+memory only). instr_will_call_interp needed NO edits
    (classify-driven).
  - STILL interpreter-only (deliberate): SRSHL/SQRSHL/SQSHL-family register
    shifts (per-lane VARIABLE shifts need AVX2 vpsllv/vpsrav which only
    cover 32-bit; 8/16-bit widening tricks not worth it yet) and the
    32/64-bit saturating add/sub forms.
  - Verified: test_simd_sat 21/21 under JIT, --no-jit AND BIFROST_JIT_VERIFY=1
    (zero divergences); native path proven by grepping BIFROST_JIT_DUMP bytes
    for `66 [REX] 0F {EC,DD,E8,D8} c1`; quick suite 202/202.
- **`test_simd_sat` registered in `scripts/run_tests.sh`** (unit table,
  after simd_misc: `"simd_sat|ctest/test_simd_sat.elf||5|ALL PASS"`).
  Suite counts bumped accordingly — full default suite **208 pass / 0 fail
  / 0 skip**, `--quick` **203** (verified with a real FULL-suite run; the
  Verification section counts were updated to match). Historical session-
  history numbers below are records of what was true at their time — do
  not "fix" them to current totals.

- Debug-methodology reminders: stderr probes print immediately but stdout
  buffers until exit — never infer ORDER from mixed streams; count probe
  LINES not positions. And after editing a file, `make setup-tests` does
  NOT rebuild the emulator binary — run `make` or you validate stale code
  (a leftover [dbg] print in the output is the tell).

## Session History (2026-08-21) — stats reporter FIXED, debug probes dropped, docs reorganized


- **BIFROST_STATS_PERIOD never printed mid-run — THREE stacked causes,
  all fixed**: (1) the period check sat in run()'s outer loop, which
  STOPS ITERATING once a game's frame loop parks inside
  jit_call_helper's callee-dispatch loop — now a background reporter
  thread ticks on wall time (sleeps ≤0.25s slices, joined before run()
  returns so it never outlives the Emulator); (2)
  `dump_periodic_stats` silently no-oped under `--no-jit` (`if (!jit_)
  return;`) — now prints `guest(interp): N MIPS` from interp_count_ +
  the syscall histogram; (3) the hot-path block/instruction counters
  were function-local TLS flushed ONLY on run_block's slow path — a
  fast-path-parked guest (last-block cache / inline cache / chains /
  jit_call_helper) never flushed and every reader saw stale zeros
  ("0.0 MIPS mid-game"). Counters moved to
  `FrostJIT::tls_stat_exec_/tls_stat_instr_` (shared TLS, defined in
  frostjit.cpp) with `flush_stat_tls()` batch-flushed every 64K
  dispatches from BOTH run_block fast paths AND jit_call_helper's
  lookup_call_target loop — one predictable branch per dispatch, NOT a
  per-dispatch atomic (dispatch-loop contract preserved; bench_mips
  0.358s unchanged). Verified: game mid-run shows 25.9 MIPS startup /
  ~55 MIPS steady frame loop; interp bench shows ~62 MIPS; exit dump
  also un-undercounted now. Do NOT move the reporter back into the run
  loop or un-batch the flush.
- **Debug probes dropped**: `BIFROST_VP_DBG` glViewport print
  (gl_state.cpp) and the `[szdbg]` TEMP size probes +
  `s_last_sdl_window_` (thunk.cpp) — the latter also carried an
  unused-local-typedef warning.
- **Docs reorganized**: every doc except README.md and AGENTS.md moved
  to `docs/` (AGENTS.md must stay at the repo root — the agent harness
  reads it there; do not move it). docs/DISPLAY_THUNK.md, docs/rules.md,
  docs/context.md, docs/findings.md, docs/SESSION_SUMMARY.md are
  gitignored (local-only notes). CHANGELOG gained the 2026-08-21
  [Unreleased] section (CSEL fix, arity bugs, reporter, Vulkan
   pipelines + vkMapMemory, Neverball batch); ROADMAP #12 marked DONE.

## Session History (2026-08-22) — Android NativeActivity lifecycle layer v2

- **Android support expanded from surface-only (v1) to full NativeActivity
  lifecycle + input + config/logging — `bifrost-emu --android libfoo.so`
  boots a native_app_glue .so without ART/Java.** `AndroidSurfaceManager`
  (frost/android_surface.hpp + android_surface.cpp) now plays the
  framework role: `create_activity()` synthesizes an ANativeActivity
  struct + 16-entry callback table in guest memory (modern NDK layout;
  `BIFROST_ANDROID_LEGACY_CB=1` switches to the pre-API-26 13-entry
  table); `fire_activity_cb()` reads the GUEST function pointer live and
  invokes it through the borrow-CPU runner (same save/restore + sentinel
  LR pattern as `wire_thunk_glfw_cb_runner_`).
  The Emulator wires it (`wire_thunk_android_runner_`) with a Memory*
  + fd→host_fd resolver (`fds_.get(gfd)->host_fd()`) so the ALooper +
  AInputQueue thunks can operate without DisplayThunk knowing the VFS.
- **ALooper registry** (thunk policy `ANDROID_WINDOW`): `prepare` returns
  a singleton `0xA90001000001` handle; `addFd/removeFd/wake` mutate the
  registration table; `pollOnce/pollAll` report readiness via a real
  `::poll()` on the resolved HOST fd (guest pipes are real host pipes
  under HostNode) plus input-queue attachments. The first write of a
  command byte to the msgpipe makes `pollOnce(30) → ident 42` work
  (verified by `test_android_activity`).
  `AInputQueue_attach/detachLooper` records the looper+ident+data so
  `looper_poll` can return LOOPER_ID_INPUT when `pending_` is non-empty.
  `wake` sets a flag the poll loop checks first. Verified: poll timeout
  returns -3; fd readiness after `write(pipe)` returns the registered
  ident.
- **AInputQueue + event store** (fixed 32-slot table, `0xA90003…` handles):
  `getEvent` pops from a `deque<InputEvent>` into a free slot and hands
  out its handle; `preDispatch` returns 0; `finishEvent` frees the slot.
  `AMotionEvent_getX(foreground)` etc. write the float to guest `v0`
  (`cpu.v_lo[0]` + zeroed `v_hi`), because a `float` return lives in S0
  not X0 (the generic `thunk_dispatch_generic` would return garbage in
  RAX for float-returning host fns). All getters snapshot the event
  under `mu_` then read fields (no lock held while invoking a runner).
  `motion_touch_major/minor` derive from `size` (48/36 px). Edge/history
  getters are stubs.
- **SDL→Android translation** (`pump_host_events`): `SDL_PollEvent` on the
  proxy window maps MOUSEBUTTONDOWN/UP→ACTION_DOWN/UP,
  MOUSEMOTION (pressed→MOVE else HOVER_MOVE), FINGER*→same, KEYDOWN/UP→
  AKeyEvent via a `SDL_Keycode → AKEYCODE_*` table (a-z 29..54, 0-9
  7..16, F1..F12 131..142, arrows 19..22, home/end 122/123, etc.,
  modifiers into `AMETA_*`). Window SIZE_CHANGED updates `width_/height_`
  and fires `onNativeWindowResized`/`onContentRectChanged` when a CPU is
  provided. `request_quit` on SDL_QUIT. Tested via
  `BIFROST_ANDROID_TAP=1` synthetic center tap injected once after the
  input queue is delivered.
- **AConfiguration stubs** (20 rows) + **liblog stubs** (`__android_log_*`
  with a minimal `%-`format translator: `%s` translates a guest pointer
  via `guest_str`, `%d/%u/%x/%c/%f` from the guest varargs slots `x3..x7`
  + `[sp]`; writes `[android-log] tag: msg` to stderr). `AConfiguration`
  returns 160 dpi, SDK 34, PORTRAIT 1, FINGER 3, `en`/`US`.
- **Opgen growth** `tools/opgen/thunk_dp.txt` 932 → 999 symbols; new rows
  all family `ANDROID` policy `ANDROID_WINDOW` (the display dispatch arm
  name-dispatches). The display dispatch arm now handles the framework
  symbols **without** requiring a host window (AConfiguration getters run
  before any surface exists); the ANativeWindow path still lazily creates
  the proxy. `kAndroidSonames` now `{"libandroid.so","liblog.so"}` so
  either soname resolves via the thunk; `is_thunk_supported_lib_` accepts
  `liblog.so`. `DisplayThunk::ensure_android_window()` eagerly arms the
  window for the `--android` driver (so `from_surface` works before any
  guest call). Needed for `run_android`'s pre-`onNativeWindowCreated`
  shim creation; previously `from_surface` returned 0 in that mode.
- **`--android` driver** (`main.cpp` + `Emulator::load_android_activity`
  / `run_android`): `ensure_thunk_linker_()` extracts the shared
  dynlinker+thunk+ifunc/init/guest-call wiring (used by both the
  normal and the android paths so the 50-line lambda duplication was
  eliminated); `load_android_activity(path, argv)` does
  `dyn_linker_->load_library(main_cpu_, path)` → resolve
  `ANativeActivity_onCreate` into `android_on_create_`, builds a dummy
  initial stack+TLS+vdso+zero-page (no main ELF), `create_activity()`,
  and eagerly ensures the window. `run_android()` fires
  `onCreate(activity,NULL,0)`, then `onStart/onResume/focus/inputQueue/`
  `windowCreated`, drains `pump_host_events(&main_cpu_)` every 4 ms until
  window close / guest exit_group — NO default deadline (a real game
  must run indefinitely; `BIFROST_ANDROID_TIMEOUT_SECS=N` opts into a
  hard cap for CI). `BIFROST_ANDROID_TAP=1` injects the synthetic tap.
  Then it shuts down gracefully (`onPause/onStop/inputDestroyed/
  windowDestroyed/`
  `onDestroy`). Wiring fix: the normal ELF paths now also call
  `wire_thunk_android_runner_()` so a plain static ELF that `dlopen`s
  libandroid (like `test_android_activity.elf`) gets its fd resolver
  even without `--android`.
- **Guest test** `ctest_real/test_android_activity.elf` (29 checks, "ALL
  PASS", headless-safe): AConfiguration defaults, looper prepare + poll
  timeout/wake + pipe fd readiness, input queue attach/empty→finish,
  null-handle getters, and liblog. Added to `scripts/run_tests.sh` as
  `android_activity` (no DISPLAY needed). Manual `.so` smoketest
  `/tmp/test_android_native.so` via `BIFROST_ANDROID_TAP=1 ./bifrost-emu
  --android /tmp/test_android_native.so` verifies the full lifecycle
  (`onCreate→onStart→onResume→focus→inputQueueCreated→windowCreated→`
  config density ok → log → timeout → onStop/inputDestroyed/
  windowDestroyed→onDestroy→ALL PASS, `rc=0`; run with
  `BIFROST_ANDROID_TIMEOUT_SECS=N` + `timeout -s KILL` — the emulator
  ignores SIGTERM).
- Verified: `make` clean, `opgen-check`/`thunk-check` up-to-date (999),
  quick suite **202/202** (was 201 before the new test), direct run of
  `test_android_activity` 29/29 both JIT and `--no-jit`, and the
  `--android` mode smoketest passes on DISPLAY=:0.
- **Review pass on this layer found five real defects; all fixed and
  their contracts hold:**
  1. **pollOnce/pollAll dispatch flag**: the display arm must pass
     `dispatch_callbacks = (n == "ALooper_pollAll")`. With `true`,
     pollOnce drains callback-mode registrations until timeout; real
     semantics fire ONE callback and return `POLL_CALLBACK(-2)`.
  2. **Indexed motion getters bounds-check FIRST**:
     `ANDROID_EVENT_GETTER_IDX` clamps `idx >= 8` BEFORE the signed
     `pointer_count` compare — a guest-passed huge size_t truncates to a
     negative int32 and the signed compare alone reads `pointers[]` OOB.
  3. **No default deadline in run_android**: real games must run until
     window close / guest exit_group. `BIFROST_ANDROID_TIMEOUT_SECS=N`
     is the OPT-IN CI cap. A guest exit_group from any thread kills the
     host process directly (nothing to detect in the pump loop).
     Remember bifrost-emu ignores SIGTERM — bound test runs with
     `timeout -s KILL` AND the env cap.
  4. **main.cpp's --android branch exits the arg loop early**, so it
     must apply CLI-over-config precedence ITSELF (mirror of the normal
     path's post-loop overrides): verbose/debug/quiet locals + cfg,
     `use_jit && cfg.jit_enabled`, threshold = CLI or config.
  5. The secondary ifunc resolver in `ensure_thunk_linker_` zeroes its
     result when the instruction limit trips (never return a
     mid-execution register value as a function pointer).

## Session History (2026-08-22) — mambo Vulkan e2e test with real audio

- **`ctest_real/test_mambo_vulkan.c` committed (`67e71c6`)**: SDL2 window +
  Vulkan textured quad (Matikanetannhauser texture from
  `assets/mambo/matikanetannhauser_race.webp`, embedded SPIR-V) exercising
  the full deep-marshal pipeline, plus one-shot audio through the ALSA
  thunk arm. The audio is REAL decoded PCM from
  `assets/mambo/mambo_sfx.mp3`, embedded as `ctest_real/mambo_audio.h`
  (44.1 kHz s16 stereo, ffmpeg offline decode — guests have no MP3
  decoder; the embed-header pattern matches mambo_tex.h/test_vulkan_spv.h).
  An earlier version synthesized a sine/noise melody — replaced.
- **Audio ring capacity 64 KiB → 256 KiB** (`src/audio/audio.cpp`
  RING_CAPACITY): ~1.5 s @44.1k s16 stereo so burst pushes (the test
  writes all PCM at once in 16 KiB chunks) never drop samples.
- **SDL_Vulkan_CreateSurface arity fixed** (`thunk_dp.txt`/`opgen_thunk.hpp`):
  `ii` → `iip` — the trailing `VkSurfaceKHR*` out-pointer must be
  translated or host SDL writes through a raw guest address.
- Source assets tracked under `assets/mambo/`. Registered as
  `mambo_vulkan` in run_tests.sh (needs DISPLAY + Vulkan, exit 77 skip);
  default suite now **211**, quick 206. Verified live on DISPLAY=:0/RADV:
  ALL PASS, "MAMBO VULKAN TEST PASSED — ¡MAMBO!", rc=0.

## Session History (2026-08-23) — scalar-pairwise FP + scalar×indexed-element FP (interp)

- **rudolf-cart investigation surfaced TWO silent-NOP FP families in
  `compiler_rt.sin`; both implemented in the FP_SCALAR case of
  `interp_fp.cpp` just before the `[FP-NOP]` fallback:**
  1. **Scalar pairwise FADDP/FMAXP/FMINP** (`Sd←Vn.2S`, `Dd←Vn.2D`):
     mask `0xFFFFFC00` matches `0x7E30D800|0x7E70D800` (add),
     `0x7E30F800|0x7E70F800` (max), `0x7EB0F800|0x7EF0F800` (min);
     bit22=double, bit23=min. D-form lanes are `v_lo[rn]`/`v_hi[rn]`;
     S-form BOTH lanes live in the low word of `v_lo[rn]`. NaN:
     std::fmax/fmin semantics (one-NaN → other operand).
  2. **Scalar × indexed-element FMLA/FMLS/FMUL/FMULX**: layout verified
     byte-for-byte vs the cross assembler — bits[31:24]=01U11111,
     bit23=1 (fixed), bit22=sz, bit21=L (.s index MSB), bit20=M=Rm<4>,
     bits[19:16]=Rm<3:0>, opcode bits[15:12] (1=FMLA, 5=FMLS, 9=FMUL,
     FMULX=9 with U=bit29), bit11=H (index LSB), bit10=0. Index:
     `.d`→H; `.s`→{H,L} with H as MSB. FMLA/FMLS read Rd as ACCUMULATOR.
     Match predicate `(op & 0xC0000400)==0x40000000 && bits[28:24]==11111
     && bit23==1` — the bit23 test is REQUIRED: scalar shift/narrow-imm
     forms (sqshrn/sshr/shl) share opcode 9 and the same space but carry
     immh there with bit23==0.
  - Encoding traps learned en route: `.d` forms only allow Vm v0-v15
    (GNU as silently aliases v≥16 into different instructions); objdump
    on a raw .word file shows ".word" — always disassemble in context;
    the assembler's scalar-pairwise mnemonics fail in inline asm, so
    tests emit `.word` directly (constant-folded via always_inline).
- JIT side unchanged: both families classify UNKNOWN → CALL_INTERP, so
  interp-only fix covers both modes ([FP-NOP] fired under JIT confirms
  routing). New regression test `ctest/jit_fp_pw_elem.c` (18 checks,
  "ALL PASS", .word-based inline asm) registered as `fp_pw_elem`;
  suite now **212**, quick **207** (207/207 run). rudolf-cart autoshot:
  zero [FP-NOP] lines after fix.

## Session History (2026-08-23) — AudioEngine multi-stream rewrite + dedicated-vCPU audio pump

- **Audio class REWRITTEN to the real SDL2/SDL3 model (one physical device,
  N logical streams, mixed in the device callback).** The old design
  funneled ALL devices into ONE shared ring gated by a global 32 KiB
  level check: music (re-topped every frame) pinned the ring permanently
  so sfx could never push; its pending grew until guests that auto-clear
  on backlog called SDL_ClearQueuedAudio — which wiped the WHOLE shared
  ring. Net: "sfx dies after minutes, music glitchy but alive"
  (rudolf-cart). New architecture in src/audio/audio.{h,cpp}:
  - `stream_open/close/pause/clear/write` — per-stream float rings at
    DEVICE rate (converted+resampled at push), mixer sums non-paused
    streams and clips once. Backpressure is PER-STREAM (full ⇒ accept 0).
  - `stream_write` accepts PARTIAL input (whole frames whose converted
    output fits); all-or-nothing DEADLOCKED guests that write
    multi-second buffers in one call (audio_test wrote 1 s = 44100
    frames > 370 ms ring → write returned 0 forever, guest spun at
    20K syscalls/s). Partial acceptance is why test_pipe's cousin
    `audio_test` passes again.
  - `stream_queued_frames()` → SDL_GetQueuedAudioSize now reports the
    HONEST backlog (thunk pending + stream ring in guest bytes).
  - Legacy /dev/dsp path (devfs) routes through an implicit stream so it
    plays via the same mixer; `write()` returns BYTES accepted.
  - mix_interleaved/ring_queued_bytes/ring_free_bytes/clear_queued are
    GONE — update any out-of-tree caller.
- **Dedicated-vCPU audio pump (`AudioThunk::start_pump`, default ON,
  BIFROST_AUDIO_PUMP=0 opts out)**: a host clock thread (2 ms tick)
  fires guest data callbacks on an EXCLUSIVE cloned vCPU (arch-state
  copy of main CPU incl. TPIDR_EL0; CPU is non-copyable — copy fields
  manually) at per-stream period pacing with 500 ms stall resync.
  Real SMP semantics — mirrors Android's in-process AAudio callback
  thread; decouples callback cadence from guest API-call frequency (the
  AAA Android requirement). When active, inline deferral from dispatch
  arms is DISABLED (single callback mutator). Bounce is snapshotted
  under `pump_mu` (recursive_mutex) before firing so a callback closing
  its own device mid-fire can't UAF. SDL/AAudio open/close arms take
  pump_mu around map mutations.
- **CRITICAL fork contract: host threads don't survive ::fork().** A
  forked child inherits the pump's joinable std::thread OBJECT whose
  real thread lives only in the parent — shutdown's join() futex-waited
  FOREVER, hanging EVERY forked guest at exit (test_pipe rc=137 after
  "CHILD: from parent"; child stuck in futex_wait inside exit_group's
  teardown). Fix: fork_guest's child branch calls
  `audio_thunk->detach_pump_for_fork_child()` (detach phantom, reset,
  start_pump() fresh for the child). ANY future emulator-owned host
  thread needs the same treatment in fork_guest.
- **Guest-side pacing lesson (rudolf-cart shim)**: wall-clock-paced
  fill threads MUST schedule in MICROSECONDS — ms-truncated chunk
  periods (23 vs 23.2199 ms) made production exceed realtime ~1%,
  accumulating ~35 s of audio latency per HOUR ("coin sounds arrive
  minutes late"). Real SDL avoids this because the DEVICE CLOCK drives
  callbacks. Also: GetQueuedAudioSize-based clear-guards must be well
  above honest ring depth (~30 chunks), or they self-delete audio.
- rudolf-cart now overlaps music+sfx correctly with stable latency;
  guest_sdl.c fill thread uses µs scheduling + 64-chunk stall guard.
- Verified: quick suite **207/207**; linux_audio 16/16, android_audio
  21/21 both modes ×pump on/off; audio_test exits cleanly; test_pipe
  3/3 with pump on.

## Session History (2026-08-23) — vk.xml registry integration (milestone 1: mechanical audit)

- **vk.xml is now the mechanical ground truth for VK row signatures.**
  New tools (analysis + CI, no runtime change to marshalling yet):
  - `tools/opgen/vkxml.py` — minimal parser for the vendored registry
    (`tools/vulkan-headers/registry/vk.xml`, 864 commands / 1754
    structs / 63 handles). Handles use BOTH forms: `name=` attribute
    (aliases) AND child `<name>` inside
    `VK_DEFINE_(NON_DISPATCHABLE_)HANDLE(...)` — parse both.
    `<param api="vulkansc">` duplicates are Safety-Critical variants:
    keep params whose api list contains plain "vulkan" (exact-match the
    comma list — "vulkansc" substring-matches!).
  - `arg_kinds()` derives thunkgen-style letters per param:
    i/f scalars, P = pointer to scalar/string/handle-array or void*
    (generic bounce), S = flat struct ptr (generic bounce), D =
    dynamically-sized/nested (needs a marshal arm). vkQueueSubmit →
    `iiDi` correctly flags its nested arrays.
  - `tools/opgen/vkxmlcheck.py` (+ `make vkxml-check`, in check-all):
    audits EVERY VK row in thunk_dp.txt against vk.xml — arity and
    pointer-position agreement. First run found **11 real errors**,
    including the exact class that bit us before (shifted pointer mask
    reaching host memcpy as raw guest addresses).
- **Bugs fixed from the first audit run:**
  1. `vkCreateDebugUtilsMessengerEXT` had pCreateInfo/pAllocator as
     un-translated 'i' — any guest creating a debug messenger crashed
     the host driver. It is now INTERCEPTED BY NAME in display_thunk
     dispatch: mints a fake handle (0xD6C0FFEE00000000+n), returns
     VK_SUCCESS, NEVER forwards (pfnUserCallback is a GUEST function
     pointer — same rule as glDebugMessageCallback). Destroy is an
     accepted no-op. Spec row annotated; checker DELIBERATE-exempted.
  2. `vkCreateAccelerationStructureKHR iiip→ippp`,
     `vkDestroyAccelerationStructureKHR iii→iip`,
     `vkGetAccelerationStructureBuildSizesKHR iipppp→iippp` (extra arg
     shifted everything; RT rows are unreachable while RT extensions are
     filtered, but corrected anyway for direct GetDeviceProcAddr users).
  3. Deliberate shapes documented in the checker's DELIBERATE table:
     graphics/compute-pipeline arms force pAllocator=NULL at the host
     call (display_thunk.cpp ~1978); VK_GET_PROC/PRESENT/SUBMIT own
     their marshalling entirely.
- Registry quirks learned: vendored vk.xml carries vulkansc duplicate
  params (api="vulkansc") that MUST be filtered or arity doubles
  (vkCreateSwapchainKHR appeared as 5-param); handle names hide inside
  parenthesized child text.
- **Warnings (39) are the deep-marshal roadmap**: dynamically-sized
  params dispatched as plain 'p' — vkCmdPipelineBarrier (3 barrier
  arrays!), vkCmdExecuteCommands, vkCmdCopyBufferToImage/ImageToBuffer
  regions, vkCmdClearAttachments, enumerations' out-arrays (safe-ish:
  count-driven writeback handled by arms or single-struct guests today),
  etc. Each warning = a future crash if a real game passes >1-element
  arrays through generic dispatch. Next milestone: GENERATE these
  marshals from vk.xml struct defs instead of hand arms.
- Verified: make vkxml-check clean (errors=0), opgen-thuck regenerated
  (1042 symbols), build 0 warnings, quick suite **207/207**,
  test_vulkan_swapchain rc=0 on live RADV.

## Session History (2026-08-23) — vk.xml milestone 2: generated deep-marshal (VK_CMD_DEEP)

- **vk.xml now GENERATES the marshalling** for the command-batch family:
  `tools/opgen/vkmarshalgen.py` → `include/opgen_vkmarshal.hpp`
  (11 struct descriptors + 13 command plans). Runtime consumer: new
  `Policy::VK_CMD_DEEP` in display_thunk dispatch — a two-pass staging
  marshal (dry size pass so VkStage reserves exactly once, then fill)
  that replaces guest array pointers with host pointers before the host
  call. All covered commands are INPUT-ONLY (command recording): no
  writeback. Descriptor fields carry {offset, elem kind
  (STRUCT/HANDLE/RAW), elem size, count-member offset} — counts are read
  from the element itself at runtime; len expressions are resolved to
  sibling param indices BY THE GENERATOR from vk.xml (no hand count
  indices — the hand table had an off-by-one on PipelineBarrier's
  memoryBarrierCount that the generator fixed automatically).
- **Layout engine in tools/opgen/vkxml.py validated byte-for-byte against
  the vendored vulkan_core.h**: 1698 struct layouts computed with natural
  LP64 alignment; a generated static_assert file compiles clean for the
  full command closure. Registry quirks handled: struct aliases (name+
  alias attr — alias structs have NO members and must resolve to their
  target), api="vulkansc" member duplicates (filter like params),
  bitmasks via child <name> typedef form (VkFlags=4B/VkFlags64=8B),
  unions = max(member) sizing with no pointer extraction, C arrays where
  the [N] suffix is SIBLING TEXT after <name> not inside it.
- Converted to VK_CMD_DEEP: SetViewport/SetScissor/PipelineBarrier (3
  barrier arrays)/ClearAttachments/ClearColorImage/ClearDepthStencilImage/
  CopyBuffer/CopyImage/CopyBufferToImage/CopyImageToBuffer/
  ExecuteCommands+WaitEvents+ResetFences (handle arrays via the
  kVkHandleElem sentinel descriptor).
- **CRITICAL lifetime contract**: the VkStage arena MUST live until after
  the host call — first version declared it inside the `if (plan)` scope
  and mambo_vulkan SIGSEGV'd walking freed staging. Declared at dispatch
  function scope next to the bounce arrays.
- Defensive caps: ≤1024 elements per array, ≤4 MiB staging, depth ≤4,
  garbage/unmapped counts fail the whole plan (falls back to generic
  single-element bounce — never worse than before).
- Warnings went 39 → 20 (remaining: cross-struct len expressions like
  GetAccelerationStructureBuildSizes' pMaxPrimitiveCounts, enumeration
  out-arrays needing query/fill semantics, GetQueryPoolResults OUT
  buffer — all documented future work).
- Verified: quick suite 207/207, mambo_vulkan + vulkan_swapchain rc=0 on
  live RADV (PipelineBarrier exercised by mambo found the lifetime bug).

## Session History (2026-08-24) — vk.xml milestone 3: generated pNext chains (A1) + OUT plans (A2) + C1 rows

- **Plan `docs/vulkan_migration_plan.md` phases A1/A2/C1(partial)/C3
  LANDED; A3/C2 deferred with recorded reasons; Phase B (17 hand arms)
  remains.** All gates green: build 0 warnings, opgen-thunk-check,
  vkxml-check errors=0 (warnings 20→6), quick suite **207/207**,
  swapchain + mambo rc=0 ×3 on live RADV.
- **A1 — GENERATED pNEXT CHAINS (hand sType table DELETED):**
  - `tools/opgen/vkxml.py` gained full VkStructureType extraction
    (`parse_structure_types` → `reg['stypes']`). THREE sources: core
    `<enums name="VkStructureType">` children; `<enum extends=...>`
    inside `<extension number=M>` (extnumber DEFAULTS TO M — the
    missing-default was the first bug); and inside `<feature>`
    promotion blocks (explicit extnumber=, container scan required).
    Alias chains resolved second-pass. VALIDATION: parsed map compared
    against ALL 1250 constants in vendored vulkan_core.h — zero
    mismatches. Two old hand-table entries were silently WRONG
    (COMMAND_BUFFER_INHERITANCE_INFO 11 vs real 41;
    DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO 1000060001 vs 1000060004) —
    harmless there, but proof the mechanical path beats hand tables.
  - `vkmarshalgen.py`: descriptors now emitted for plan-closure ∪ EVERY
    struct with a resolvable sType member (1479 chainable); a pointer
    FIELD does not force its pointee's layout to exist, so structs whose
    nested refs have no computable layout are PRUNED iteratively
    (VkDirectDriverLoadingListLUNARG funcptr member) and excluded from
    the sType map — runtime truncates guest chains at them (safe).
    latexmath count members (ShaderModuleCreateInfo codeSize/4 etc.)
    still skip that FIELD only (pre-existing).
  - Header: `VKM_PNEXT = 0x80` flag OR'd into VkFieldDesc.elem marks the
    pNext link; sorted `kVkStypeIndex[]` (1227 entries, dup-stype dedupe)
    + binary-search inline `vk_find_struct_by_stype(int32_t)`.
  - Runtime (display_thunk.cpp): `vk_marshal_pnext_chain` reimplemented
    over the generated map — CREATE_DEVICE / Properties2 / Features2 /
    BEGIN_COMMAND_BUFFER arms upgraded implicitly; writeback now SKIPS
    the pNext link bytes [8..16) (the staged HOST pointer previously got
    written back over the guest's own chain link — latent corruption);
    one-shot unknown-sType diagnostic (`vk_deep_unknown_stype_once`).
    VK_CMD_DEEP size/fill passes walk VKM_PNEXT-flagged fields via
    `vk_deep_chain_size`/`vk_deep_fill_chain` (input-only; ≤8 nodes).
- **A2 — OUT PLANS (VK_CMD_DEEP_OUT):**
  - `VkPlanRef` gained `out` + `elem_size`; generator derives direction
    AUTOMATICALLY: len-target sibling is a POINTER → out=1 enumeration;
    non-const data ptr → out=2 copyback-only; else out=0 IN. Scalar/
    enum/bitmask arrays stage as raw bytes with their type_size
    (VkDeviceSize* offsets = 8B elems, uint32 dynamic offsets = 4B).
    NOTE: flat POD structs (VkSurfaceFormatKHR 8B, VkQueueFamilyProperties
    24B) classify as raw staging via type_size BEFORE the layouts check —
    correct for flat structs, would SKIP nested-pointer translation if a
    chainable struct ever hit this path.
  - Runtime: enumeration = read guest *count_ptr → cap kVkDeepMaxElems →
    4-byte count bounce armed with staged count → host call → copy back
    min(staged, actual) elements AND actual count to guest. Count bounces
    allocate in the FILL phase (after reserve) — hard contract #2 caught
    a dangling-pointer version of this during self-review. Failed plans
    (garbage counts) restore original guest pointers before generic
    dispatch. Raw OUT jobs carry BYTES in j.count, struct jobs ELEMENTS.
  - Rows migrated → VK_CMD_DEEP_OUT: EnumeratePhysicalDevices,
    GetSwapchainImagesKHR, SurfaceFormatsKHR, SurfacePresentModesKHR,
    QueueFamilyProperties, GetQueryPoolResults. → VK_CMD_DEEP (IN):
    FreeCommandBuffers, FreeDescriptorSets, CmdBindDescriptorSets,
    CmdBindVertexBuffers, CmdUpdateBuffer, CmdPushConstants. NOT moved:
    extension-properties enumerations (RT filter hand arm),
    AllocateCommandBuffers (count nested in pAllocateInfo struct).
- **C1 rows added** (table 1042 → 1056): EndRendering(+KHR),
  DrawIndirect/DrawIndexedIndirect, ResetQueryPool, SignalSemaphore,
  GetSemaphoreCounterValue, AcquireNextImage2KHR (plain VULKAN);
  BindVertexBuffers2(+KHR), Set/ScissorWithCount(+KHR) (auto VK_CMD_DEEP
  plans). DEFERRED (need command-level recursive plans — arrays nested
  INSIDE pCreateInfo-style args): BeginRendering, PipelineBarrier2,
  QueueSubmit2, PushDescriptorSetKHR, update templates.
- **C3**: VK_EXT_descriptor_buffer hidden from device-extension
  enumeration (advertise ⟺ implemented). Update templates are
  core-promoted — hiding is moot, left alone.
- **DEFERRED decisions**: A3 nullify option lands WITH the pipelines
  migration (no consumer before it). C2 vkCreateAndroidSurfaceKHR is NOT
  small: Android guests enable VK_KHR_android_surface at instance
  creation while the host needs Wayland/XCB (or SDL_Vulkan_CreateSurface)
  — requires instance-extension rewriting FIRST plus an Android-Vulkan
  test guest; its own milestone.
- New contracts in plan file §Hard-contracts: (7) size pass mirrors fill
  allocation-for-allocation; unknown sType → NULL link + one-shot log.
  Count bounces never allocate pre-reserve.

## Session History (2026-08-24) — Phase B batch 1: create-style plans (5 hand arms deleted)

- **Create-style VK_CMD_DEEP plans landed** (`vkCreateShaderModule`,
  `vkCreatePipelineLayout`, `vkCreateDescriptorPool`, `vkCreateFramebuffer`,
  `vkCmdBeginRenderPass` migrated off their hand arms — five arms + five
  orphaned H structs DELETED, ~90 lines). New generator capability
  (CMD_CREATE_PLANS in vkmarshalgen.py) derives ref roles per parameter:
  const struct-ptr → out=4 SINGLE_STRUCT_IN, VkAllocationCallbacks →
  out=3 NULLIFY (A3 delivered as a plan role), non-const handle* →
  out=5 OUT_HANDLE. Runtime: single-struct refs stage via
  vk_deep_size_one/fill_elem with count=1 (pNext chains inside work
  automatically); OUT_HANDLE gets a zeroed 8-byte bounce armed in the
  fill pre-pass and copied back post-call by the existing copyback loop
  (staged_elems=1, elem_size=8).
- **latexmath len resolution**: `len="latexmath:[\textrm{codeSize} \over
  4]"` fields now recover the member identifier by regex, retry
  member_offset, and stage BYTE-granular (elem_size=1) so pCode stages
  its full byte count regardless of divisor. This also un-skipped
  VkPipelineMultisampleStateCreateInfo.pSampleMask (over-staging is
  harmless; host reads only ceil(samples/32) words).
- **Plan-failure sweep fixed**: a failed size pass now clears EVERY
  planned arg's vk_deep_done bit (jobs included), not just rec bits —
  previously a failed mixed plan could leave SINGLE_STRUCT args marked
  done and send raw guest pointers to the host driver.
- **Phantom C1 alias names FIXED**: vkCmdBindVertexBuffers2KHR /
  SetViewportWithCountKHR / SetScissorWithCountKHR do NOT exist in the
  registry (the aliases are EXT: extended_dynamic_state). The KHR rows
  were dead (thunkgen doesn't validate names; vkxmlcheck skipped them).
  Renamed to EXT + added EXT plans.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean,
  quick suite 207/207, swapchain + mambo rc=0 ×3 each on live RADV
  (both exercise all five migrated commands end-to-end).

## Session History (2026-08-24) — Phase B batch 2: RenderPass + DescriptorSetLayout generated

- **vkCreateRenderPass / vkCreateDescriptorSetLayout migrated to
  VK_CMD_DEEP create-style plans** — the attachment/subpass/reference/
  preserve-attachment trees and pImmutableSamplers handle arrays are
  consumed by generated SINGLE_STRUCT_IN recursion wholesale; two hand
  arms + seven H structs DELETED (~120 lines). The hand arm's ≤32 caps
  are replaced by the global kVkDeepMaxElems=1024 / depth≤4 caps.
- **scalar:N field bug FIXED**: typed scalar pointer fields
  (uint32_t* pPreserveAttachments) were emitted with elem_size=8
  (the 'scalar:N' elem kind was never decoded in vkmarshalgen field
  emission) → 2× over-staging; mostly harmless but a guest array at a
  page end would zero-fill on overread. Now byte-exact.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean,
  quick suite 207/207, swapchain + mambo rc=0 ×3 each on live RADV.

## Session History (2026-08-24) — Phase B batch 3: pipelines generated

- **vkCreateGraphicsPipelines / vkCreateComputePipelines migrated to
  VK_CMD_DEEP create-style plans** — two hand arms + 14 H structs
  DELETED (~110 lines incl. the whole frozen pipeline-state tree).
- **New generator/runtime capabilities:**
  - VKM_STR (0x40) field flag: NUL-terminated char* fields stage
    strlen+1 bytes via bounded vk_deep_guest_strlen (cap 256), mirrored
    in size and fill passes.
  - STRUCT_IN (out=4) unified: count_arg = len-sibling param index for
    struct ARRAYS (pCreateInfos), 0xFF sentinel = exactly one
    (BeginRenderPass/ShaderModule shapes). Count capped at
    kVkDeepMaxElems.
  - OUT_HANDLE (out=5) unified the same way: register-count handle
    ARRAYS get a zeroed bounce of count*8; copyback is gated on
    host ret==0 (on_success_only rec flag).
  - latexmath pSampleMask already byte-granular from batch 1 — stages
    rasterizationSamples bytes vs ceil(/32)*4 needed (harmless
    overstage; host reads only what it needs).
- Registry note: BOTH pipeline commands carry len="createInfoCount" on
  pCreateInfos AND pPipelines — no per-command overrides needed.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean
  (warnings 6→4), quick suite 207/207, swapchain + mambo rc=0 ×3 each
  on live RADV (both exercise graphics-pipeline creation end-to-end).

## Session History (2026-08-24) — Phase B batch 4: SUBMIT(+2) + descriptor-set pair; two marshal bugs fixed

- **vkQueueSubmit / vkQueueSubmit2(+KHR) / vkUpdateDescriptorSets
  migrated to auto-derived VK_CMD_DEEP plans; vkAllocateDescriptorSets
  via create-style plan with a NEW count source: count_arg=0xFE +
  VkPlanRef.aux = byte offset of the count member inside the staged
  STRUCT_IN struct (descriptorSetCount in pAllocateInfo). The OUT
  handle-array bounce is sized by reading that member from the guest
  struct at scan time.**
- **BUG A (type_size-before-layouts classification)**: the CMD_PLANS
  param scan checked scalar type_size() BEFORE the struct-layouts
  check — type_size() resolves struct names too, so ANY struct-typed
  array parameter was staged as FLAT RAW BYTES with its interior guest
  pointers untranslated (VkSubmitInfo → RADV derefs raw guest
  pCommandBuffers pointer → SIGSEGV). A2-era plans were unaffected by
  luck (all their structs are flat PODs). Fix: layouts check FIRST.
  This also explains phantom "need=72" undercounts during debugging.
- **Latent b2/b3 issue (superseded commits)**: the plan-failure sweep
  only cleared out-rec bits, leaving STRUCT_IN/NULLIFY args marked done
  on a failed size pass — raw guest pointers then reached the host via
  skipped generic translation. The batch-4 rewrite sweeps EVERY planned
  arg bit unconditionally. Intermediate commits dc02fe2/0a7c8b9 can
  crash intermittently when a plan fails; HEAD supersedes them.
- **PCWFC push hook**: vkQueueSubmit/vkQueueSubmit2 now push mapped
  bounces via a name check after deep staging (applies to both deep and
  generic fallback paths), replacing the deleted hand arm's inline call.
- Debug methodology note: stderr dispatch traces print BEFORE the host
  call, so "last traced command" attribution is reliable, but a stale
  binary masquerading as a bisect point cost an hour — verify the
  binary actually contains/excludes your marker (strings) before
  trusting a bisect result.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean
  (warnings 4), quick suite 207/207 (swapchain runs for real with
  DISPLAY set), swapchain ×5 + mambo ×3 rc=0 on live RADV.

## Session History (2026-08-24) — Phase B batch 5 (FINAL): instance/device generated; PRESENT stays behavioral

- **vkCreateInstance / vkCreateDevice / vkAllocateCommandBuffers migrated;
  SYNC_PULL arm dissolved** (vkWaitForFences → auto handle-array plan;
  WaitIdle rows → plain VULKAN + a name-gated POST-call pull hook:
  `ret==0 && name ∈ {vkDeviceWaitIdle, vkQueueWaitIdle, vkWaitForFences}`
  → vk_sync_pull_all). Four hand arms + five H structs deleted.
  Remaining hand arms: PRESENT (behavioral by design) + the memory
  family + BEGIN_COMMAND_BUFFER — all declared permanent/hand-coded.
- **New generator capability VKM_STRARR (0x20 | elem 0x60)**: counted
  arrays of NUL-terminated char* (ppEnabledExtensionNames &
  friends, len="enabledLayerCount,null-terminated"). Size pass sums
  slot-array + per-string strlen+1; fill allocates the slot array then
  every string. CRITICAL ORDERING: test STRARR BEFORE STR in both
  passes — STRARR's value CONTAINS the STR bit and the single-string
  branch would swallow it (staged one string where an array belonged).
- **out=2 direction now byte-vs-element aware**: byte counts come from
  size_t/VkDeviceSize siblings (dataSize); element counts multiply by
  elem_size (pResults-style typed OUT arrays).
- **Process landmines hit (all recovered):**
  1. Regex surgery on vk_deep_size_one/fill_elem corrupted both
     functions (mis-nested braces, spliced blocks) — rebuilt them from
     `git show HEAD:` extraction + clean insertions instead.
  2. The arm-deletion script's case list included VK_PRESENT,
     silently dropping QueuePresent into generic dispatch (flat bounce
     of PresentInfo → guest pointers to RADV → crash at the triangle-
     loop present). Symptom appeared TWO migrations away from the cause
     because the early present still passed. When deleting arms, NEVER
     delete switch cases for policies that keep a hand arm.
  3. Generator-side BISECT disables left in place made later crashes
     look like unrelated bugs (raw guest pointer derefs from my own
     debug dump reading a non-staged arg). Always re-diff the generator
     before interpreting crash sites.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean
  (warnings 4), quick suite 207/207, swapchain ×3 + mambo ×3 rc=0 on
  live RADV with the complete change set.

## Session History (2026-08-24) — vkQuake regression hunt: enum-sized-array layout bug FIXED

- **MAJOR LAYOUT BUG (silent, affects every enum-sized C array)**: vk.xml
  writes fixed arrays as `<name>driverName</name>[<enum>
  VK_MAX_DRIVER_NAME_SIZE</enum>]` — the parser's `\[(\d+)\]` regex only
  matched DIGIT literals, so driverName[256]/deviceName[256]/uuid[16]-
  style members parsed as SINGLE elements → structs UNDER-SIZED
  (VkPhysicalDeviceDriverProperties 32 vs real 536; Properties2 568 vs
  840). The A1 chain walker staged undersized nodes and RADV read
  garbage fields → vkQuake crashed in GetPhysicalDeviceProperties2
  before device selection. FIX: parse `[NAME]` suffixes through a new
  API-constants table (<enums name="API Constants">; skip float values).
  NOTE: itertext() strips XML tags, so the suffix is "[NAME]" not
  "[<enum>NAME</enum>]".
- **VALIDATION UPGRADE**: compiled static_assert of ALL 1698 generated
  struct sizes against the vendored vulkan_core.h. Result: 23 failures
  → after excluding C-BITFIELD structs (7 of them; vk.xml models
  bitfields as separate members which natural-alignment layout cannot
  express) → **3 remaining, all NV ray-tracing instance structures**
  (unreachable: RT extensions hidden). reg['bitfield_structs'] now
  carries the set; the marshal generator pops them from layouts so
  plans never reference them and guest pNext chains truncate safely.
- VkFieldDesc.elem_size widened u8→u16 (nested struct elements can
  exceed 255 bytes: VkExtensionProperties=260 etc).
- **vkQuake status**: with the fix it boots FURTHER THAN EVER — full
  RADV device init (correct Properties2 chains: DriverProperties 536,
  SubgroupSizeControl 32...), swapchain, compute+graphics pipelines,
  command-buffer recording — then hits the KNOWN nondeterministic
  crash during triangle-loop submit/present (RADV derefs a guest
  pointer recorded into its own IR: `cmpl $0x3ba38c32,(%rax)`,
  rax=0x24c1). Needs its own focused session with TID-attributed
  traces; suspects remain descriptor-update staging or an unmarshalled
  nested struct in the record path.
- Verified: build 0 warnings, opgen-thunk-check/vkxml-check clean,
  quick suite 207/207, swapchain ×3 + mambo ×3 rc=0 on live RADV.

## Session History (2026-08-24) — vulkan_pnext suite test (chain-query regression net)

- **New guest test `ctest_real/test_vulkan_pnext.c`** (26 checks,
  registered as `vulkan_pnext`, headless-capable, exit 77 without host
  Vulkan): closes the coverage gap vkQuake exposed — NO suite test
  touched the Properties2/Features2 chain path before. Drives:
  - Properties2 → DriverProperties → IDProperties three-node GUEST
    chain: driver must fill every staged node (vendorID, limits,
    driverID, NUL-terminated driverName/deviceName, deviceUUID), and
    the GUEST pNext links + sTypes must survive (regression for the
    writeback-stamped-host-pointers-over-guest-chain-links bug class).
  - Guard-canary u64s bracketing every struct catch over-staging /
    under-sizing writes (would have caught the enum-sized-array bug).
  - Unknown-sType truncation mid-chain: bogus node → safe truncation,
    core properties still filled, guest chain untouched.
  - Features2 → BufferDeviceAddressFeatures: feature fields must be
    clean bools (catches garbage staging).
- Verified on live RADV: vendor=0x1002 "AMD Radeon RX 7600 (RADV
  NAVI33)", driver="radv"; one-shot unknown-sType diagnostic fires for
  the deliberate bogus node; passes headless too.
- Suite counts now: full default **213**, --quick **208**.

## Session History (2026-08-24) — AllocBlock stomp forensics: surfaces array BULK-shredded; VK_MAP_MEMORY remap fixed

- **VK_MAP_MEMORY remap-offset bug FIXED** (display_thunk.cpp ~2006): a remap
  of an already-mapped VkDeviceMemory now returns the existing bounce ONLY
  when map_offset AND map_size match the recorded shape; any different shape
  is treated as implicit unmap+remap (push old bounce → host, untrack the
  window range, host-unmap, then build a fresh mapping). The old code
  returned the first bounce for ANY remap — with a LARGER size the guest
  indexed past the smaller tracked range straight into neighboring window
  allocations (guest-heap stomp vector). VkMapped is private to the impl
  class, so copy fields into plain locals under vk_maps_mu and do the
  push/untrack/host-unmap AFTER releasing the lock.
- **AllocBlock forensic state (issue still OPEN — paused by user):**
  - gdb watch on used_columns[0][0] (window+0x16905e0, cond <0) fires at
    guest pc=0x4742ac again; w=-911 (x1=x10=x14=0xfffffc71) every run;
    used_columns[0][0] went 111 → −799 this run.
  - `[x19]` at that stop is NOT the current surface (register-reuse in the
    giant inlined GL_SortSurfaces/AllocBlock blob) — do not trust it.
  - REAL model array found from cl (static @0x1d616b0) + offset 0x16230 →
    worldmodel m; qmodel_t offsets read off CalcSurfaceExtents disasm:
    vertexes@280, edges@296, surfaces ptr@344, numsurfaces@336,
    surfedges@360; medge_t/mvertex_t = 12 B stride. e1m1: 5274 surfaces at
    0x40780080.
  - **SCANNED THE WHOLE ARRAY: 1306 / 5274 surfaces are CORRUPT**
    (numedges float-bit-patterns like 0x42E80000, zeroed structs, negative
    extents), spread across ~1 MB of the array — a BULK overwrite /
    zero-and-rewrite of live model heap, NOT a single-field poison and NOT
    vertex data (pattern search against vertexes[] found no match).
    Consistent with mmap-range reuse zeroing (untrack/free_ranges_ first-fit
    + MAP_ANONYMOUS zeroing) hitting a STILL-LIVE allocation, or an
    out-of-bounds write through a stale/wrong bounce — i.e. allocator
    accounting or thunk write-path bug, upstream of GL_BuildLightmaps.
  - JIT-miscompile lead (a) is WEAKENED but not dead: corruption is bulk
    memory damage, so chasing SizeToBin x86 was dropped. CalcSurfaceExtents
    (0x480500–0x4806f0) FMA/frintm/frintp/fcvtzs sequence documented here
    if a value-level miscompile resurfaces later.
  - Tools preserved: /tmp/opencode/vkq/lmwatchB.py (one-run forensic: catch
    used_columns stop → read cl/worldmodel → scan all 5274 surfaces → dump
    payload + compare vs vertexes[]), lmwatchA.py (register dump variant).
    Run: `gdb -batch -x /tmp/opencode/vkq/lmwatchB.py bifrost-emu` from repo
    root (~2 min to fire under BIFROST_NO_ASLR=1).
  - NEXT (when resumed): (1) end-to-end test whether the remap fix alone
    cures AllocBlock: full (fixed binary never got a full run — user
    aborted; runs take >100 s to reach map load); (2) if not, find WHO
    frees/reuses the surfaces-array range: instrument Memory::untrack_
    allocation + mmap_alloc reuse (env-gated print when a reused/zeroed
    range overlaps a tracked-live allocation or when free_ranges_ first-fit
    picks a range containing 0x40780080..0x40877000); (3) audit remaining
    guest-write paths for size/offset errors (vkCmdUpdateBuffer staging,
    PCWFC push bounds).
- Verified today: build clean with the remap fix (0 warnings); no suite run
  (session ended early at user request).

## Session History (2026-08-24) — GL automation Stage 1: glxmlcheck + 6 real row bugs FIXED

- **`tools/opgen/glxmlcheck.py` landed** (`make glxml-check`, wired into
  `check-all`): mechanically audits every GL/GLES row in thunk_dp.txt
  against the Khronos gl.xml — arity (trailing-'i' elision allowed) +
  pointer-position agreement, same contract as vkxmlcheck for Vulkan.
  Registry VENDORED at `tools/gl-registry/gl.xml` (copied from the
  cargo khronos_api-3.1.0 crate cache — re-vendor if that crate is
  pruned; no egl.xml in that crate, so EGL rows are SKIP-listed for now).
- **First run found SIX real errors; ALL FIXED:**
  1. glGetShaderInfoLog / glGetProgramInfoLog (GL AND GLES rows,
     4 rows total) had `iiip` — the GLsizei *length out-pointer (param 2)
     was passed VERBATIM as an integer → host driver wrote through a raw
     guest address. Fixed to `iipp`.
  2. glCompressedTexImage2D had NINE arg tokens for the EIGHT-param
     function — pointer mask shifted, `data` reached the host driver as a
     RAW guest pointer (compressed uploads were broken end-to-end). Fixed
     to `iiiiiiiz` + NEW SizeKind::ARG6 ('arg6' in the spec; thunkgen enum
     + VALID_SIZE updated; dispatch case sizes the bounce from args[6]
     imageSize, 16 MiB cap like ARG1/ARG2).
  3. glDrawRangeElements had indices (a pointer when no VBO is bound)
     dispatched as a plain scalar. Row policy → EL_PTR; the EL_PTR arm now
     picks its arg index per name (glDrawRangeElements → arg5, default
     arg3) mirroring the VA_PTR precedent.
- Checker SKIP rules: policies {SHADER_SOURCE, MAP_BUFFER, UNMAP_BUFFER,
  FLUSH_BUFFER, EL_PTR, VA_PTR, GET_PROC, TF_VARYINGS} own their
  marshalling; DELIBERATE names {glGetString, glGetStringi} are
  intercepted. Warnings (91, non-fatal, WARN_CAP 40 displayed) = pointer
  params whose gl.xml len= marks dynamic size riding the default 64 KiB
  bounce — this IS the stage-2 promotion roadmap.
- Self-caught en route: my sed dropped one 'i' from the compressed-tex row
  and the checker's own arity guard flagged MY edit — the net works on the
  fisherman too. Also: never trust clangd LSP diagnostics in this tree
  (no compile_commands.json — g++ with Makefile flags is the arbiter).
- Verified: opgen-thunk regenerated (1058 symbols), opgen-thunk-check +
  glxml-check + vkxml-check + opgen-check all clean, build 0 warnings,
  quick suite **208/208**, test_sdl_gl_triangle rc=0, test_sdl_gl_modern
  17/17 on live DISPLAY=:0/RADV.

## Session History (2026-08-24) — GL automation Stage 2: core-GL coverage auto-expansion

- **`tools/opgen/glcoverage.py` landed** (`make glcoverage-check`, wired
  into `check-all`): generates missing core GL <=3.3 rows from the
  vendored gl.xml into a marked BEGIN/END block inside thunk_dp.txt
  (spec stays single source of truth; opgen-thunk-check pins the header).
  **428 new rows**, table 1058 → 1486 symbols.
- Candidate rules (conservative): api='gl' features number<=3.3 not
  already in spec; SKIP GLdouble/GLclampd by-value functions (78 — 'f'
  is binary32-only, doubles need a dedicated arg kind, future work);
  SKIP bulk pixel buffers whose len= references width/height/depth or
  COMPSIZE() (7: glDrawPixels/glBitmap/glTexImage1D/glTexSubImage1D/
  glTexSubImage3D/glPolygonStipple/glGetPolygonStipple — need real
  SizeKinds); ARGS = 'p' at '*' params else 'i'.
- GOTCHAS: (1) --check must EXCLUDE the autogenerated block when scanning
  existing spec names, else it sees its own rows as "already present"
  and regenerates an empty block → permanent DRIFT failure. (2) flag
  parsing must filter '--check' before positional args. (3) classify()
  must use its parameter, not a stale global (NameError).
- Verified: glcoverage-check + glxml-check (735 rows, errors=0,
  warnings=349 = dynamic-size roadmap) + opgen-thunk-check clean; build
  0 errors; quick suite **208/208** on the expanded table;
  test_sdl_gl_triangle rc=0 + test_sdl_gl_modern 17/17 on live :0/RADV.

## Session History (2026-08-24) — GL automation Stage 3: dispatch de-hand-armed

- **Three name-string dispatch arms became table-driven** (thunkgen +
  thunk_dp.txt + thunk.cpp):
  1. **PCWFC consumer list → SYNC column**: optional 7th spec column
     (`SYNC`) emits `Spec::sync_before`; dispatch pushes persistent map
     bounces before marked rows. Marked: all glDraw* EXCEPT glDrawBuffer/
     glDrawBuffers (old prefix `compare(0,6,"glDraw")` over-synced those
     two harmlessly — precise set now; over-pushing was always safe),
     glGetBufferSubData, glCopyBufferSubData, glTexBuffer,
     glDrawElementsInstancedBaseVertex, glDrawRangeElementsBaseVertex.
     NOTE glTexBufferRange is NOT in the spec at all (was dead in the old
     name list too). PARSER RULE: a trailing literal `SYNC` token strips
     BEFORE the last-three-tokens RET/POLICY/SIZE read — both thunkgen.py
     and glxmlcheck.py do this.
  2. **glGetString/glGetStringi → GET_STRING policy** — arm branches on
     the ARGS string ("i" vs "ii"); exact-prototype calls + string cache
     unchanged. Removed from glxmlcheck DELIBERATE (policy owns it).
  3. **glDeleteBuffers → DELETE_BUFFERS policy** — post-call mapping
     cleanup arm keyed by policy instead of name.
- **Two latent bugs fixed en route** (found while marking SYNC): the
  autogenerated glDrawElementsInstancedBaseVertex ('iiipii') and
  glDrawRangeElementsBaseVertex ('iiiiipi') marked indices as ALWAYS-
  translate 'p' — wrong when a VBO is bound (offset, not pointer).
  Converted to EL_PTR ('iiiiii'/'iiiiiii'); EL_PTR arm's per-name pi map
  gained DrawRangeElementsBaseVertex→arg5.
- thunkgen plumbing: VALID_POLICY += GET_STRING/DELETE_BUFFERS;
  Spec struct + row emission carry sync_before; module DOCSTRING updated
  (the HEADER template struct is separate — updating only the docstring
  compiles fine but leaves the field missing; caught by grep).
- Verified: opgen-thunk-check / glcoverage-check / glxml-check clean
  (735 rows, errors=0); build 0 warnings; quick suite **208/208**;
  test_sdl_gl_triangle rc=0, test_sdl_gl_modern 17/17 (GET_STRING +
  EL_PTR draws), test_sdl_gl_mapbuffer 21/21 (DELETE_BUFFERS + PCWFC)
  on live DISPLAY=:0/RADV.

## Session History (2026-08-26) — neverball demon ROOT-CAUSED and FIXED: W-form CBZ/CBNZ 64-bit test

- **The neverball heap corruption is SOLVED.** Root cause: `BRCOND_ZERO`
  (W-form `cbz/cbnz`) emitted a 64-bit `test rax,rax`, so dirty bits
  [63:32] flipped the branch. glibc's hand-written NEON
  `__strlen_asimd` keeps fold state in x3's upper half across
  `cbnz w3` (a deliberate 32-bit test); under JIT the branch took the
  wrong path for alignment-dependent string layouts → strlen returned
  SHORT lengths → malloc/strcpy size mismatches shredded guest heap
  metadata ("malloc(): invalid size (unsorted)" / "free(): invalid
  next size (fast)") ~8 s into neverball's menu load.
- **Hunt methodology that landed it** (all tools kept): (1)
  differential strlen hash probe (glibc-dynamic guest, JIT vs interp);
  (2) alignment sweep — failures were exactly al=9..23 returning
  chunk-relative lengths; (3) isolated-op inline-asm probes cleared
  uminp/cmeq#0/shrn/fmov/rbit/clz individually (interp==JIT), killing
  the SIMD-suspect theory; (4) minimal dirty-upper `cbnz w3` probe
  reproduced the misbranch (JIT taken=1, interp taken=0). Verify-mode
  lesson: the earlier `[VERIFY] PC DIVERGENCE` at __strlen_asimd+0x38
  was REAL, not non-idempotency noise — do not dismiss PC divergences
  inside hand-written guest asm.
- **FIXES (both verified)**: (1) translator records `sf` on
  BRCOND_ZERO/TST insts; both codegen sites
  (jit_codegen_branch.cpp BRCOND_ZERO + jit_tier2.cpp BRCOND_ZERO)
  emit `mov eax,eax` before the test when sf=0; frostjit.cpp TST emits
  a true 32-bit TEST for W-form ANDS/TST/BICS (N flag from bit 31, was
  stuck at 0 via REX.W SF-from-bit63 — every b.mi/b.pl/csel-mi after a
  W logical test misbranched).
- Verified: p12 probe correct; slprobe3 sweep 0 wrong (was 15);
  strlen hash == interp (49680eac41dc0249); neverball exits CLEAN
  (exit 0, zero crash signatures, first time ever under JIT);
  quick suite **208/208 + 1 env skip**; bench_mips acc
  `0xf800800a2c4ff835` unchanged (its hot loop is a CBZ self-loop with
  deferred pins — unaffected by the mask).
- Forensic tooling built this session: window-base discovery recipe
  (scan exec/anon maps for `49 BA` movabs-imm64 whose alias shows the
  ELF header bytes — /proc maps region-splitting defeats naive base
  guessing; R10-at-syscall-entry does NOT work, R10 is clobbered);
  leftover TEMP DEBUG probes inside deliver_signal (signal.cpp ~576+)
  SEGFAULT on unmapped allocation holes during abort handling — read-
  only probes but they hard-fault via Memory::read's fast path;
  cleanup candidate. GraphicThunk full-bounce writeback (thunk.cpp
  ~2346, 64 KiB stomp on above-window pointers, DisplayThunk-fixed
  class) remains a LATENT shredder; four unguarded 4 KiB ifunc/init
  scratch stacks remain the top live hazard (audit-only findings).

## Session History (2026-08-25) — commits split; check-all "segfault" was STALE BUILD ARTIFACT

- **All multi-session uncommitted work split into 4 commits**: mem
  allocator hardening / vk remap+vk.xml layout+vendored registries /
  GL automation stages 1-3 / docs.
- **`make check-all` test-capi SIGSEGV chased to its lair and it was
  NOT a code bug**: a clean `rm -rf build && make` rebuild fixes it
  (54/54). Days of refactoring + partial incremental builds left
  stale objects under build/ that -MMD deps did not fully catch up
  after header/member changes (Memory gained a member mid-series).
- LESSON (contract): after ANY multi-file refactor series — especially
  struct member additions in core headers — do `rm -rf build && make`
  BEFORE trusting check-all/test-capi results. A deterministic-looking
  segfault can be pure build staleness; verify with a clean rebuild
  FIRST (cheap) before source-level bisection.
- ctest/test_capi.c version check de-hardcoded ("1.5.4-alpha" literal →
  shape check) so version bumps stop breaking it.
- Verified at final HEAD: full check-all = suite **213/213**, capi
  **54/54**, nb **61/61**, all five generation guards clean.

## Session History (2026-08-25) — neverball guest-heap corruption: PRE-EXISTING JIT wild-write (same family as vkQuake AllocBlock)

- **GL leftovers batch landed (commit 5142851)**: 7 bulk-pixel SizeKinds
  (DRAWPIXELS/BITMAP/TEXIMAGE1D/TEXSUBIMAGE1D/TEXSUBIMAGE3D/STIPPLE),
  +65 GLdouble-by-value rows via the already-native 'd' kind
  ('d' was plumbed all along — glOrtho used it; only mixed int+double
  shapes stay skipped), and the mixed-FP dispatch gained the
  (2 ints, 4 floats, pointer) glBitmap shape with a padded bounce.
  Table 1493 → 1558 symbols; guards clean; suite 208/208.
- **NEW DEMON (PRE-EXISTING, verified at parent a08ec46): neverball
  aborts with GUEST glibc "malloc(): corrupted top size" under JIT.**
  The corruption is in GUEST heap (guest libc aborts), written by
  JIT-generated stores:
  - Repro: `cd rootfs/usr/games && DISPLAY=:0 BIFROST_ROOT=<repo>/rootfs
    LD_LIBRARY_PATH=<repo>/rootfs/usr/lib/aarch64-linux-gnu timeout -s
    KILL 60 <repo>/bifrost-emu neverball` — dies ~8 s in (after GL
    context + extension print, during menu texture load).
  - `--no-jit` survives indefinitely BUT the menu stays WHITE (separate
    cosmetic issue: textures not rendering under interp — unknown cause,
    possibly a thunk path that silently no-ops without JIT).
  - ASAN build does NOT flag it (guest window memory isn't host-malloc'd)
    and the corrupting write is JIT code = invisible to ASAN.
  - Feature gates ALL still corrupt: NO_SELFLOOP, NO_PIN, NO_FLAGSKIP,
    TIER2=0, NO_DIRECT_CALL, REGALLOC_CHECK, NO_FP_CACHE, NO_CHAIN —
    core codegen or an un-gated path.
  - BIFROST_JIT_VERIFY=1 logs ~24 divergences; blocks cluster in
    ANONYMOUS executable guest memory (~0x194xxxxx / 0x225a7ec in one
    run's layout) — NOT any file-backed module. Suspected runtime-
    dlopened NSS/gconv modules (glibc NSS activity confirmed by the
    crash dump's NSSMOD probes). Divergence samples: x2 jit=0x10400000
    ref=0x10400800; x0 jit=0xffffffff ref=0x7fffff; a SIMD v_lo[0..3]
    divergence at 0x19483340 adjacent to 0x194831cc. CAUTION: verify
    re-executes blocks through the interpreter, so SVC-bearing anon-code
    blocks may be non-idempotent false positives.
  - Module attribution recipe (works): run bifrost-asan (-O1 -g build)
    under gdb, break jit_dispatch.cpp:703, walk
    emu.dyn_linker_ raw pointer → DynamicLinker::objects_ for bases
    (/tmp/opencode/attr4.gdb). NOTE guest lib bases are heap-ASLR'd per
    run — attribute within the SAME run.
  - **This is almost certainly the same disease as vkQuake's AllocBlock
    surfaces-array shredding** (bulk guest-heap damage, JIT-only,
    interp-clean). One hunt kills both. NEXT SESSION: identify the anon
    exec region definitively (attribute within ONE verify run), disassemble
    the divergent block, BIFROST_JIT_DUMP+BIFROST_DUMP_PC it, diff against
    interp semantics. Also check whether the white-menu-interp issue is
    the same block failing silently.

## Session History (2026-08-25) — JIT heap-corruption hunt II: shift+ADCS fixed, MEMFULL built, culprit narrowed

- **TWO REAL JIT MISCOMPILES FIXED (both value-dependent, both verified):**
  1. **32-bit variable-shift count masking**: LSLV/LSRV/ASRV W-forms took the count mod 64
     (`and rcx,0x3f` + REX.W) instead of mod 32 — any W-shift with count ≥ 32 miscompiled.
     glibc `_int_malloc`'s binmap update `lsl w8,w2,w8` with bin idx 107 produced 0 instead of
     0x800 → shredded binmap → overlapping chunks → "corrupted top size"/"invalid size".
     Fix: translator sets `width=32` for all !sf variable shifts (ir_translate.cpp:~343);
     codegen emits true 32-bit forms (count hardware-masked mod 32, upper container zeroed).
     This bug was LIVE during all previous bisection sessions and POISONED THEM (any single-gate
     fix was masked). `_int_malloc` verify divergences gone after fix.
  2. **ADCS/SBCS width hardcoded 0** (ir_translate.cpp ~207): every 32-bit adcs/sbcs ran as
     64-bit ADC/SBB → x86 CF from bit 63 (never set for W inputs) → ARM C stuck at 0.
     Fix: pass `d.sf ? 64 : 32`. Regression tests added to ctest/jit_carry.c
     (adcs32_chain/sbcs32_chain via inline asm with early-clobber &; test VERIFIED to fail
     on buggy build, pass on fixed). NOTE: adcs w does not appear in neverball/libc/libpng/zlib,
     so this was latent, not the neverball killer.
- **Also fixed:** alGenBuffers/alGenSources wrote 8 bytes into 4-byte-strided guest arrays
  (wr64→wr32); glMapBuffer access ENUM (GL_WRITE_ONLY=0x88B9) failed the GL_MAP_WRITE_BIT test →
  unmap dropped all writes (normalize legacy enums); opgen.py/fpgen.py parsed IRSUB column as hex
  ("10"/"11" → 16/17) breaking SQRDMULH_ELEM/SQDMULL_ELEM subops (+ will-call-interp mirror mismatch);
  Memory::untrack claim in external audit was FALSE (sizes already consistent).
- **NEW TOOLING (all env-gated, kept):**
  - `BIFROST_JIT_VERIFY_EVERY=N`: re-verify each block on its first N run_block dispatches
    (default 1 = old behavior). Data-dependent miscompiles pass first-dispatch verify.
  - SIMD_ST16 stores tracked in store_infos (width 16) + memverify covers them.
  - `BIFROST_JIT_VERIFY_MEMFULL=1` + `BIFROST_MEMFULL_START/END`: full-range byte diff of
    guest memory between JIT post-state and interpreter re-run state, with jit/ref hexdump.
    THIS WORKS AND IS THE PRIMARY WEAPON. Range must be mapped or it silently disarms
    (armed/fail prints added).
  - `BIFROST_WRITE_TRACE=<file>`: logs every Memory::write (addr,size,tid) — CAVEAT: JIT inline
    window stores BYPASS Memory::write, so JIT-side streams are incomplete; diffing JIT vs INTERP
    write streams is therefore NOT meaningful past the engine split point (~line 13839 =
    end of loader/init-array phase where BOTH runs use the interpreter).
  - `BIFROST_ALLOC_TRACE=1`, `BIFROST_SCRATCH_TRACE=1`: allocation/placement prints.
- **VERIFICATION-BLIND-SPOT LESSONS (all confirmed empirically):**
  - Chained entries bypass run_block entirely → never verified regardless of VERIFY_EVERY.
    ALWAYS combine deep verify with BIFROST_NO_CHAIN=1 BIFROST_NO_SELFLOOP=1.
  - Blocks containing BL_CALL produce STRUCTURAL false positives under verify (JIT swallows the
    callee; ref steps N instructions) — expect PC divergences + register noise from them;
    do not chase. Same for blocks entered mid-function.
  - Non-idempotent memory (e.g. __libc_start_call_main's TLS sp-chain self-modification at
    tpidr-0x628/-0x620) produces plausible-looking MEM divergences — check idempotency before
    chasing.
  - __libc_start_call_main block @ startup: v_lo[3..5] "garbage vs zeros" divergence is the
    whole-game-inside-jit_call_helper structural artifact. NOT a bug.
- **EXHAUSTIVELY ELIMINATED as the neverball/vkQuake corruption source** (neverball dies
  ~8s in, glibc "malloc(): invalid size (unsorted)" / "free(): invalid size"; interp clean):
  tier2 regions, chains, self-loops, pins, flag-skip, vec-cache, fp-cache, direct-call,
  thread/shared-JIT modes, DSE, opt, exitchain, wex, MALLOC_INTERP, audio pump, MRS,
  window aliasing, CALL_INTERP handoff, vec prologue, ADCS (pre-fix it WAS broken but not
  reachable), audio-device-open path (FAIL_OPEN still corrupts). Deep verify (every block,
  200 dispatches, full 64MB heap diff) catches NOTHING yet the game dies ⇒ the corruption
  occurs (a) beyond verification coverage, (b) outside the diffed range (stack 0x3effxxxx /
  heap >0x14000000), or (c) in HOST-SIDE writers invisible to replay (thunks/signal frames/
  borrow-runner) — though pure-host-writer theories conflict with gate-independence.
- **CONFUSION SOURCES TO NOT REPEAT:** BIFROST_NO_ASLR omitted in some runs → library bases move
  (libc seen at 0x1062d000/0x19ce2000/0x2f928000) — ALWAYS set NO_ASLR for cross-run comparisons.
  neverball's own /root/.neverball/neverball.log APPENDS across runs → lseek(SEEK_END) sizes
  differ benignly; truncate before differential runs. --no-jit CLI arg shifts argv → prefer
  BIFROST_NO_JIT=1 env for identical argv. gdb disables HOST ASLR → JIT code-buffer addresses
  stable across gdb runs but NOT comparable to non-gdb runs. /tmp fills up with traces
  (46M-line write traces) — clean before builds.
- **NEXT SESSION PLAN (in order):**
  1. Extend MEMFULL range to cover the guest STACK band (0x3E000000..0x3F000000) and heap above
     0x14000000; accept stack false-positives by filtering sp-relative deltas.
  2. Identify the ANON-EXEC module whose blocks diverge (seen at 0x25d6388/0x2602220/0x28b2dd1c
     in various runs — late-dlopen'd NSS/gconv/glib module?) — correlate with DYNLINK_TRACE.
  3. If MEMFULL still silent at higher coverage: instrument JIT inline window stores behind an
     env flag (trace hook in the ST16/LD16 fast paths) to make JIT stores visible to traces.
  4. Re-test vkQuake AllocBlock with the guarded callback stacks (128KB, PROT_NONE guards via
     Memory::mmap_alloc_callback_stack) — pump-thread scratch overflow was suspected and fixed
     defensively but did NOT cure neverball alone.
- **GUARDED SCRATCH STACKS LANDED (defensive, kept):**
  Memory::mmap_alloc_callback_stack(usable) = mmap_alloc + mprotect PROT_NONE guard pages both
  sides; call_guest_function and the wire_thunk_glfw_cb_runner site now allocate 128 KiB guarded
  stacks (were 8 KiB mid-heap, overflow-prone). The pump thread writes PCM bounces adjacent to
  these — bounce placement vs guard interaction produced one loud SIGSEGV (progress: silent
  corruption → loud fault). Four more scratch sites (4096B IFUNC resolvers, 8192B
  set_guest_call_args at emulator.cpp ~1674) remain unguarded-but-shallow.

## Session History (2026-08-25) — static contract audit: EXTR/BFM/CLZ fold + thunk fixes

- **Two parallel READ-ONLY subagent audits** (translator↔codegen IR field contracts; thunk
  guest-writes + generator parsing) found **five more definite bugs**, ALL FIXED same session:
  1. **EXTR W-form unmasked Xm** (ir_translate.cpp ~412/416): the SHL/SHR decomposition of
     `extr w` fed FULL 64-bit Xm into the low-part SHR — garbage bits [32:63] OR'd into the
     32-bit result. Fix: explicit `d.sf ? 64 : 32` width on all decomposition SHL/SHR emits
     (the fixed W-shift path masks count mod 32 + zeroes upper container).
  2. **BFM insert W-form unmasked Rn** (~446/462): identical disease in the rotate
     decomposition — garbage Xn[32:63] landed INSIDE the mask window of `bfi w`. Same fix.
     These two are prime candidates for the neverball/vkQuake corruption itself (dirty-upper
     W-form bitfield ops are everywhere in game code) — retest pending.
  3. **CLZ constant-fold ignored width** (ir_optimize.cpp): folds always computed 64-bit CLZ;
     ARM `clz w` = 32 for zero and counts from bit 31. Reachable via XZR sources and tier-2
     FWD propagation. Fold now width-aware; dead RBIT/REV16/REV32 folds deleted (never
     emitted — SWAR-decomposed in ir_lower.cpp).
  4. **pa_simple_new double-dereference** (audio_thunk.cpp ~945): treated PulseAudio's
     `int *error` as `int **` → rd64(errp) then write-through-garbage-pointer = arbitrary
     guest-memory corruption class. Now `wr32(I, errp, 0)` directly.
  5. **SDL_PauseAudioDevice off-by-one** (~698): `name[15]=='D'` always false ('D' is [14])
     → device handle ignored, paused wrong device.
- Hygiene: CLZ native emission gated on has_lzcnt() (BSF-decode silent corruption on non-ABM
  CPUs); scalar SIMD SHL unallocated encoding now falls to CALL_INTERP instead of silent
  shift-by-0 NOP; glxmlcheck.py literal-\n comment bug (DELETE_BUFFERS/SHADER_SOURCE never
  exempted); vkxmlcheck/eglcheck now strip SYNC column; vkxml.py hex value= crash;
  opgen.py dead var removed.
- **Regression tests**: ctest/jit_bfext.c (8 checks: extr W dirty-upper, bfi W dirty-upper,
  clz w/x zero+one, positive sanity) — inline asm with volatile inputs AND early-clobber "&"
  outputs (GCC legally aliased an input into an early-written output without &). Registered
  in run_tests.sh after carry. Suite totals are DYNAMIC — no count bumps needed.
- **AUDIT METHODOLOGY (keep for future)**: spawn parallel read-only explore subagents with a
  table-driven contract spec ("for each IROp: fields codegen consumes vs what every emit site
  passes") — found 5 real bugs in one pass with zero runtime debugging. Clean-audited list
  covers ADDS/SUBS/ADCS/SBCS/SBFM/UBFM/CSEL/CCMP/UDIV/SDIV/MUL-family/LDP-STP/SIMD families/
  FP families/thunk write sizes/generator parsers — see session transcript for full table.
- Validation: clean rebuild, bfext 8/8 under JIT + interp, carry still PASS,
  quick suite 209/209, all five generation guards clean.
- Neverball still dies ~8s with "malloc(): invalid size" (pre-existing demon, unchanged by
  these fixes) — the dedicated hunt continues separately (see earlier entry today: MEMFULL
  verifier, VERIFY_EVERY, elimination list).

## Session History (2026-09-02 22:56 EDT) — shared crash reporter + AGENTS.md split

- new `src/core/crash_report.{h,cpp}`: one short `[crash]` line per fatal
  guest crash (reason, pc + module via `find_object_by_addr`, sp/x30/fault)
  plus `last=` line (last syscall/thunk name + pc). last-call record lives
  per-vCPU in `CPU` (set in `Emulator::syscall` and `jit_thunk_svc`, reset
  on clone); `syscall_name_for_crash()` exposes names (+ thunk + readv/
  writev/pread64/pwrite64 gaps filled). detail (regs/stack/fp chain) only
  with `BIFROST_CRASH_DUMP`. decode throw site quiet by default (SIGILL-
  handler guests stay silent); run-loop/thread no-handler branches log.
  new flags in `debug_flags.h` (`CRASH_DUMP`/`DBG_GUARD`/`TRACE_CRASH`),
  ad-hoc `getenv()` uses in jit_glue/interp removed. verified: interp UDF
  smoke shows `sigill` + `last=writev(66)` lines, rc=132; suite 208/208.
- AGENTS.md slimmed 4398 → ~1070 lines: all 65 session entries moved here
  (`docs/session_history.md`, order preserved); live contracts (incl. the
  crash-reporter note) stay in AGENTS.md. committed as
  `8126b7a` (reporter) + `515a805` (module + last-svc).

## Session History (2026-09-02 23:56 EDT) — neverball null call was missing rootfs libs

- neverball died at startup with `decode error pc=0` (x30=0x41a634,
  last=thunk sym=280). same death under `--no-jit`, so NOT a JIT bug.
  root cause: `find_library` never searched `$ROOT/usr/lib/aarch64-linux-gnu`,
  so libSDL2_ttf/vorbis/openhmd fell through to an empty host thunk (TTF_*
  resolved NULL, 0/0 symbols) and the game called NULL. fix: search the
  rootfs multiarch triplet dirs (6-line change in `dynamic_linker.cpp`).
  SDL2 itself stays host-thunked (absent from rootfs multiarch — no
  behavior flip). after the fix the game runs 2+ minutes clean (it swallows
  SIGTERM via signal forwarding, so kill by PID not `timeout`).
- side findings (verify-harness work): fixed 16-byte snapshot truncation,
  svc/call/unresolved-store quarantine, pc-split containment, MEMFULL
  self-overwrite, v_lo-high canonicalization on single fp stores — verify
  sweep 31/31 silent. one `test_dlopen_mt` 139 seen once under suite load,
  green on rerun (flaky, not chased).

## Session History (2026-09-03 00:35 EDT) — taken-path GPR flush (zlib x19/x22)

- real JIT bug, caught by the hardened checker: `emit_taken_path_epilogue`
  never flushed dirty GPR vregs (the vec-cache sibling got its `false` flag
  long ago; GPRs were forgotten). values computed before a conditional
  branch (zlib inflate's umov x22/x19 feeding a CBZ) were dropped on the
  taken exit while the fall-through epilogue still wrote them — the
  successor read stale homes. one-line fix (`flush_all_vregs()` in the
  taken epilogue, cold path only). zlib block healed, suite 209 green,
  neverball 95 s with zero fatal lines.
- OPEN (benign, parked): glibc memcpy-loop block shows deterministic v_lo
  disagreements (all four lanes read exactly one loop advance stale)
  with matching GPRs, clean codegen, and no live effect (game completes).
  standalone ldp/umov/memcpy/overlap repros all pass; NOT a umov, cache,
  tier2, or snapshot artifact (each ruled out by experiment). trail in
  transcript; resume with per-lane ground-truth print if it ever bites.

## Session History (2026-09-03 00:39 EDT) — Wayland host bridge (real compositor)

- DisplayProxy talks to a real compositor now: `wl_display_connect`
  upgrades to host libwayland when reachable (stub fallback headless);
  get_fd/flush/dispatch/pending/roundtrip/read/prepare/cancel +
  proxy_destroy forward. `wl_proxy_marshal(_constructor[_versioned])`
  decode guest varargs from the `opgen_wl` tables (new `created` field
  for the created type; bind special-cased — its new_id carries no
  interface in the XML) and forward through the host array forms;
  created objects map to guest handles with interface+version tracked.
  `wl_proxy_add_listener` stores guest fn tables; host registry
  trampolines queue events, delivered after dispatch via a borrow-CPU
  runner wired like the GLFW one. fd-passing stays unsupported.
  `wlgen` gained `event_count` + `created`; `wl_display_get_registry`
  row added (it is inline upstream — no host symbol to dlsym).
  `test_wayland_bridge.elf` does connect → registry → 67 globals →
  bind compositor → create surface → destroy → disconnect, all
  asserted. suite 210 green.

## Session History (2026-09-03 02:16 EDT) — dlopen_mt thread-safety + Wayland input

- `test_dlopen_mt` crashed ~50% (silent host SIGSEGV, worse under load).
  gdb caught free() inside iterate_phdr: guest callbacks mutate
  objects_ while iterating it (use-after-free on vector reallocation).
  fixed with a pre-callback snapshot. second crash (operator new in
  mmap_alloc) was the borrow-CPU scratch stack leaked per call (OOM
  march); now thread-local reused. BIFROST_DYNLINK_TRACE moved into
  debug_flags.h (contract: no ad-hoc getenv in hot contended paths).
  12/12 under parallel-build load + suite green.
- Wayland gaps closed: fd-passing via fd-resolver plumbing, seat
  pointer/keyboard/callback listeners with queue-then-deliver, get_class
  via string bounce, real surface commit forwarding. ALMOST derailed by
  popping the predecessor's leftover stash into the tree (cross-wire
  xchg experiment) — removed; their WIP stash left untouched.

## Session History (2026-09-03 07:27 EDT) — Wayland input end-to-end (guest fd, no phantom SDL, X11 queue, flush)

- Symptom: Wayland window ignored cursor/keyboard, compositor eventually
  flagged it not-responding. Four ranked causes found: (1) get_fd handed
  out the raw host number, colliding with guest fds so guest poll waited
  on the wrong object; (2) every bridged connect spawned a phantom SDL
  window that was never pumped; (3) X11 proxy input was stubbed
  (XPending=0, events drained and dropped); (4) commit/ack/pong queued
  without flush, starving the compositor under dispatch_pending-only
  guests (the test already documented this one).
- Fixes (405a6eb): get_fd publishes a real guest fd (dup + HostNode +
  allocate, cached per display, dropped from the cache on disconnect);
  wl_display_connect tries the host bridge first with no SDL, thunk skips
  SDL init for the 17 bridge symbols; X11 proxy queues SDL key/mouse/
  wheel/close as 192-byte XEvents (pending/nextevent/checkmask/queued
  all pump first, present() feeds the queue instead of dropping);
  wl_flush_all_ after surface commit, xdg ack_configure, xdg pong.
  Display/window attribution via x_last_display_/x_last_window_.
- The test app itself was half the bug: its live loop did
  dispatch_pending + flush + sleep, but dispatch_pending never reads —
  clicks sat in the kernel buffer. Now runs the real client cycle
  (pending -> flush -> prepare -> poll(wlfd, 200ms) -> read/cancel).
  Loop converted to CLOCK_MONOTONIC wall-clock (iteration counting
  finished early under input), prints once per real second, capped at
  20s. Verified live: motion/button/key count up after clicking the red
  window. Non-interactive part still ALL PASS.

## Session History (2026-09-03 07:40 EDT) — graphics thunk review round

- Subagent audit of thunk.cpp/gl_state/graphics/input (17 claims);
  every one re-verified against the code before touching. Fixed (6e57400):
  display-mode write used `m` not `src` (SDL3 bool flavor = write from
  0x1); glBitmap arm asked ni==3 but iiffffz yields ni==2, bits now from
  x2 per AAPCS64 (was dead no-op reading iv[2]==0); RWFromMem nullptr
  guard; SDLVK_EXT count cap 64; SDL_calloc overflow check; init recheck
  under lock; input modifier mask index-based (was shift-by-keycode UB,
  broke headless build too); TEXTURE_BINDING_2D query key 0x8069 ->
  TEXTURE_2D — the old test passed the get-pname as bind target and
  masked it, fixed the test to 0x0DE1; COLOR_WRITEMASK (bool+int)
  forwards to host; clear-color int query uses lround like DEPTH_RANGE.
- Deliberately left: dispatch-path races (string-cache ring, glfw maps,
  host error string, persistent snapshot window, per-process statics) —
  needs a locking pass with a threaded repro, not a drive-by;
  DrawRangeElements BaseVertex rows lack EL_PTR policy (offset-vs-pointer
  under bound EBO) — spec change + regen with a game repro;
  GetProcAddress unbounded strlen over the window alias (contiguous
  mapping, crash unlikely).
- Verify: make clean, opgen checks clean, quick suite 211 pass / 0 fail /
  1 env skip; wayland bridge/input, gl_state, sdl triangle all PASS.

## Session History (2026-09-03 08:22 EDT) — draw-row EBO policies + dispatch races (plan GO)

- Draw rows: 7 single-draw rows (2 GL + 5 GLES) spelled indices `p`/`-`
  mistranslated nonzero EBO offsets; now `i`/EL_PTR. Two review
  detours paid off: (1) dlsym serves the global table first-wins, so a
  GLES-second test silently exercises GL rows — new test_sdl_gles_ebo
  dlopens libGLESv2 FIRST (offset-12 readback: FAIL pre-fix, PASS
  post-fix, proven both ways); (2) indirect commands are 20 bytes, so
  cmd2 sits at offset 20, not 16 — the native host probe agreed with the
  thunk, the scenario was wrong.
- New EL_PTR_ARRAY (multidraw nest: outer bounce + per-element
  offset-vs-client) + INDIRECT_PTR (raw iff DRAW_INDIRECT_BUFFER bound,
  now tracked) policies; new GL indirect rows. Negative control for
  indirect: `-` rows fail (bounce addr as offset), INDIRECT_PTR passes.
  Multidraw+EBO is accidentally correct pre-fix on core (outer bounce
  contents + inners-as-offsets); the test guards the new arm. vao test
  carries multidraw + 2-command indirect (offset 20) readbacks; suite
  grew 211 -> 214, all green.
- Races: leaf state_mu over string-cache ring (slot-select under lock,
  mem->write outside), tex sizes, glfw maps, error slot; deliver
  snapshots under lock with guest/host calls outside; display-mode cache
  moved off function-statics (cross-instance leak); error trampoline
  locked (cross-instance routing stays documented single-emulator
  limit). Phase 3: getproc alias capped at 255B (window is a full
  reservation, reads can't fault) + bounce fallback; persistent push
  re-validates liveness under mu. New test_thunk_mt race smoke
  (4x500 proc-address stability + string paths, 3/3 clean runs).

## Session History (2026-09-03 08:26 EDT) — plan GO: EBO policies, races, hardening

- Phase 1/1b all committed (5f755d2, 1bb8a60, c628d7a) + history (8f505c7).
  check-all: 219 pass / 0 fail / 1 env skip (suite grew 211 -> 214 with
  gles_ebo, vao, thunk_mt, then full-suite count).
- Two findings worth keeping: (1) dlsym serves the global table
  first-wins, so GLES rows are untestable unless libGLESv2 is dlopened
  first — test_sdl_gles_ebo does exactly that. A global-first dlsym for
  thunk libs is arguably wrong (handle scope ignored); left as known
  behavior, not changed. (2) glcoverage.py regenerates marked spec
  blocks purely from gl.xml and flattens any hand policy inside them —
  all hand-tuned rows (EL_PTR/ARRAY/INDIRECT_PTR, GL_DEBUG_CB) now live
  outside the marks next to the reference rows; regen is a no-op. The
  indirect offset-20 lesson: commands are 20 bytes; the native probe
  agreed with the thunk before the scenario was fixed.
- Leftovers / known limits: no-EBO client-array multidraw untestable on
  core profiles (INVALID_OPERATION either way); cross-instance
  host_err_sink routing stays single-emulator; thunk dtor assumes
  quiesced dispatch.

## Session History (2026-09-03) — audio-thunk review + 3 fixes

- Reviewed `src/frost_graphics/audio_thunk.cpp` (1615 lines) at the user's
  request; both 0x1000 call sites (`src/syscalls/misc.cpp`, `src/jit/
  jit_interp.cpp:jit_thunk_svc`) already follow the documented audio
  return-value polarity (write x0 whenever dispatch != -ENOENT) — no bug.
- Fixed THREE real issues in `audio_thunk.cpp`: (1) pump thread pushed
  STALE bounce bytes — it read the bounce before firing the guest
  callback, so every period played pre-callback data (one period late);
  now snapshots only args under lock and re-reads the bounce after the
  callback returns (matches the inline path); (2) legacy 1-arg
  `SDL_PauseAudio` read pause_on from R(1) (garbage) instead of R(0);
  (3) handle/engine-stream leaks — `snd_pcm_close`, `pa_simple_free`,
  and `alDeleteSources` never erased their map entries nor closed their
  engine streams, and `AAudioStreamBuilder_delete` had no arm (builder
  entry leaked); all four now release properly.
- Left alone on purpose (documented, NOT fixed): dispatch maps lack
  locking vs the pump thread / across guest threads (std::map
  read+write/erase race); S32-labeled-as-S16 with no narrowing + S24
  size inconsistency between arms; write arms report full success when
  the ring accepted 0 frames; pump vCPU shares main-thread TPIDR_EL0
  (TLS aliasing); `#define R(i)` never #undef'd.
- Verified: clean build, `test_linux_audio` 16/16 and
  `test_android_audio` 21/21 pass under BOTH JIT and `--no-jit`, plus
  `audio_test.elf` writes its PCM fine.

## Session History (2026-09-03) — audio-thunk locking overhaul

- Closed the biggest leftover from the audio review: dispatch maps had no
  locking vs the pump thread / across guest threads (concurrent std::map
  read+write/erase = UB). All in `src/frost_graphics/audio_thunk.cpp`.
- `AudioThunkImpl::mu` is now a `recursive_mutex` (was plain `mutex`), so
  arms can hold it across helpers that re-lock it (`resolve` via
  `make_vtable`, same-thread runner reentry) without self-deadlock.
- SDL/AAudio stream maps (`sdl_devs_`, `aa_streams_`) are now guarded by
  `pump_mu` in EVERY dispatch arm (open holds it through engine-open so
  the slot ref stays valid; pause/queue/getqueued/clear/write/getters all
  take it). Fixed lock discipline: callbacks fire BEFORE the map find
  (QueueAudio/GetQueued) or the iterator is re-found after guest code
  runs (Pause-resume) — a callback closing its own device can no longer
  dangle a caller's iterator. Handle counters now increment under lock.
- `run_due_callbacks` rewritten: per-period args are snapshotted under
  `pump_mu`, `next_cb_us` advances BEFORE firing (a concurrent thread can
  never double-fire the same period), guest code runs with the lock
  RELEASED, and the push re-checks liveness then uses the snapshot.
  Same snapshot-outside-runner shape applied to `__osl_bq_enqueue`.
- ALSA/Pulse/OpenAL/AAudio-builder/OpenSL arms (+ handle counters and the
  shared static string buffers) guarded by `mu`. Lock order is always
  mu → pump_mu → mem/engine locks, never reverse.
- Verified: clean build (0 warnings), `test_linux_audio` 16/16 and
  `test_android_audio` 21/21 pass under BOTH JIT and `--no-jit`, plus
  `audio_test.elf` fine.
- Still open (documented, NOT fixed): S32-labeled-as-S16 with no
  narrowing + S24 size inconsistency; write arms report full success when
  the ring accepted 0 frames; pump vCPU shares main-thread TPIDR_EL0
  (TLS aliasing); `#define R(i)` never #undef'd.

## Session History (2026-09-03) — audio S32 playback + S24 size fix

- Fixed the S32-at-2x-speed bug properly: new `PCM_FMT_S32` engine format
  (`src/audio/audio.h` tag + size, `load_sample` conversion in
  `src/audio/audio.cpp`) instead of mislabeling S32 bytes as S16. Safe:
  the physical device already opens in F32 mode for 4-byte formats, so
  S32 flows input→float→device with no device-side change.
- Thunk maps SDL `AUDIO_S32LSB` (0x8020), ALSA `S32_LE/BE` (10/11), and
  Pulse `S32LE/BE` (7/8) to it — numbers verified against
  `/usr/include/alsa/pcm.h` and `/usr/include/pulse/sample.h`, not
  memory. Also added the missing BE/adjacent rows the headers confirm:
  ALSA S24_BE (7), Pulse FLOAT32BE (6), Pulse S24_32LE/BE (11/12, real
  32-bit containers; packed S24LE/BE stays unsupported and documented).
- Killed the three hand-rolled "bytes per sample" copies in
  `audio_thunk.cpp` (top_up_queue/QueueAudio used 2 for S24, writei used
  4, engine converts 4): everything funnels through `pcm_fmt_size` now
  via the local `fmt_size` wrapper (unknown → 2 fallback preserved), so
  accounting can never disagree with conversion again.
- Verified: clean build (the one `-Warray-bounds` warning reproduces on
  the clean tree — pre-existing, not ours), `test_linux_audio` 16/16 and
  `test_android_audio` 21/21 pass under BOTH JIT and `--no-jit`, plus
  `audio_test.elf` fine.
- Still open: write arms report full success when the ring accepted 0
  frames; pump vCPU shares main-thread TPIDR_EL0 (TLS aliasing);
  `#define R(i)` never #undef'd.

## Session History (2026-09-03) — rudolf-cart "music at 2x pitch" diagnosis

- User report: in-game music sounds an octave too high, sfx fine. NO code
  change resulted — the chain is exonerated end to end (details below).
- Verified the asset: `music.wav` is honest 44100 Hz stereo S16 (~199 s);
  guest `SDL_AudioSpec` layout matches SDL2 exactly, so our spec read is
  right; the game's shim forwards music bytes untouched.
- Measured OUR chain with guest-side probes (scratch, not committed):
  440 Hz sine in → 440 Hz out, both at 1.4 MB and at the game's 35 MB
  scale with pause/unpause + half-backlog requeue (the exact
  `loopMusic` pattern). Host grants 44100/stereo/S16; mixer healthy.
- Measured the USER'S live 63 s race via a temporary mixer-output
  capture (probe removed afterwards): long-term average spectrum of the
  speaker-bound audio tracks `music.wav` band-for-band (50 Hz–12.8 kHz,
  within a few dB; +6 dB at 800–1600 Hz is the coin sfx) — NO octave
  shift. Music plays at 1x through our chain during real gameplay.
- Lesson (hard-won): plain cross-correlation is USELESS as a music
  detector here — the loop is so repetitive it can't even match the song
  against itself (conf 0.045 on an exact self-hit). Every "no music in
  capture" reading from correlation was void; spectrum comparison is the
  loop-proof tool. The sine probes (exact-frequency FFT) were the
  reliable instruments throughout.
- Leading hypothesis for what the user heard: overlapping copies — their
  terminal history shows two `nohup ... &` background game launches never
  killed, so up to three simultaneous offset copies of the song (comb/
  chorus) while playing. No stale processes remain now; awaiting a fresh
  single-instance playtest to confirm.

## Session History (2026-09-03) — audio partial-write over-report (real 2x fix)

- User confirmed single-instance and music STILL fast — overlap theory
  dead. Fresh clue from them: sfx fine, asset verified normal in mpv,
  "music is the first sound". Dev-tagged trace of their race showed
  dev1's backlog draining 35.04 MB → 31.39 MB in ~9 s wall (~406 KB/s
  vs the 176400 B/s realtime rate, ~2.3x).
- Root cause in `Audio::push_frames_locked_` (`src/audio/audio.cpp`):
  on the partial-acceptance path it wrote `accept_in` frames to the ring
  but returned/accounted `frames_in` (requested) — return value,
  `bytes_pushed_`, and the WAV-dump buffer all used the wrong count.
  The comment even promised "Returns INPUT frames accepted". Callers
  (`top_up_queue`'s `pending_off`, legacy `/dev/dsp` byte count) advanced
  past samples that were never played → skips → fast playback. Triggers
  whenever pushes exceed per-poll drain (high-fps games topping up a
  nearly-full ring every frame); paced small pushes (sfx fill thread)
  and slow-polling probes always fit whole, which is why sfx and all
  repro probes sounded exact and masked it.
- Fix (3 lines): return/account `accept_in` instead of `frames_in`.
- Verified by dose-response on a scratch tight-poll probe (not
  committed): same 8 s tone drains in 1.9 s wall on old code (4.25x)
  vs 7.6 s wall fixed (~1.05x). Suite: linux 16/16 + android 21/21
  (JIT and --no-jit) and `audio_test.elf` all green.

## Session History (2026-09-03) — honest audio write returns + R hygiene

- Follow-up to the partial-write fix: the three direct-write arms still
  claimed full success on partial acceptance. `snd_pcm_writei` and
  `AAudioStream_write` now return the frames actually accepted (short
  write → guest retries the remainder instead of skipping it);
  `pa_simple_write` returns `-EAGAIN` when zero bytes fit (was silent
  drop + fake 0). OpenAL/OpenSL/SDL paths needed nothing (keep-what-
  fits / full backlog buffering already honest).
- `#define R(i)` (dispatch-local register shorthand in `audio_thunk.cpp`)
  now `#undef`d at the end of `dispatch()` instead of leaking to the
  rest of the file.
- Verified: clean build, linux 16/16 + android 21/21 green.
- Still open: pump vCPU shares main-thread TPIDR_EL0 (TLS aliasing) —
  big surgery, parked until a game breaks over it.
