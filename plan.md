# Plan: minecraft_weekend — cut the SIGPROF "other" bucket (mmap/munmap + GL thunk)

## Objective
The game (ctest_real/minecraft_weekend, GLFW+OpenGL voxel) is CPU-bound but only
~46% of wall time is guest JIT execution. ~49% is SIGPROF "other" (host-side:
mmap/munmap zeroing, GL/SDL thunks, driver work). Goal: shrink "other" by
attacking the two addressable sources — the per-frame mmap/munmap mesh-buffer
churn and the thunk dispatch overhead. Host GL driver/upload work inside "other"
is irreducible.

## Environment run discipline
- Repo: `/home/gamingpc/Downloads/bifrost-emu-1.5.0-alpha`; build with `make`.
- Profile from the game dir (res/ is relative), NO input and NO clean exit
  needed — the game auto-simulates/generates; periodic stats print mid-run:
  ```
  cd ctest_real/minecraft_weekend
  BIFROST_PROF=1 BIFROST_STATS_PERIOD=10 BIFROST_CLASS_PROF=1 \
      timeout 42 <repo>/bifrost-emu ./minecraft_weekend.elf 2>&1 | \
      rg 'SIGPROF|guest:|block-end|syscalls|class'
  ```
- One ~40s timeout-killed run is a valid profile (SIGPROF buckets print every
  10s via dump_periodic_stats since b3a2a60).
- Do NOT run long suites repeatedly for A/B (user constraint); use single
  targeted runs. Game frame-count A/B is unreliable (±30% same-binary spread).

## Baseline profile (2026-08-14, default JIT)
- SIGPROF: jit ~46%, dispatch ~5%, translate ~0.4%, interp 0.0% (0 fallbacks,
  0 interp_only), **other ~49%**.
- ~100 MIPS real, avg 6.2 instr/block, 16.4 M blocks/s.
- Syscalls ~900/s (growing): mmap 222 ~31% (~280/s), munmap 215 ~30% (~265/s),
  madvise 233 ~2.5%, clock_gettime 113 ~2%, brk 214. mmap+munmap = ~61%.
  (Thunk calls = syscall 0x1000 are NOT yet counted — SYSCALL_HIST_MAX=512
  underflows them; Phase 0 fixes this.)
- Game: FPS ~150, TPS ~60. Mesh remesh allocates 4 buffers (~25 MB: 18 MB DATA
  + 2.25 MB INDICES + FACES + t_indices) via malloc→mmap, uploads, frees→munmap.
  Buffers live in the 4 GiB direct window (heap 256..768 MiB) — JIT fast path.

## Design principle: replicate Linux kernel mmap semantics
Real Linux does NOT zero fresh anonymous mappings eagerly — it drops physical
pages at munmap and zero-fills on demand via page faults. The emulator should
mirror that. The direct window is ONE contiguous `MAP_PRIVATE|MAP_ANONYMOUS|
MAP_NORESERVE` host mapping (memory.cpp:18-20) that the JIT addresses as
`direct_window_ + guest_addr`, so sub-ranges can't be literally unmapped (real
munmap → SIGSEGV on stale access is impossible without tearing the window).
The closest kernel mechanism that fits is `madvise(MADV_DONTNEED)`: physical
pages freed at munmap (kernel behavior), later access faults zero-filled
instead of SIGSEGV. That divergence (zeros vs SIGSEGV on use-after-free) is
deliberate, documented, and strictly safer. JIT direct-window loads/stores
(`mov (%r10,%rax),%rdx`) fault transparently — no host signal, the kernel
zero-fills and continues. DECISION LOCKED: use madvise, NOT bulk memset.

## Work items

### Phase 0 — Count thunk calls (measurement, no behavior change)
- src/syscalls/syscalls.cpp: SYSCALL_HIST_MAX=512 < 0x1000; raise the cap (or
  special-case 0x1000) so thunk calls appear in the histogram. Re-profile ~40s.
  Output gates Phase 3 (skip it if thunk volume is low).

### Phase 1 — mmap/munmap fast path (biggest measured win, kernel-faithful)
1. `untrack_allocation` (memory.cpp:446-486): for all-window ranges
   (`direct_window_ && addr+size <= DIRECT_WINDOW_SIZE`) replace the no-op
   per-page loop (4608 iterations of nothing) with one
   `madvise(direct_window_+addr, size, MADV_DONTNEED)`. Keep the non-window
   `pages_` loop.
2. `mmap_alloc` (memory.cpp:327-345): remove the window memset entirely (both
   reused + fresh). New invariant: window pages are never-touched OR
   madvise'd ⇒ zero-on-fault. Keep non-window zeroing.
3. OOM count loop (memory.cpp:313-318) + the untrack page loop: short-circuit
   when the whole range is in-window (provably 0 new pages).
4. Guards: `direct_window_` null-check; verify MAP_FIXED path (hint!=0,
   memory.cpp:302-307) doesn't rely on old zeroing (madvise only strengthens
   it); confirm no host code caches window pointers across munmap (JIT computes
   from direct_window_ at runtime — safe).
- Correctness: madvise makes guest use-after-free read zeros (vs stale data) —
  strictly safer; test_mmap zero-fill expectations hold (fault → zero).

### Phase 2 — Syscall dispatch + thunk hygiene (cheap)
1. `Emulator::syscall` (syscalls.cpp:239-244): pre-check hot numbers →
   direct subsystem call, skipping the 5-handler + 4-subhandler chain:
   222/215→mem, 0x1000→misc (thunk), 98/220→threads. Everything else falls
   through unchanged (zero mis-dispatch risk).
2. `thunk.cpp:1155`: `getenv("BIFROST_FRAME_TRACE")` runs on EVERY dispatch —
   cache to a static bool (per debug_flags.h rule). Grep for other per-call
   getenv in thunk/syscall hot paths.
3. `track_state_change` (thunk.cpp:1150 → gl_state.cpp:550-733): 32 string
   compares per call, all miss for hot calls. Precompute an `is_state` flag on
   each SymbolEntry at registration; per-call check = one bool test.

### Phase 3 — (conditional on Phase 0 thunk volume)
JIT direct thunk fast path: mirror `jit_vdso_clock_svc` — emit a direct call to
the thunk dispatcher for `0x1000` SVC, skipping Emulator::syscall (histogram/
drain_host_signals/running check). Medium effort; only if measurement justifies.

## Verification (Phase 4)
- Mem tests: test_mmap, test_mremap, test_brk, test_dlopen, test_dyn_malloc,
  malloc-heavy tests.
- Full suites default + chain-skip (199 each), regalloc-check quick,
  bench_mips byte-identical under JIT_VERIFY/JIT_VERIFY_MEM/FWD.
- Game profile re-run: compare "other" % and MIPS vs baseline (single run per
  phase, not repeated A/B).

## STATUS UPDATES
- 2026-08-14: Plan written. Phase 0 next (raise SYSCALL_HIST_MAX to see thunk
  calls), then Phase 1 madvise.
- 2026-08-14 (Phase 0/1/2 DONE): Raising SYSCALL_HIST_MAX to cover 0x1000 was
  decisive: **thunk syscalls (4096) are 99.8% of ALL syscalls — ~115K/s
  ramping to ~173K/s** after warmup. mmap/munmap churn is ~0.1% (~1500/s
  combined) — NOT the syscall bottleneck (the earlier ~900/s baseline was a
  cap-512 measurement artifact that hid 0x1000). The SIGPROF "other" ~46% is
  dominated by the thunk dispatch path (JIT SVC round-trip + dispatch marshalling
  + host GL driver), so Phase 2's thunk hygiene is on the right target and
  **Phase 3 (JIT direct thunk fast path) is now strongly justified**.
- Implemented + verified:
  - Phase 1: `madvise(MADV_DONTNEED)` at munmap for window ranges
    (untrack_allocation), window memset removed from mmap_alloc (kernel-faithful
    lazy zeroing), OOM + untrack page loops short-circuited for all-window
    ranges.
  - Phase 2a: hot-number pre-dispatch in Emulator::syscall (222/215/216/214/226/
    233→mem, 98/220/435→threads, 0x1000→misc) skipping the 6-handler chain.
  - Phase 2b: BIFROST_FRAME_TRACE moved to cached dbg().frame_trace (debug_flags.h);
    GLStateTracker::tracks_state() gate so non-GL thunk calls skip the 32-name
    scan in track_state_change.
  - Histogram cap: SYSCALL_HIST_MAX 512 → 4097 (counts 0x1000).
  - Verification: build clean; test_malloc pass; **full suite 199/199 default
    AND --chain-skip**; regalloc-check quick 194/194; bench_mips byte-identical
    acc=0xf800800a2c4ff835 under JIT_VERIFY / JIT_VERIFY_MEM / FWD.
  - Game still runs (30s+ profile, ~101 MIPS, interp 0%, 0 fallbacks).
- NEXT: Phase 3 — JIT direct thunk fast path (mirror jit_vdso_clock_svc): emit a
  direct call to the thunk dispatcher for SVC 0x1000, skipping
  Emulator::syscall (histogram/drain_host_signals/running check). Medium
  effort; only if measurement justifies.
- 2026-08-14 (Phase 3 DONE): `jit_thunk_svc` added — `jit_native_svc` branches
  on `cpu.regs[8] == GraphicThunk::SYSCALL_NUMBER` (0x1000) and dispatches
  directly to the GraphicThunk/AudioThunk/DisplayThunk chain, skipping
  Emulator::syscall (drain_host_signals, running check, trace gates,
  pre-dispatch). `note_syscall()` exported from syscalls.cpp so the histogram
  still counts thunk volume. Zero codegen changes (reuses emit_call_native_svc
  + flush_all_vregs). Verified: test_sdl_gl_triangle ALL PASS; game runs
  (104.8 MIPS vs ~87-101 pre-phase-3; dispatch% 3.5% vs 5-11%); full suite
  199/199 default + --chain-skip; regalloc-check quick 194/194; bench_mips
  byte-identical under JIT_VERIFY/JIT_VERIFY_MEM/FWD.
- REMAINING "other" (~50%) is the host GL driver work + thunk dispatch
  marshalling inside jit_thunk_svc (real GPU uploads, bounce sizing) — mostly
  irreducible. Possible future trim: GLFW_POLL's 316 glfwGetKey calls/frame.
