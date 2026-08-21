# Codegen optimization plan — post-M2 tier-2

Status: **ALL TRACKS RESOLVED (2026-08-20 night).** Track 0 measured
(minecraft parity), Track 1 shipped (`054a4d5`), Track 2 superseded
(regions cover it), Track 3 shipped (`10b0b4d`), Track 4 shipped
(`b84cc22`), Track 5 closed after re-measurement (LICM/pins healthy,
`0c30ce6`). Bonus: loop-regions-only default (`0c518c5`) turned CoreMark
from −7% to **+3.5%** under tier2. Future ideas live in AGENTS.md session
notes; this file is historical.

## Measured baseline (so far)

- M1 (2-block natural loop, 20M iters): 272-273 ms → 249-251 ms (~8%).
- m3loop back-edge region (`b .Lloop` bottom): 3.71 s → 2.25 s (~39%).
- m5big self-loop fusion: 1.553 s → 0.961 s (~38%).
- bench_mips: ~360 ms, self-loop, acc `0xf800800a2c4ff835` unchanged under tier2
  (zero tier2 activity — body-bound, not dispatch-bound).
- CoreMark: 1687 → 2210 iters/sec plain (cross-block flag-skip), 2541 with
  FWD+CHAIN_SKIP. All CRCs valid. Hot loop is a 2-block cross-block loop.
- Minecraft weekend: FPS 18 | TPS 59 during worldgen, 420+ idle (host frame
  pacing). Chunk loads <20 ms. Worldgen noise path is blr-heavy (~40K
  indirect calls/column; noise3 = 156 instr + 8×16-instr grad3 leaf).
- ~~Not yet measured: minecraft under `BIFROST_TIER2=1`.~~ **MEASURED
  (2026-08-20) — see Track 0 below: startup-burst parity, tier2 stays opt-in.**

## Track 0 — M3 measurement (DONE 2026-08-20 — read the methodology before re-measuring)

**Result: tier2 ON vs OFF is PARITY on the input-free worldgen burst**
(time GAME-ENTER-LOOP → chunk #80 done, host clock: OFF avg 2.81 s / ON avg
2.80 s over 2 reps each; chunks@15s 91/92 vs 89/87). No regression, no win.
tier2 stays opt-in (default OFF). The in-code counter tax question stays open
but looks small (the burst fires its hottest blocks within ~3 s and shows no
drag; the earlier "-13%/-41%" numbers were contaminated, see below).

**Three measurement confounds discovered — do NOT repeat them:**
1. The guest's own FPS print is unreliable (guest clock stall, known).
2. The game's whole loop (ticks + chunkgen) is chained to HOST VSYNC. Run
   with `__GL_SYNC_TO_VBLANK=0` or you measure the monitor, not the emulator.
3. **Chunk-generation counts measure PLAYER INPUT.** `[DBG-GEN] done` lines
   follow the player: standing still → ~91-chunk ring then nothing; walking →
   hundreds. An interactive user on DISPLAY=:0 silently poisons every run
   (this produced a fake 485-chunk "outlier" and a fake -13% regression).

**Clean protocol (works):** pipe stdout through a host-timestamping reader,
take `GAME-ENTER-LOOP` → Nth `[DBG-GEN] done` as the burst time (N=80 is
inside the no-input ring), keep hands off keyboard/mouse for the first ~20 s.
After that the user may play freely — later output is not used.

Also noted: `BIFROST_STATS_PERIOD` prints only when the main run loop spins;
this game blocks it in GL/thunk calls most of each frame, so expect ONE dump
per run and a meaningless `0.0 MIPS` (huge dt). Don't trust it here.

## Track 1 — Call-aware regions (SHIPPED `054a4d5`)

Landed: BL fuses when the callee is pre-translated (`lookup_only` gate,
aborts as `bl_untranslated` otherwise); BLR fuses unconditionally;
`BIFROST_NO_CALLREGION=1` restores M1 aborts; region-wide `has_call` gates
pinning + LICM arch-load hoisting. Verified: m6bl/m6blr harnesses tri-mode
byte-identical with regions formed around calls, JIT_VERIFY +
REGALLOC_CHECK clean, suites green. Micro-harness timing neutral (leaf
prologue dominates a 6-inst loop). Minecraft burst parity — see Track 0.
Full contract in AGENTS.md Session History 2026-08-20.

## Track 2 — Cross-block pinning

**Problem:** pins are per-block; CoreMark's hot matrix loop is 2 blocks, so
pinning cannot help it.

**Plan:** carry pins across a 2-block chained loop via the chain edge.
- The chain edge today re-runs the successor's prologue preloads; a pinned
  (deferred) value should instead stay in its pin register across the chain.
- Requires the chain edge to know the successor's pin set (communicate via the
  chain contract — the predecessor's epilogue currently flushes all vregs).
- Deferral safety invariants from the self-loop pinning apply (exit flush once,
  call-like ops exclude, `BIFROST_NO_PIN` gate).
- Acceptance: CoreMark matrix loop iteration cost drops; CRCs stay valid;
  bench_mips acc unchanged.

## Track 3 — Region code-size / cold-exit bloat

**Problem:** regions are ~260 B per 2-instr block (vs 60-80 B standalone) — the
per-edge cold exits (`materialize + flush_all_vregs + flush_pins + pc-store +
ret`, jit_tier2.cpp:1189-1212) and per-edge snapshot restores are the bulk.

**Plan:** route cold exits to a shared exit stub.
- A few registerized slots describe the live set; each cold exit jumps to one
  shared flush+ret stub instead of a full per-edge copy.
- Cold exits are rare (taken edges that leave the trace), so a small runtime
  cost is fine; the win is region bytes + I-cache + raising the 64-block cap's
  practical ceiling.
- Consider a bytes-budget cap (not just block/inst caps) once traces shrink.
- Acceptance: region bytes drop; m3loop/m5big timings hold; acc byte-identical.

## Track 4 — Flip wins currently OFF by default

**FWD** (`BIFROST_ENABLE_FWD=1`, ~4-6%):
- All three corruption bugs are diagnosed + fixed (regalloc clobber, SBFM
  const-fold, SBFM JIT sign-extension) and re-verified 205/205 + JIT_VERIFY.
- Re-soak (quick suite + verify + FWD+verify + REGALLOC_CHECK + game_demo),
  then consider default-on. Regions already force FWD on their IR.

**Chain-skip** (`BIFROST_CHAIN_SKIP=1`, ~17-19% on multi-block workloads):
- Regions currently decline under chain-skip (`!chain_skip_enabled_` gate).
- Making regions compose with chain-skip (shared 32 KB frame + chain_entry
  contract) is the biggest per-block win. Measure the game A/B first
  (frame time at a fixed render tick, not FPS — FPS spikes are host pacing).

**Dropped `ir_optimize` folds** (commutative src1→src2 swap + ZEXT-after-LOAD_MEM,
+0.4%):
- They hang CoreMark under FWD. Re-diagnose now that the regalloc evict bug is
  known — it may have been the culprit, not the folds.

## Track 5 — LICM / pin refinement

**Problem:** M2 measured LICM neutral-to-slightly-negative when the invariants
are already pinned (m2loop3 ~1% regression — hoisted bare LOAD_REGs read from a
slot instead of the cheap pin).

**Plan:**
- Hoist the invariant *chain* onto a free register directly (skip the preheader
  slot-load per iteration) when pins don't already cover it.
- Bump pins 4 → 6 (R12-R15 + two more) if the allocator has headroom.
- Smarter pin selection for written-then-read vregs (currently excluded because
  a pin starves LOAD_MEM's spare reg — revisit with a 5th/6th pin).
- Acceptance: m2loop4 keeps its ~5% LICM win; m2loop3 regression disappears or
  flips positive; acc byte-identical.

## Do-not-regress list (from AGENTS.md contracts)

- No per-dispatch atomic / per-PC watchdog / fast-path `cpu.pc` store; `~0ULL`
  cache sentinels; trimmed fast paths.
- In-code tier-2 counter is the only place that sees CHAINED execution.
- Cold exits flush ALL pins (loop-carried deferral makes snapshot "clean" a
  lie); the Lback does NOT flush.
- Chain-in repatch selects the slot by recorded target PC, not
  `has_taken_chain_slot`; skip `is_region` heads.
- Direct-write pin detection uses the EXPLICIT op list (branch dummy dest=0
  must never flag x0); LOAD_MEM/ATOMIC included.
- Regions decline under chain-skip and W^X until tracks above land.
- Every change: tri-mode byte-identical (interp on `_quick` variants), suite
  205/205 tier2 off + quick 200/200 on, JIT_VERIFY no NEW divergences,
  REGALLOC_CHECK clean, bench_mips acc `0xf800800a2c4ff835`.