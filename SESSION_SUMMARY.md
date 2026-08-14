# bifrost-emu — Session Handoff (Aug 2026)

## Objective
Get `ctest_real/minecraft_weekend` (AArch64 Minecraft-ish game) running fast via
JIT. Four commits landed this session: `e69eff0` (native FCVT rounding +
de-interp of FP blocks), `d27d396` (native SIMD-scalar FP-source int↔FP converts
+ fbits sentinel fix), the MODIMM commit (native AdvSIMD modified-immediate
MOVI/MVNI/ORR/BIC), and the scalar-shift commit (native `ushr/sshr/shl dN,dM,#imm`).
Full suite **193/193 PASS default** (~50s). Game runs from its own dir
(`ctest_real/minecraft_weekend/`, `../../bifrost-emu`); running from repo root
fails on shader load. Seeds from `NOW()` — runs are noisy, use
`BIFROST_STATS_PERIOD` + long runs. Terrain confirmed CORRECT (varied heights
incl. below-sea-level chunks). Game wall time ~30.5s → **~22.3s clean**.

## What was just committed (this session)
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
- `perf(jit): native AdvSIMD modified-immediate MOVI/MVNI/ORR/BIC` (previous).
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

## Profiling state (the key numbers)
- SIGPROF (`BIFROST_PROF=1`): jit ≈ 75%, interp ≈ 7%, dispatch ≈ 10%,
  translate ≈ 0.6%, other ≈ 1%.
- Game wall time: ~30.5s → **~22.3s clean** after the scalar-shift native
  work. `bench_mips` tight ALU self-loop = **660 MIPS**.
- Post-scalar-shift fallback words (classprof @500K in JIT mode):
  `7f600401/02/03/20` (`ushr dN,dM,#32`) GONE. Remaining `[fp/simd]`:
  `4e010c20` = `dup vN.16b, wN` (~4K), `4e083c01` = `mov xN, vN.d[0]` (~0.3K).

## Next steps (suggested)
1. **`dup vN.16b, wN` (4e010c20, now the top remaining fallback ~4K/sample)**
   and `mov xN, vN.d[0]` (4e083c01, ~0.3K). `dup` is a 0x0F00x00c-group
   SIMD_DP sub3 op (2-elem broadcast, GPR source) — likely easy via a
   GPR load + broadcast to both halves; `mov xN, vN.d[0]` (UMOV) is the
   lane-extract inverse (may already have UMOV native elsewhere).
2. Codegen quality (jit=75%): consider fusing STP (33% of guest instrs).
3. Do not regress fast dispatch paths (bare call/ret, no atomics, no per-PC
   watchdog). Hot-interp promotion stays opt-in (BIFROST_HOT_INTERP).

## Critical traps (read AGENTS.md for full list)
- Scalar 64-bit shifts (SHL 0x5F005400 / USHR 0x7F000400 / SSHR 0x5F000400)
  are FP-space (bits[28:24]=11111) → DECODER routes to FP_SCALAR, the
  simd_dp table never sees them. Reuse SIMD_SHL/USHR/SSHR esize=8 q=0 in
  the FP_SCALAR translator; shift==64 falls back (SSE2 imm-shift masks
  count to 6 bits). Mirror in fp_gate bit 0x100.
- **PSRAQ is NOT SSE2/AVX2** (AVX-512F only): never emit `66 0F 73 /4 ib`
  or VPSRAQ for esize=8 SSHR — CALL_INTERP it (and SSRA/SRSRA already do).
- Modified-immediate guard: NO SHRN exclusion clause (`bits[15:10] != 0x21`)
  — cmode=8 MOVI (16-bit LSL#0) also has bits[15:10]=0x21 and gets zeroed.
  `immh==0` (bits[22:19]) alone separates SHRN (every valid SHRN has immh>=1).
  Keep interp and simd_dp table in sync on this.
- FP_I2F_FIXED/FP_F2I_FIXED: `immr` is RAW fbits, sentinel is 0 (= integer
  form, no scale). Never re-add the `? : 64` default. FCVTAS + unsigned non-Z
  STILL fall back — keep instr_will_call_interp mirror in sync if added.
- fp_gate polarity: "native" = `fp_gate >= 0 && !(fp_gate & bit)`, NOT the
  inverted form.
- cvttsd2si sentinel: always pre-check range, never post-clamp on sentinel.
- Runtime hot-interp promotion: DO NOT re-enable blindly.
- Taken-path chain slot: do NOT remove or restrict to BRCOND.
- SIMD_LDST `src2` must be a REAL vreg (vreg 0 = guest X0), never literal 0.
- Heap/stack MUST stay in 4GiB direct window (MMAP_BASE_MIN=0x10000000,
  MAX=0x30000000, STACK_TOP=0x3F000000).
- Thunk trampolines must load symbol id into x9 via `write_thunk_trampoline()`.
- DisplayThunk prefers SDL2 DisplayProxy for THUNK_PROXY symbols.
- brk must page-align + refuse stack region; tests use `syscall(214, 0)`.
- New tests: no libm, `-static -O2`, register in run_tests.sh INTEGRATION_TESTS.

## Persona
Act as **DeepSeek-chan** (skill at `/home/gamingpc/.opencode/skills/deepseek-chan`):
sassy whale-maid "Freeloader-kun" tone, world-class error-free technical work.
Load `code-reviewer` skill for review tasks. Update AGENTS.md after changes.
