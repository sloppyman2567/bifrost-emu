# Plan: Scalar-FP register cache + leaf inlining → ~1 GIPS worldgen

Goal: cut the minecraft_weekend worldgen lag spike by chasing ~1 GIPS on the
terrain-generation hot path (currently ~430 MIPS). The remaining spike is
genuine Perlin-noise work (2 fresh chunk loads/frame ≈ 87ms of the ~92ms
spike frames), and the JIT itself is the limiter for the FP-heavy phases.

Status: Phase 1 (baseline) DONE, diagnosis confirmed. Committed baseline is
`87a33ed`. Next: Phase 2 (FMOV compaction).

## Phase 1 results (measurement — 2026-08-14)

JIT MIPS ceiling on cross-compiled benches (interp-counted instr ÷ JIT
run-loop time):

| bench | guest MIPS | result |
|---|---|---|
| bench_mips (pure int addi/ori/andi) | **1084** | `acc=0xf800800a2c4ff835` |
| bench_matrix (float matmul) | **765** | 254.6 MFLOPS, 256×256 |
| bench_fib | 726 | fib(35)=9227465 |
| bench_sort | 499 | qsort 100K |
| bench_memcpy | 420 | 256 MiB @ 3264 MiB/s |

Int/float ceiling ratio = **1.42x**. The game's 430 MIPS sits well below
BOTH ceilings → the FP path is the limiter, and the noise should be able to
approach ~765 MIPS (1.8x) with FP caching; integer ILP proves ~1.1 GIPS is
reachable in principle.

Hypothesis confirmed via `BIFROST_JIT_DUMP=1` IR dump of noise3/grad3
(game is PIE, load base 0x400000 — plan's original 0x32b50/0x324d0 were
stale; real syms: `grad3`=0x431f60, `noise3`=0x4325e0):
- noise3 = 12 blocks, 507 IR ops: **67 FMOV-family** (each `fmov sN,sM`
  expands to 4 IR ops: `FMOV_F2G → IMM 0xffffffff → AND → FMOV_G2F`, the
  single-precision masking variant), **54 FP arith** (FP_BINOP=23,
  FMADD=13, FP_CMP=5, FP_I2F=8, FP_F2I=5), 16 LOAD_MEM (perm[] ldrb),
  and LOAD_REG=77/STORE_REG=64 GPR-cache traffic.
- grad3 = 3 blocks, 63 IR ops: FMOV=18, FP=6.
- Every FP operand crosses `cpu.v_lo[]` memory (`[rbx+off]`) — no XMM
  cache for scalar FP, while GPRs use the LOAD_REG/STORE_REG cache.

## Diagnosis (measured + code-verified)

Fresh-column heightmap = 40K `noise3` × (156 instr + 8× the 16-instr
`grad3` leaf) ≈ 11.4M guest instructions in ~26.5ms ≈ **430 MIPS**.
`grad3` = 16 instr at guest `0x431f60`; `noise3` = 156 instr at `0x4325e0`
(PIE base 0x400000; static offsets 0x31f60 / 0x325e0).

Three dispatch/lookup experiments measured FLAT (BL cap 2→8, read-only fast
caches in jit_call_helper, cache-populating lookup) → the phase is not
dispatch/lookup-bound. Root cause is in scalar-FP codegen:

- **FP_BINOP/FP_UNOP/FP_CMP** (`jit_codegen_fparith.cpp`) round-trip every
  operand through memory: `movsd xmm0,[rbx+off]`; `movsd xmm1,[rbx+off]`;
  op; `movsd [rbx+off],xmm0`; v_hi zero-store; RAX flush → ~7 host instr
  and 2-3 memory accesses **per op**.
- **FMOV Dd,Dn** (`ir_translate_fp.cpp:100`) is lowered to FOUR IR ops
  (F2G → G2F → FHI2G → G2FHI), each a load+store through a GPR scratch,
  +2 scratch vregs allocated — because the translator "avoided adding a new
  IR op". noise3 has 38 fmovs → ~152 memory round-trips per body.
- GPRs have a vreg cache (`x86_regalloc.cpp`) and SIMD blocks have the vec
  cache (`jit_codegen_vec_cache.cpp`, XMM3-15 pinning). **Scalar FP has no
  cache at all.**

→ ~2 memory ops per guest instruction explains the measured ~10 cyc/instr.

Why 1 GIPS is reachable here: Perlin noise has real ILP (the 8 grad3
contributions are independent until the final lerp), so with register
caching the noise can run at ~1-2 cyc/instr. A native serial float chain
would not be faster — the ILP is the unlock.

## Phases

### Phase 1 — Baseline ✅ DONE
- Built bench ELFs (musl-static `-O2`), measured the int (1084 MIPS) and
  float (765 MIPS) JIT ceilings.
- `BIFROST_JIT_DUMP=1` IR dump confirmed the FMOV 4-op expansion and the
  FP memory round-trips (see Phase 1 results above).

### Phase 2 — FMOV compaction ✅ DONE (not shipped — ~1.5% only)
- New `IROp::FP_MOV` (FP↔FP move): 1 IR op, codegen = 1 `vmovsd`/`vmovss`
  load + store (+ v_hi zero for single, + v_hi copy for double), replacing
  the old 4-op `FMOV_F2G→(IMM/AND)→FMOV_G2F(→FHI2G/G2FHI)` GPR round-trip.
  Files: `include/ir/ir.hpp` (op), `src/ir/ir_translate_fp.cpp` (both FMOV
  FP↔FP branches emit it; `ftype` = width), `src/jit/jit_codegen_fparith.cpp`
  (codegen; clobber_flags + flush RAX only on single), `src/ir/ir_optimize.cpp`
  (is_pure, DCE-liveness FP branch, dump name).
- Verified: full 199/199, quick chain-skip 194/194, quick FWD 194/194,
  triangle rc=0, bench_mips byte-identical, bench_matrix correct.
- Measured: heightmap 26.5 → **26.1ms** (~1.5%). Confirms the plan caveat:
  FMOV is NOT the bottleneck — the FP_BINOP/FMADD memory round-trips and
  the serial powf chains are. FP_MOV stays anyway: it is the correct IR
  shape for Phase 3's FP register cache (pinned XMM can turn it into a
  reg-reg movsd). Do not ship Phase 2 standalone.

### Phase 2 — FMOV compaction (small, low-risk)
- Add a real `IROp::FP_MOV` so `fmov Dd,Dn` / `Sd,Sn` = 1 `vmovsd`/`vmovss`
  (+ v_hi zero for single) instead of 4 IR ops + 4 memory round-trips.
- Mirror in `src/ir/ir_optimize.cpp` (purity/consts/`last_store_to`) and
  `instr_will_call_interp`'s FP gate in `jit_translate.cpp`.
- Est. ~10-20% on the heightmap alone.

### Phase 3 — Scalar FP vreg cache (the main event)
Reuse the vec-cache machinery (`jit_codegen_vec_cache.cpp`) for scalar FP:
- New `fp_cache_may_enable` gate for scalar-FP-heavy blocks (noise3
  qualifies: ~65% FP ops), exclusive with the SIMD vec cache (same XMM pool).
- Pin hot scalar FP vregs (v_lo of v0-31, 32/64-bit aware) into XMM3-15;
  track dirty + v_hi consistency (FMOV_FHI2G/G2FHI must flush through).
- Prologue loads on cold entry; epilogue writeback before block return;
  self-loop blocks keep pinned regs as loop-carried state; `emit_call_interp`
  guard writes back + reloads.
- Rewrite FP_BINOP/FP_UNOP/FP_CMP/FP_MOV/FMOV_* codegen to read/write the
  cached XMM regs (1-2 host instr per op).
- Careful with BL_CALL/BLR_CALL: flush pinned FP vregs (cheap, only dirty
  ones) before the helper call, invalidate after.

### Phase 4 — Leaf-call inlining (pushes past 2x)
- Runtime-detect BL_CALL/BLR_CALL targets that are single-block leaves
  (translate the target; verify 1 block ending in RET, no internal
  branches). `grad3` qualifies (16 instr).
- Splice the leaf's IR into the caller on re-translation, eliminating the
  call round-trip (flush/invalidate/prologue/epilogue/helper).
- The 8×grad3 round-trips become the dominant cost after Phase 3, so this
  is the step that gets the heightmap from ~1.5x to ~2.5-3x.

### Phase 5 — Verify (full matrix)
- `make check-all` / quick suite 194/194; full 199/199.
- `--chain-skip` quick 194/194; FWD quick 194/194.
- `bench_mips` byte-identical (`done: acc=0xf800800a2c4ff835`) under
  `BIFROST_JIT_VERIFY` / `BIFROST_JIT_VERIFY_MEM`.
- `test_sdl_gl_triangle` ALL PASS.
- Game: fresh-column heightmap 26.5 → ~9-11ms; fresh chunk 43.6 → ~20ms;
  spike frames roughly halve.

## Expected outcome
- Heightmap ~2.5-3x (26.5ms → ~9-11ms) → ~1.1-1.2 GIPS on that phase.
- Same win flows into mesh (cglm float math) and lighting.
- Fresh chunk 43.6ms → ~20ms; exploration spikes halve.

## Honest caveats
- Phase 3 touches the hottest codegen paths; regalloc bugs hide here — the
  verification matrix (JIT_VERIFY, FWD, regalloc-check, bench_mips
  byte-identical) is the safety net.
- Serial float chains (e.g. musl `powf` in the expscale wrappers) stay
  latency-bound — that slice of worldgen won't hit 1 GIPS.
- Phase 2 alone (~15%) is not worth shipping without Phase 3.
- Game instrumentation must be reverted before any commit; never commit
  `ctest_real/minecraft_weekend/t`, `subaru_stairs*.mp4`.

## Key files
- `src/ir/ir_translate_fp.cpp` — FMOV lowering (`:100`), FP op IR emission
- `src/jit/jit_codegen_fparith.cpp` — FP_BINOP/FP_UNOP/FP_CMP codegen
- `src/jit/jit_codegen_vec_cache.cpp` — the XMM pinning machinery to reuse
- `src/jit/jit_codegen_fp.cpp` — FMOV_G2F/F2G/FHI2G/G2FHI codegen
- `src/ir/ir_optimize.cpp`, `src/jit/jit_translate.cpp` — mirrors
- `ctest_real/minecraft_weekend/lib_noise/noise1234.c` — noise3/grad3
- `ctest_real/minecraft_weekend/src/world/gen/worldgen.c` — heightmap/fill