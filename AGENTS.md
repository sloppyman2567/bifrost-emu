# bifrost-emu

## Purpose

AArch64 Linux user-mode emulator for x86_64 hosts: JIT + interpreter,
syscalls/VFS, dynamic linking, and host GL/EGL/SDL2/Vulkan thunks so
guest apps (including SDL2+OpenGL demos) can run without QEMU.

## Ownership

- Core: `src/core/`, `src/interp/`, `src/jit/`, `src/ir/`
- Syscalls/VFS: `src/syscalls/`, `src/yggdrasil/`
- Dynlink/ELF: `src/frontend/`
- Graphics thunks: `src/frost_graphics/`, `include/frost/`
- Tests: `ctest/`, `ctest_real/`, `scripts/run_tests.sh`
- Trace/diagnostic toggles live in `include/debug_flags.h` (single cached
  parse): `BIFROST_TRACE=1` enables the whole trace suite; the fine-grained
  `BIFROST_XTRACE` / `BIFROST_FUTEX_BT` / `BIFROST_PPOLL_PEEK` / `BIFROST_THUNK_TRACE`
  (→ `dbg().thunk_trace`) / etc. still override. Add new diagnostic switches
  there, NOT as ad-hoc `getenv()` checks in hot syscall/interp paths.

## Local Contracts

- Build: `make` (optional `USE_SDL2=1 USE_THUNK_GL=1` for host GL/SDL)
- Cross tests: `make cross SRC=… OUT=…` via musl toolchain in `tools/`
- Thunk trampolines end with `ret` after `svc`; pointer args outside
  the 4 GiB direct window bounce through a host buffer with writeback
- Thunk trampolines MUST load the symbol id into x9 (the dispatcher reads
  `cpu.regs[9]` in `src/syscalls/misc.cpp`). Always emit trampolines via the
  shared `write_thunk_trampoline()` helper in `thunk_common.hpp` (which uses
  `MOVZ_Xd_IMM16(9, …)`); a hand-rolled `0xD2800000u | (id << 5)` silently
  encodes `movz x0` and misroutes every call. This bit DisplayThunk for a
  long time (fixed in the third review pass).
- DisplayThunk prefers the SDL2 DisplayProxy for `THUNK_PROXY` (X11/Wayland)
  symbols even when the host libX11/libwayland are present: proxy handles are
  guest addresses that round-trip, while a HOST `Display*` returned by
  `XOpenDisplay` cannot be translated back through guest memory (bounce →
  SIGSEGV). Host libs are only a fallback when no SDL proxy initializes
  (headless). Do not "restore" host-lib preference for THUNK_PROXY symbols.
- dlopen of libGL/libSDL2 prefers thunk registration when the on-disk
  `.so` is not AArch64 (do not map host x86_64 libs as guest code)
- Absolute-path `dlopen` rejects non-AArch64 ELFs and falls back to
  soname search under BIFROST_ROOT / toolchain paths
- glibc `dlopen` hook offset in `_rtld_global_ro` is detected from
  `dlopen` disassembly (368 vs 376 across glibc versions)
- Static ELFs still get a DynamicLinker so runtime thunk dlopen works
- Unhandled SIMD_DP ops in `interp_fp.cpp` throw `DecodeError` (→ SIGILL),
  not a silent NOP; log via `BIFROST_SIMD_TRACE=1`. Implement the missing
  op rather than re-silencing. SADDW/SADDW2 (0x0E201000) and UMINP
  (0x2E20AC00) sub3_noq groups are covered. The pairwise max/min family
  (SMAXP/SMINP/UMAXP/UMINP) case label covers ALL sizes 0-3
  (0x2E20A400/0x2E60A400/0x2EA0A400/0x2EE0A400 for max, +bit11 for min);
  do not narrow it back to size=0 — GCC's vectorized memchr/strchr emit
  `umaxp v31.4s` (size=2), which a size-0-only case label silently
  DecodeErrors (SIGILL in interp-only, SIGABRT via JIT CALL_INTERP).
  TBL/TBX all four forms
  (TBL1 0x0E000000, TBL2 0x0E002000, TBX1 0x0E001000, TBX2 0x0E003000;
  op2=bit12, L=bit13) are in interp — GCC lowers `vextq_u8` to TBL2 +
  `ins v.b[i], v.b[j]` index building. INS (element, vector) shares the
  EXT prefix `(op & 0xBFE00000) == 0x2E000000`; distinguish by bit10
  (INS=1, EXT=0). RBIT/NOT/CNT all collapse to sub2 0x0E205800 — RBIT is
  size=1, NOT is U=1; RBIT bit-reverses per byte. Vector FCVTZS/FCVTZU
  share sub3 with ABS/NEG (0x0E20B800/0x2E20B800) but set bit16 (0x10000);
  FCVTZU clamps negatives to 0. The vector shift-by-immediate
  family (SHL/USHR/SSHR/USRA/SSRA/SLI/SRI/URSRA/SRSRA) is native in the
  JIT (AVX2 VEX 256-bit, `BIFROST_NO_AVX2` disables; SSE2 128-bit fallback;
  esize=1 and 64-bit SSHR/SSRA/SRSRA via CALL_INTERP — there is NO PSRAQ in
  SSE2/AVX2, `66 0F 73 /4 ib` SIGILLs the host; VPSRAQ is AVX-512F only, so
  a "native esize=8 SSHR" is a trap). The scalar 64-bit Dd,Dn,#imm forms
  (SHL 0x5F005400 / USHR 0x7F000400 / SSHR 0x5F000400, mask 0xFF00FC00) live
  in the FP space (bits[28:24]=11111), so the DECODER routes them to
  FP_SCALAR, not SIMD_DP — the simd_dp table never sees them and they
  used to CALL_INTERP (the voxel game's `ushr dN,dM,#32` was the top
  remaining fallback). They're handled in the FP_SCALAR case of
  ir_translate_fp.cpp by reusing SIMD_USHR/SHL/SSHR with esize=8, q=0
  (v_lo shifted, v_hi zeroed; shift==64 falls back to the interp's
  clear/sign-fill). `instr_will_call_interp` mirrors them under gate bit
  0x100. USRA masks to 0x2F001400 —
  do not confuse it with the rounding variants URSRA (0x2F003400) / SRSRA
  (0x0F003400): those add the round-half-up top discarded bit, computed via
  (Vn >> sh) + ((Vn >> (sh-1)) & 1) (isolation: PSRL (sh-1) then PSLL/PSRL
  (esize*8-1) round-trip, no mask constant). URSRA sh==esize*8 → top-bit
  test; SRSRA sh==esize*8 → 0 (sign-fill cancels the round carry).
- SIMD DUP (general, GPR→vector) is native for ALL element sizes and both
  Q values (table guard in `simd_dp.txt`: imm5 ∈ {1,2,4,8}, no Q predicate —
  the old `imm5==8 && Q` was dead code: `dup Vd.2d` needs sf=1 which the
  decoder rejects at `decoder.cpp:433`). `memset` in the game does
  `dup v0.16b, w1` (0x4E010C20) then `mov x1, v0.d[0]` (UMOV), so the
  broadcast crosses a block boundary and the next block reads `cpu.v_lo`.
  Codegen: mask low esize via `(RAX<<(64-esize*8))>>(64-esize*8)` then
  shift-replicate up the qword (RCX scratch); `vmovq xd,rax` (+`vmovddup`
  only for Q=1 — VMOVQ already zeroes the upper 64 bits for Q=0); memory
  path stores RAX to v_lo and, for Q=0, zeroes v_hi. CRITICAL: the
  shift-replicate chain DESTROYS src1 when `ensure_vreg` returns RAX
  (s==RAX), while RAX stays mapped to src1 — drop the mapping FIRST via
  `clobber_host_reg(RAX)` (spills if dirty) or a later reader of src1 in
  the same block reloads the broadcast. Both the vec path and the memory
  path need this; the memory path's trailing Q=0 clobber only masked the
  clean-src1 case.
- SIMD UMOV (vector element → GPR) is native for all element sizes and both
  Q values (table row in `simd_dp.txt`: mask `0xBFE0FC00`, match
  `0x0E003C00`). UMOV shares the ASIMDINS encoding group with INS
  (bits[15:12]=0001 → 0x0E001C00) and SMOV (0010 → 0x0E002C00); UMOV's
  bits[15:12]=0011 picks it alone — do NOT use a guard that only checks
  bit12, and do NOT match 0x0E002C00 (that's SMOV, still interp, no sign
  extend). The game's memset does `umov x1, v0.d[0]` (0x4E083C01) right
  after the dup to read the broadcast back. imm5 encodes esize AND index
  (esize = 1 << ctz(imm5), index = imm5 >> (ctz+1)); the `size` field
  (bits[23:22]) is 00 and must stay 00 in the match. Q=0 → Wd, Q=1 → Xd.
  JIT: NOT vec-cache compatible (never pinned), so reading `cpu.v_lo`/
  `cpu.v_hi` directly is always current; compute qword = (index*esize)/8
  (0 → v_lo, 1 → v_hi) + byte offset, zero-extending load of esize bytes
  (emit_load32/16/8 are movzx) into a fresh `alloc_reg()`, then
  `set_vreg_reg(dest, d)` (mirror the interp's full-element write).
  Translator skips rd==31 (XZR). `instr_will_call_interp` auto-syncs via
  classify (Family::UMOV ≠ UNKNOWN).
- SIMD 2-REG (CNT/NOT/RBIT/ABS/NEG), SIMD_CVTF (SCVTF/UCVTF/FCVTZS/FCVTZU),
  SIMD_ADDP (byte-pair), SIMD_XTN (XTN/SQXTUN/SQXTN/UQXTN), SIMD_TBL/TBX,
  and SIMD_INS (element,vector) are native in the JIT (jit_codegen_simd.cpp,
  1.5.3-alpha, memory path — never vec-cache pinned, so all XMM0-15 are
  free scratch and every helper is REX-aware). Field contract per op
  (ir_translate_fp.cpp SIMD_DP case): 2REG/CVTF/XTN → `imm`=subop,
  `width`=esize (CVTF always 4), `flags_op`=Q; ADDP → `flags_op`=Q;
  TBL → `flags_op`=(is_tbx<<1)|Q, `imm`=nregs (1/2); INS → `width`=esize,
  `imm`=dest-byte-offset, `aux`=src element index, `flags_op`=Q (unused,
  built manually via IRInst because emit() has no aux arg). Do NOT shuffle
  these (the TBL case once read cond/flags_op and silently ran as TBX1 with
  Q=0 — bytes ≥8 came back 0). FCVTZS/FCVTZU truncation MUST use the F3
  prefix: `66 0F 5B` is cvtps2dq (ROUNDS, the 12.75→13 bug); truncation is
  `F3 0F 5B` (cvttps2dq, `sse2_f3` helper). TBL/TBX needs SSSE3 (pshufb:
  dst=TABLE, src=CONTROL; OOR control byte has bit7 → 0); esize-2/4 XTN
  pack via packssdw/packsswb, esize-4 SQXTUN/UQXTN need SSE4.1
  (packusdw/pminud) else CALL_INTERP. `instr_will_call_interp` stays
  classify(…)==UNKNOWN-driven so no gate edits were needed. New test:
  `ctest/jit_simd_misc.c` (30 checks, "checks passed" pattern). Test-expected
  values for ABS use the shift/xor abs form — the scalar JIT had a
  sign-extension bug for negative operands that the src-fill loop exposed
  as a cneg/abs miscompile (see the SBFM note below).
  **FIXED (2026-08-15):** the general-case SBFM sign-extension in
  `jit_codegen_alu.cpp` shifted by `width - field_width` and then used a
  64-bit `sar` — for a 32-bit op (sxtb/sxth/sbfx W) the sign bit lands at
  bit 31 but the 64-bit SAR reads bit 63, so `sxtb w3, w21` of byte `0xf8`
  returned `0xf8` (248) instead of `0xfffffff8`, turning `cmp w3,#0`+`csel`
  into the wrong branch. Fix: shift by `64 - field_width` (sign bit reaches
  bit 63) and let the trailing `mov %eax,%eax` truncate to the W container.
  Verified: cneg3 (was `want=248` for i=0), all cneg/scalarabs repros,
  203/203 suite, `BIFROST_JIT_VERIFY=1`+`JIT_VERIFY_MEM`, `REGALLOC_CHECK`,
  `ENABLE_FWD=1` all clean. Interp was already correct.
- FWD (`BIFROST_ENABLE_FWD=1`, the `arm_reg_cache` load-forwarding in
  `ir_optimize.cpp`) is disabled by default: it had a "subtle correctness bug"
  since the original author (commit 1257f7b). The original regalloc clobber bug
  is fixed (jit_helpers.cpp `emit_fmov_helper` spills RAX before reuse), but
  ANY op that writes an ARM reg vreg DIRECTLY (bypassing STORE_REG) — currently
  `FP_F2I`, `FP_F2I_FIXED`, and `SIMD_UMOV` (jit_codegen_simd.cpp
  `set_vreg_reg(inst.dest, d)`) — MUST also update `arm_reg_cache[dest]=dest`
  in `optimize_ir`, or a later LOAD_REG of `dest` substitutes a stale cached
  vreg. If you add another such op, mirror the SIMD_UMOV case
  (ir_optimize.cpp) or FWD will silently corrupt values (this broke jit_neon's
  umov tests). Verified 198/198 under FWD=1; it gives only ~4% on chunkmesh_mesh
  (the real cost there is regalloc spill/reload bloat, not round-trips).
  **FIXED (2026-08-14): the deterministic corruption under `BIFROST_ENABLE_FWD=1`
  (bench_mips printed ~1 GB of spaces + `done: acc=0x0c5a4000` instead of
  `done: acc=0xf800800a2c4ff835`; suite SIGSEGVs by the second test) was NOT in
  the forward-walk cache — it was a regalloc bug in `load_vreg_to_reg_fast`
  Tier 1.5 (x86_regalloc.cpp): it emitted `emit_mov_reg(dst, home)` while `dst`
  was still mapped to a DIFFERENT dirty vreg, then `flush_dirty_host_regs`
  wrote the new value into the old vreg's stack slot (e.g. v53 spilled into
  v48's `[rbp-0x80]`). FWD's shorter IR exposes it because scratch vregs stay
  live/cached in RAX across LOAD_MEM; without FWD the extra LOAD_REG/STORE_REG
  ops break the pattern. Fix: `clobber_host_reg(dst)` BEFORE the mov (same
  class of bug as the SIMD DUP broadcast — drop the mapping, spilling if dirty,
  before overwriting the register). Re-verified: bench_mips byte-for-byte,
  `BIFROST_JIT_VERIFY=1` + FWD=1 → zero divergences, `BIFROST_ENABLE_FWD=1`
  run_tests.sh = 198/198, plain suite = 198/198. FWD is still OFF by default;
  it measures ~6% on bench_mips (~0.91s vs ~0.97s) but gives ~4% on
  chunkmesh_mesh — leave the env default alone unless the game shows a win.
  **FIXED (2026-08-15): the last FWD-only failure (scratch sxtest.c sbfx loop,
  `sbfx i=0 src=88 got=8 want=2147483640`, both should be -8; ~16 failures
  pre-SBFM-JIT-fix → 1 after) was NOT in the forward-walk cache or the regalloc
  — it was the SBFM/UBFM CONSTANT FOLD in `ir_optimize.cpp` (~line 590). The
  fold computed the `imms >= immr` (extract) case as `ROR(a, immr) &
  ones(imms+1)` with sign-extend from bit `imms`. ROR is only equivalent to
  `a >> immr` when immr == 0 (SXTB/SXTH/SXTW, the only forms the fold had ever
  exercised), because ROR wraps the low immr bits to the TOP of the register
  and ones(imms+1) keeps them: `asr w4, w2, #4` (SBFM #4,#31) of 0x88 folded
  to ROR(0x88,4)=0x80000008 instead of 8, and `sbfx w3, w2, #4, #4` (SBFM
  #4,#7) folded the 4-bit field 8 to +8 instead of -8 (sign bit lives at bit
  imms-immr=3, not imms=7). The wrong `want=2147483640` was exactly the
  folded `0x80000008` minus 0x10. Fix: mirror `interpreter.cpp:446-513`
  exactly — extract case `(a >> immr) & ones(imms-immr+1)` + sign-extend from
  bit (imms-immr); rotate case (`imms < immr`, SBFIZ/UBFIZ/BFI/LSL) field =
  `a & ones(imms+1)` + sign-extend from bit imms, then `<< (width-immr)`;
  guard `len==64`/`fw==64` (no UB shifts). FWD alone exposes it because
  `arm_reg_cache` propagates the source constant into the loop's first
  iteration (i=0); without FWD the LOAD_REG stays a real load and consts is
  cleared. Re-verified: sxtest ALL OK under FWD/JIT/VERIFY/VERIFY_MEM/
  REGALLOC_CHECK, 203/203 suite, 198/198 quick, FWD quick 198/198,
  `BIFROST_JIT_VERIFY=1` 203/203 + 198/198, `REGALLOC_CHECK` 198/198,
  FWD+VERIFY+MEM quick 198/198, all cneg/scalarabs/neon/permute/xtn repros.**
- `emit_taken_path_epilogue()` must NOT clear the vec-cache dirty flags:
  `vec_cache_writeback_all()` clears `vec_dirty_` as a codegen-time side
  effect, and the FALL-THROUGH (main) epilogue is emitted LATER — if the
  taken path already cleared the flags, the fall-through epilogue emits NO
  writeback and dirty vectors (e.g. memset's dup broadcast) are lost on
  the fall-through exit. Call it with `vec_cache_writeback_all(false)`.
  Self-loop blocks are exempt (they use the self-loop slot, not
  `emit_taken_path_epilogue`), which is why the FMA loops never hit this —
  a non-self-loop conditional branch (memset's `b.hi`) exposed it as a
  corrupted memset → PNG inflate "bad huffman code". Diagnose with
  `BIFROST_JIT_DUMP=1` and objdump the generated x86 (block dump is the
  raw bytes after "→ N bytes of x86 code"); `BIFROST_JIT_VERIFY` +
  `BIFROST_JIT_VERIFY_MEM` won't catch it because chains skip the
  verified epilogue and the divergence is a memory write, not a register.
- The fp-cache pre-call guard around BL_CALL/BLR_CALL (`vec_cache_writeback_all_pinned`
  in jit_codegen_vec_cache.cpp, called from frostjit.cpp) MUST flush every
  pinned reg that is written ANYWHERE in the block, not just the regs
  statically dirty AT the call point. The static set is computed at codegen
  time and misses loop-carried dirtiness: a self-loop block that does
  `bl grad3; fadd s8,s8,s0` keeps the accumulator s8 cached across the
  self-loop back-edge, so on the NEXT iteration s8 is dirty when the call
  executes — but the pre-call writeback was emitted with s8 clean (the fadd
  comes after the call in the IR) and skipped it, so the post-call reload
  (`vec_emit_prologue_loads`) read a STALE cpu.v_lo[8] and the sum
  re-accumulated from the wrong base (JIT `g=41.0` vs interp `g=16.0` on an
  8-iter grad3 loop; 2M-loop JIT=6000001 vs interp=-500000; the host x86
  ground truth was the interp value). The exact minimal flush set is
  `dirty-at-call ∪ loop-carried` = `written-anywhere-in-block`, tracked as
  `vec_written_this_block_[]` filled by the fp_cache_may_enable pre-scan
  (dest-write ops: FP_BINOP/UNOP/MOV/MOVI/I2F, FCVT_S2D/D2S, FRINT, the FMA
  family, FMOV_G2F, FP_F2I_FIXED fp_dest). Clean regs write back identical
  values, so over-flushing is always safe. Cost: ~0.5% on the game (26.5 →
  26.4ms/column) — the post-call reload reads every pinned reg anyway.
  `BIFROST_NO_FP_CACHE=1` or `BIFROST_NO_SELFLOOP=1` both dodge the bug
  (no pinning / no carried dirtiness); the divergence is deterministic and
  JIT_VERIFY/MEM blind to it.
- Vector FMOV immediate (cmode=0xF in the AdvSIMD modified-immediate block,
  e.g. `fmov v31.2d, #20.0` = 0x6F01F69F) is NOT a NOP: expand via
  AdvSIMDExpandImm. 64-bit (op bit29 set): `(imm8&0x3f)<<48`, sign bit →
  bit63, exponent = `imm8&0x40 ? 0x3FC0000000000000 : 0x4000000000000000`,
  replicated to both 64-bit lanes. 32-bit (op clear): sign→bit31,
  exponent = `imm8&0x40 ? 0x1F000000 : 0x40000000`, mantissa `(imm8&0x3f)<<19`,
  replicated per 32-bit lane. A NOP here corrupts Qt QRectF values built
  with `fmov v.2d,#imm` + `str q` (NaN rects → broken rounded rect).
  JIT falls back to CALL_INTERP for this (not in the simd_dp table).
- AdvSIMD modified-immediate MOVI/MVNI/ORR/BIC/MSL is Family::MODIMM in the
  simd_dp table (mask `0x9F800C00`, base `0x0F000400`, guard `immh
  bits[22:19]==0`) and MUST precede the SHIFT rows: every 32-bit `movi
  vN.2s` has immh==0 and would otherwise be swallowed by the esize==1
  SSHR path (the game's `movi vN.2s,#imm` was ~500K interp executions).
  cmode = bits[15:12], imm8 = bits[18:16]:bits[9:5], op=bit29 (MVNI/BIC),
  Q=bit30; ORR/BIC read the DESTINATION as their source (IR SIMD_ORRIMM is
  a read-modify-write, not a copy). JIT: `movabs`+`vmovq` (+`vmovddup`
  for Q=1), ORR=`vpor xd,xd,xmm0`, BIC=`vpandn xd,xmm0,xd` (imm in scratch
  XMM0). CRITICAL: do NOT add a SHRN exclusion clause to the guard
  (`bits[15:10] != 0x21`): every valid SHRN has immh (bits[22:19]) >= 1,
  so `immh==0` already separates it, while cmode=8 MOVI (16-bit LSL #0)
  ALSO has bits[15:10] = 0x21 — the crude clause made `movi vN.4h,#imm`
  silently return 0 in BOTH interp and JIT (0x0F058560 = movi v6.4h,#0xab
  → all zeros). The `immh==0` guard is the only safe discriminator.
  FMOV (cmode=0xF) is excluded by the guard and stays on the interpreter.
- FP-FMA semantics: FMADD = c + a*b, FMSUB = c − a*b, FNMADD = −(a*b + c),
  FNMSUB = a*b − c. FNMADD/FNMSUB are NOT −a*b±c aliases — encoding those
  wrong corrupts any value computed via `-(a*b+c)` / `a*b−c` (musl `pow`,
  `rgba_lerp`'s lab conversions). Keep interp, JIT FMA3 map, and IR in sync.
- FP_BINOP FNMUL (opcode 0x8, the negated multiply — NOT the FMA family)
  must negate with a WIDTH-AWARE sign mask: single flips bit 31, double
  flips bit 63. A hardcoded 0x8000000000000000 only negates doubles —
  movss-loaded single-precision values live in bits 0-31, so the bit-63
  XOR is a no-op and `fnmul s` silently returned +a*b. cglm's `glm_ortho`
  computes its translation row with `fnmul s` (`-(left+right)*rl`): the +1
  instead of −1 pushed every HUD quad to NDC > 1 (off-screen) under JIT
  while the interp (which does `r = -(a*b)` for both widths) drew it fine —
  the "JIT has no HUD" bug. Match the FABD (0xD) width-aware mask pattern.
- Scalar FP→int conversions (FCVTNS/FCVTPS/FCVTMS/FCVTZS + U variants) are
  native in the JIT. rmode = bits[20:19] (0=N nearest-even, 1=P +inf,
  2=M −inf, 3=Z toward-zero), bit[18]=A (ties-away), bit[16]=U. The IR
  translator matches the WHOLE family via `(op & 0x7F220000) == 0x1E200000
  && ((op>>10) & 0x3F) == 0` (the bits[15:10]==0 guard excludes FCSEL
  0x1E200C00 and FCVT D↔S 0x1E6240C0) and passes the rounding mode in the
  FP_F2I `cond` field `(is_away<<2)|rmode`. The JIT lowers N→`cvtsd2si`
  (MXCSR nearest, matches llrint under default host rounding), P/M→
  `roundsd/roundss` (SSE4.1, `has_sse41()` gate with CALL_INTERP fallback)
  then `cvttsd2si`, Z→`cvttsd2si` (truncate). FCVTAS (ties-away) and
  unsigned non-Z still fall back — do not "just add" them without also
  updating `instr_will_call_interp`'s mirror (it returns TRUE for those
  so the block-split gate agrees with the translator). A mask of only
  `(op & 0x7F3E0000) == 0x1E380000` (FCVTZS/FCVTZU only) silently routes
  every `floor()` (fcvtms) and `ceil()` (fcvtps) through the interpreter —
   Minecraft chunk/mesh math ran half in interp (interp share 13.9% → 7%).
- SIMD-scalar int↔FP conversions with FP register source/dest (the 0x5E200800
  "two-register-misc" group: `scvtf s2, s2` opcode 0x1D, `fcvtzs s2, s2`
  opcode 0x1B; size=bit22, U=bit29) are native in the JIT via
  `FP_I2F_FIXED`/`FP_F2I_FIXED` with `imms=1` (FP source/dest) and
  `immr=0`. CRITICAL: in those two codegen ops `immr` is the fbits field
  and its sentinel is `immr ? immr : 64` — an emit with `immr=0` (plain
  integer conversion) was treated as **fbits=64** and divided every terrain
  coordinate by 2^64 (all heights → 0 → flat water + void). Keep `immr`
  meaning raw fbits (0 = no scale) and skip the 2^±fbits multiply when
  `fbits==0`. GCC/clang emit the FP-source forms for `(float)int_var` when
  the int already sits in an FP register — the voxel game's chunk math does
  `scvtf sN, sN` on every coordinate (~1.6M interp executions before this).
- FP→int saturation in the JIT must PRE-CHECK the input range, never trust
  `cvttsd2si`'s INT64_MIN sentinel: x86 returns 0x8000000000000000 for ALL
  out-of-range inputs, so a post-convert clamp can't tell +overflow from
  −overflow (2^64 → 0 via the subtract-2^63 wrap; positive overflow → the
  negative clamp). Range-check against ±2^63 (signed) / 2^64 (unsigned)
  first and saturate explicitly; the subtract-2^63 trick is then exact and
  the width clamp only narrows in-range values. For unsigned-64 the sat_hi
  value must be width-aware (UINT64_MAX only for 64-bit dest — UINT64_MAX
  reads as negative sign-extended and the 32-bit clamp turns it into 0).
  The interpreter's FP-source fcvtzs also needs an explicit `std::isnan` →
  0 (C++ `static_cast<int32_t>(NaN)` is UB and yields INT32_MIN on x86).
- `instr_will_call_interp`'s FP gate polarity: "native" must predict NO
  interp call, so the return is `fp_gate >= 0 && !(fp_gate & bit)`.
  `fp_gate < 0 || (fp_gate & bit)` is INVERTED — under the default unset
  gate (-1 = all native) it returned TRUE for every native FP op, splitting
  FP-heavy blocks every 2 instructions and feeding the interp_only demotion.
- The interp_only demotion decision in `jit_translate.cpp` must use the
  ACTUAL `IROp::CALL_INTERP` count in the generated IR, NOT the heuristic
  `call_interp_count` from `instr_will_call_interp` (which over-predicts).
  With the heuristic, tiny native FP blocks (`[fcvtms, fcvtms]`,
  `[fcvtms, str, fcvtms]`) were demoted to the interpreter and ran ~2x
  slower. The actual-count rule leaves 0 interp_only blocks on the game.
- `[classprof]` in `interpreter.cpp` must loop `i < FP_SCALAR + 1` — the
  bound `SYS_NOP + 1` (107) hides SIMD_DP (108) and FP_SCALAR (109), so
  the histogram showed "no FP traffic" while FP was ~74% of interp time.
  `BIFROST_CLASS_PROF_PERIOD` overrides the 20M-instruction print period.
- UBFM/LSR pitfall: `lsr Xd, Xn, #0` (UBFM #0, #(datasize−1)) is a NO-OP —
  a shift by zero returns the source. Do not special-case it to 0; compilers
  emit `lsr w3, x19, #0` to grab the low 32 bits of a 64-bit constant during
  vec3/vec4 struct packing (returning 0 zeroed the z component of colors).
  The general UBFM extract path already yields the correct value.
- SIMD_LDST `src2` must be a REAL vreg, never literal 0: vreg 0 is guest X0
  (vregs 0-30 = X0-X30, 31 = SP, 32 = XZR — XZR only via `load_imm(b, 0)`).
  On the B/H/S/D vector load path (v_hi zeroing) the codegen reads
  `ensure_vreg(src2)`, so literal 0 loads X0's value into v_hi; on the store
  path `set_vreg_reg(src2, dhi)` clobbers guest X0 with v_hi[d.rt] — which
  corrupted `cmp x1, x0` in `test_simd_arith`'s Test 1 (JIT-only, interp
  passed). Loads use a `load_imm(block, 0)` zero vreg; stores use a fresh
  `g_alloc.alloc()` scratch for the unused v_hi half.
- The guest heap and stack MUST stay inside the 4 GiB direct window or
  every heap/stack access falls through to the `pages_` + rwlock slow
  path (~9× JIT regression; the voxel game dropped from ~18 MIPS to
  ~2 MIPS). `src/core/memory.h` was long described in comments as
  "v1.5.2: heap/stack inside the window" while the constants still put
  them ABOVE it (`MMAP_BASE_MIN=0x5000000000`, `STACK_TOP=0x8000000000`).
  Current layout: ELF+brk low, heap 256..768 MiB (`MMAP_BASE_MIN/MAX`),
  stack spans 944..1008 MiB (`STACK_TOP=0x3F000000`, 64 MiB). If you ever
  bump these, keep the comment AND `threads.cpp`'s backtrace heap-range
  check (which uses `Memory::MMAP_BASE_MIN/MAX`) in sync; `TRAMPOLINE_ADDR`
  (0x7000000000) deliberately stays above the window.
- `mmap_alloc` is NOT a pure bump allocator anymore: `munmap`
  (`untrack_allocation(addr, size)`) now frees the `pages_` entries,
  decrements `total_pages_`, and hands the address range back to
  `free_ranges_` (`std::map`, coalesced); `mmap_alloc` first-fits a
  reclaimed range before bumping `mmap_next_`, and zeroes reused pages
  (MAP_ANONYMOUS semantics). The old bump-only behavior was the
  minecraft game's crash: each frame's 18 MB DATA + 2.3 MB INDICES mesh
  buffers were mmap'd then munmap'd, marching the heap to ~8.5 GB
  (above the 4 GiB window → `pages_` slow path → 1M-page cap hit → mmap
  returned 0), and musl only treats -1/MAP_FAILED as failure, so mallocng
  built its arena at guest address 0 and the next `free()` BRK #1000'd in
  `get_meta` (misdiagnosed for weeks as "normal rc=133"). The mmap syscall
  also now returns `-ENOMEM` when `mmap_alloc` fails (was returning 0).
  Keep the OOM check counting ONLY pages that would actually be added
  (non-window pages absent from `pages_`) — a naive `aligned_size/PAGE`
  count spuriously trips the cap on reused window ranges. Keep
  `free_ranges_` under `mu_` (it's mutated by untrack/mmap_alloc/mremap)
  and copy it in `clone_for_fork`.
- `brk` must round the request up to page granularity (Linux semantics)
  and refuse growth into the stack region
  (`new_brk >= Memory::STACK_TOP - Memory::STACK_SIZE`). musl's variadic
  `syscall()` wrapper passes a STALE `x0` for a no-arg call, so a guest
  `syscall(214)` (brk(0)) with no explicit arg can hand the emulator a
  garbage stack-relative address; without alignment that left `brk_`
  unaligned and failed `test_brk`'s page-alignment check after the
  layout change (fails in BOTH JIT and interp). Tests calling brk(0)
  must use `syscall(214, 0)`; the emulator still aligns to be safe.
- JIT runtime hot-block promotion to interp_only is DISABLED by default
  (`BIFROST_HOT_INTERP=1` opts back in). Do NOT re-enable blindly: the
  translator already marks CALL_INTERP-heavy blocks interp_only
  (`call_interp_count*2 > instr_count` in translate_block), and demoting
  hot MIXED blocks (1-2 fallbacks + native ops) measured ~8-10% SLOWER
  on the minecraft game — interp is ~2x slower than JIT, so the native
  ops get dragged down to interpreter speed. Diagnose with the STANDARD
  profiling recipe (no clean guest exit and no manual gameplay needed):
  ```
  cd ctest_real/minecraft_weekend   # run from the game dir (res/ is relative)
  BIFROST_PROF=1 BIFROST_STATS_PERIOD=10 BIFROST_CLASS_PROF=1 \
      timeout 42 <repo>/bifrost-emu ./minecraft_weekend.elf 2>&1 | rg 'SIGPROF|guest:|block-end|syscalls|class'
  ```
  The game auto-simulates + generates/renders chunks with NO input, so a
  ~40s timeout-killed run is a valid profile. `BIFROST_PROF=1` is the
  SIGPROF sampler (jit/dispatch/translate/interp/other buckets; installs
  lazily on first run_block so install_host_signal_handlers doesn't
  overwrite it — a naive install crashes the game because SIGPROF is
  guest-forwarded). `BIFROST_STATS_PERIOD=N` prints the guest MIPS +
  block-end reasons AND (since b3a2a60) the SIGPROF bucket snapshot every
  N seconds MID-RUN: the exit-time dump only ran under `--verbose` and a
  clean guest exit, which games never reach (they exit_group / get killed
  by timeout), so periodic is the only reliable way to see the split.
  `BIFROST_STATS_PERIOD` also prints a syscall histogram
  (dump_syscall_histogram in syscalls.cpp: top-N syscall numbers with
  names + per-second rates; counted unconditionally with relaxed atomics,
  printed whenever the periodic reporter runs) that attributes the
  "other" bucket to specific syscalls. `BIFROST_CLASS_PROF=1` is the
  per-class dynamic histogram in the interpreter; with JIT ON it shows
  exactly which instruction classes run through the interp fallback.
- Block dispatch has THREE layers: a single-entry last-block cache, an
  inlined 256-slot direct-mapped inline cache (hash
  `((pc >> 2) ^ (pc >> 17)) & 255`), then the shared-mutex + unordered_map
  slow path. The fast paths are trimmed to a bare call/ret — the global
  safety-valve watchdog is a THREAD-LOCAL counter checked against
  `GLOBAL_BLOCK_LIMIT` (1e12), incremented on every dispatch with NO
  atomic (`lock xadd` was ~15-25 cycles per transition at 20M
  dispatches/sec). Do NOT re-add a per-dispatch atomic, a per-PC watchdog,
  or a `cpu.pc = next_pc` store to the fast paths. Both caches (last-block
  + inline) use the non-canonical `~0ULL` PC as the EMPTY sentinel and are
  written pc+fn together on the slow path, so a `pc == cached.pc` match
  alone implies a valid fn — do NOT add back a redundant `fn != nullptr`
  test (2 loads+cmp per dispatch), and do NOT switch the sentinel back to
  0 (guest PCs are 48-bit; ~0ULL can never collide).
- The UBFM/SBFM + LOAD_MEM/STORE_MEM regalloc "sandwich" elimination:
  these ops previously did `flush_dirty_host_regs(mask)` +
  `flush_scratch_host_regs(mask)` + `invalidate_host_regs(mask)` +
  `load_vreg_to_reg(dst, src)` whenever src was cached in dst — if a
  preceding LOAD_REG/ADD/SHL left the source vreg dirty in RAX (or RCX),
  that emitted `mov %rax,slot; mov slot,%rax` on the hot chunkmesh path
  (the 662-byte block 0x405304 had ~5-9 such pairs). They now go through
  `load_vreg_to_reg_fast()` (x86_regalloc.cpp), which is gated on the
  EXACT per-op liveness already computed in translate_block
  (`kills_per_op_[cur_op_index_]`, `cur_op_index_` set in the compile loop
  right before `compile_ir_inst`). Tier 1 (always safe): src cached in dst
  — still flush+invalidate (value preserved for later readers) but skip
  the redundant reload, since the flush never modifies the register. Tier
  1.5: src cached in ANOTHER reg the op clobbers — copy to dst first, then
  flush (avoids ADD/SHL→LOAD_MEM sandwich). Tier 2 (needs liveness): src
  is a DEAD scratch vreg (v>32, last use = current op, and v != inst.dest)
  — skip the flush of dst entirely and keep it mapped dirty for the op to
  consume; the trailing `set_vreg_reg(dest, dst)` (UBFM) or explicit
  `kill_vreg` (LOAD_MEM) drops the mapping. The dead-check MUST use
  `kills_per_op_` liveness, NOT the FWD env gate: `optimize_ir`'s CopyMap
  copy-substitution can make a scratch vreg multi-read even with FWD off,
  and `kills_per_op_` is the precise "no later reader" predicate. STORE_MEM
  decides BOTH operands via `vreg_fast_keep_candidate` BEFORE flushing
  (it also must not flush the other operand's kept reg), and relies on
  `emit_store_mem` PRESERVING RAX and RCX (it pushes/pops them around the
  slow call). LOAD_MEM now keeps its dest cached via `set_vreg_reg(dest,
  RAX)` instead of `store_reg_to_vreg` — the loaded value is usually
  consumed immediately (STORE_REG/next op) and store_reg_to_vreg killed
  the mapping forcing a reload sandwich; the epilogue flushes dest if the
  block ends first. Block 0x405304: 662 → 590 bytes. Do NOT add a FWD
  env gate to this fast path — the liveness check is sufficient and FWD
  remains disabled by default.
- SIMD GPR-flush discipline: GPR-free SIMD ops must NOT flush RAX/RCX/RDX.
  `flush_invalidate_host_regs` on an op that only uses XMM regs (RBX is the
  reserved base and never holds a cached vreg) unnecessarily evicts live
  vregs cached in those GPRs. Current truth per op in `jit_codegen_simd.cpp`:
  SIMD_LOGICAL / SIMD_ARITH / the shift family / AES PMULL need NO GPR flush;
  SIMD_CMP needs RAX only on the unsigned sign-flip path (`uns` gate);
  SIMD_FP_FMA needs RAX only (Q=0 v_hi zero); SIMD_FP_ARITH needs RAX
  (FABD mask + Q=0 v_hi zero); SIMD_ORRIMM needs RAX|RCX (union of the vec
  path's RAX and the memory path's RAX+RCX); SIMD_DUP uses precise
  `ensure_vreg`/`clobber_host_reg` instead of a broad flush. CALL_INTERP
  fallbacks self-flush (`flush_all_vregs` in `emit_call_interp`), so a
  pre-flush before a fallback is always redundant. When auditing an op for
  this, check EVERY emitted instruction path (native, vec-cache, fallback)
  and flush the UNION of actually-clobbered GPRs — don't copy a boilerplate
  RAX|RCX|RDX from an FP op.
- `flush_scratch_host_regs` (x86_regalloc.cpp) skips the spill store for
  CLEAN scratch vregs: every caller runs `flush_dirty_host_regs` on the same
  mask first (evicting all dirty vregs), so at flush_scratch time the only
  remaining cached scratch vregs are clean and the dirty-flag invariant
  (`dirty=false ⇒ stack slot current`) makes the store redundant. The
  `vreg_dirty_[v]` guard keeps the defensive standalone-call behavior from
  the header comment. This relies on dirty tracking being exact — the
  `BIFROST_REGALLOC_CHECK=1` suite (verify_dirty_host_regs_, jit_translate.cpp)
  is the guard. Do NOT revert to unconditional stores to "be safe": a clean
  scratch's store is provably dead code. Tier-4 measurement: neutral on
  bench_matrix/bench_mips/mixed SIMD+scalar microbench; the minecraft game
  frame-count A/B is unreliable (guest clock stall + ±30% same-binary spread).
- Every block ends in a 5-byte chain slot (`ret` + 4 NOPs) patched to
  `jmp rel32` once the target is translated. Conditional branches
  (BRCOND/CBZ/CBNZ/TBZ/TBNZ) ALSO emit a second chain slot on their TAKEN
  path (`emit_taken_path_epilogue()` in `jit_codegen_branch.cpp`); this
  halves dispatch overhead (~21% → ~10% wall time on the minecraft game)
  because loop-back edges no longer return to the C dispatcher. Skipped
  when a self-loop slot exists. Both slots live in `BlockEntry`
  (`chain_patch_off`/`taken_chain_patch_off`, `chained`/`taken_chained`),
  both register in `back_refs_`, and both are un-patched during
  `BIFROST_JIT_VERIFY`. Do NOT remove the taken-path slot or restrict it
  to BRCOND — CBZ/CBNZ/TBZ/TBNZ while-loop edges (GCC vectorized
  memchr/strchr, pointer loops) rely on it.
- Chain-skip (`BIFROST_CHAIN_SKIP=1`, DEFAULT OFF — `--chain-skip` in
  run_tests.sh): a second block entry `BlockEntry.chain_entry` recorded in
  `jit_translate.cpp` right after the prologue's `mov rbx,rdi`/`mov r14,rsi`
  (BEFORE the R10 window load and vec prologue loads). Chain slots patch to
  `chain_entry` instead of `fn`, so a chain edge skips the predecessor's
  frame teardown AND the successor's push/frame/reg-setup (~17 instrs/edge).
  The chain ROOT allocates ONE unified 32 KB frame
  (`kChainSkipFrameBytes = 0x8000`, the vreg-space ceiling: 4095 vregs × 8 B)
  that every block in the chain reuses; successors enter at `chain_entry`
  without allocating. The epilogue becomes a lease: [state flush: flags +
  flush_all_vregs + vec writeback + store PC + mov rdi/rbx rsi/r14] [5-NOP
  chain slot] [cold exit: mov rsp,rbp; pop×6; ret] — the cold exit is only
  reached when the slot is unpatched (chain edges `jmp` past it, keeping the
  chain root's frame). `patch_chain`'s unpatched-slot guard is `0x90` (5 NOPs)
  under chain-skip vs `0xC3` (ret+4 NOPs) otherwise; `BIFROST_JIT_VERIFY`
  un-patch restores 5 NOPs so the block falls through into the cold exit.
  CRITICAL invariants: (1) `chain_entry` MUST be recorded BEFORE the R10
  window load — R10 is only loaded by window-using blocks, so a chain into a
  window block from a non-window predecessor carries stale R10 (the first
  chain-skip bug: r10=0xd0 → SIGSEGV in `mov (%rax),%rax` after `add %r10,%rax`).
  (2) The default path of `emit_taken_path_epilogue` MUST keep the
  `mov rsp,rbp; pop×6` BEFORE the slot — dropping it makes the unpatched `ret`
  pop the dispatcher's return address off the block's own frame and jump to
  garbage (the second bug: RIP=0x555500000008). (3) RBX/R14 need no setup on
  the chain edge (reserved regs, never reassigned in a body). (4) vec blocks
  re-run their vec prologue loads at `chain_entry` (the predecessor's
  epilogue wrote back dirty vectors to cpu.v_lo/v_hi first). The stack-frame
  pre-scan in jit_translate.cpp must cover EVERY vreg the block may touch,
  including `inst.aux` (SMADDL/SMSUBL accumulator) — any missed vreg gets a
  lazy `vreg_stack_slot` (-8 * num_stack_slots_++) past the pre-allocated
  frame. Measured on
  multi-block workloads: bench_fib +18.8% (0.653→0.530s), bench_sort +16.6%
  (0.699→0.583s), bench_matrix +1.2%; bench_mips NEUTRAL (single self-loop
  block — the selfloop slot already skips all this overhead). Leave the env
  default OFF (opt-in) until the game confirms a win.
- BL_CALL's sibling BLR_CALL (`IROp::BLR_CALL`, the indirect/function-pointer
  form) is native too: `blr rn` emits it (ir_translate.cpp) whenever
  `!bl_call_disabled_`, codegen mirrors BL_CALL but loads the target from
  src1 (the rn vreg) into RAX/RDX at the call site instead of an immediate.
  It has the same chain-skip gate (BLR is counted into `bl_call_count` and
  `has_bl_call` like BL, so the cap and chain-skip handling agree). The
  worldgen noise `.compute` wrappers (combined/octave/expscale function
  pointers, ~40K indirect calls per fresh chunk column) use it. It measured
  NEUTRAL on the game's worldgen (the fresh-column heightmap is
  body-throughput-bound at ~430 MIPS — 40K noise3 × (156 instr + 8× the
  16-instr grad3 leaf) ≈ 11.4M instr in ~26ms — NOT dispatch/lookup-bound),
  but it's strictly more native than re-dispatching on every blr.
- `jit_call_helper` (the BL_CALL/BLR_CALL target dispatcher) uses
  `lookup_call_target` (frostjit.hpp), which mirrors run_block's tiers —
  thread-local last-block cache, inline cache, then the shared-mutex map —
  and POPULATES the caches on its slow path (run_block's slow path is the
  only other writer; jit_call_helper previously fell straight to the
  unlocked `blocks_.find` on every call). Also measured NEUTRAL on the
  game's noise (body-bound, see above) but removes a per-call map find from
  the hottest call path. Its dispatch loop uses the SAME thread-local
  watchdog as run_block (`++tls_call_blocks_ > FrostJIT::GLOBAL_BLOCK_LIMIT`,
  1e12, disables the JIT on trip) — do NOT reintroduce a per-invocation
  `steps > 10000000` cap: window_loop's helper legitimately dispatches >10M
  blocks in ~40s of gameplay, the old cap broke it mid-game and returned a
  garbage pc (0x41fd7c) into the caller's block. The caller's BL_CALL epilogue
  then overwrote cpu.pc with the static fall-through (0x400f38), main's tail
  block popped the frame twice and ret'd through a stack-resident LR
  (pc=0x3efffbb8) → DecodeError. Fixed 2026-08-15; the minecraft game now
  runs past the 200s mark where it previously crashed at ~40s.
- The prologue's 10-byte `movabs r10, window_base` (WIN_REG) is emitted
  LAZILY: only for blocks containing LOAD_MEM/STORE_MEM/ATOMIC/
  SIMD_LD16/SIMD_ST16. Don't unconditionally re-emit it — it's ~3-4 cycles
  of setup on every entry for blocks that never touch the direct window.
- Native syscall dispatch: the `IROp::SVC` codegen in jit_codegen_branch.cpp
  does NOT call the interpreter for non-vDSO syscalls. It emits
  `emit_call_native_svc` → `jit_native_svc(emu, cpu, svc_pc)`, which sets
  `cpu.pc = svc_pc + 4` (return address — signal frames must save the
  return address, else rt_sigreturn re-executes the SVC and re-enters a
  blocking syscall forever) and calls `Emulator::syscall()` directly,
  skipping step()/decode/dispatch. The vDSO clock stubs keep their own
  `jit_vdso_clock_svc` fast path. `unchainable_end_` stays true (syscall
  may modify pc, e.g. execve/rt_sigreturn) and `rax_holds_next_pc_` is set;
  the emit helper reloads pc from cpu.pc into RAX and invalidates ALL
  vregs after the call. Do NOT revert SVC to emit_call_interp: the game
  makes ~2M syscalls/run and the interp round-trip was ~6.3% of wall time
  (SIGPROF interp went 6.3% → 0.0%). No vec-cache interplay needed: SVC
  disqualifies the block in vec_cache_may_enable. `instr_will_call_interp`
  correctly returns false for SVC (not in its switch).
- Thunk SVC fast path (`jit_thunk_svc` in jit_interp.cpp): `jit_native_svc`
  branches on `cpu.regs[8] == GraphicThunk::SYSCALL_NUMBER` (0x1000) and
  calls `jit_thunk_svc` directly — replicating syscalls/misc.cpp's case
  0x1000 (GraphicThunk → AudioThunk → DisplayThunk → ENOSYS) but skipping
  `Emulator::syscall()` (drain_host_signals, running check, trace gates,
  six-handler pre-dispatch). Thunk calls are 99.8% of ALL game syscalls
  (~115K/s ramping to ~173K/s — the histogram cap-512 artifact hid this
  until SYSCALL_HIST_MAX was raised to 4097), so this is the hottest
  syscall path. The check is unambiguous (no real AArch64 syscall number
  is near 4096; the trampolines load 0x1000 into x8 right before the SVC).
  Skipping drain_host_signals mirrors the vDSO clock precedent (signals
  still drain at the run-loop boundary and at every real syscall). The
  histogram is still counted via `note_syscall(0x1000)` so
  BIFROST_STATS_PERIOD keeps attributing thunk volume. If the game ever
  shows stale signals, re-add drain_host_signals to jit_thunk_svc — do
  NOT route 0x1000 back through Emulator::syscall.
- Direct-window limit constants are emitted as `mov r32d, imm32`
  (`emit_mov_imm32_zext`, zero-extends) in all five bounds-check sites
  (emit_load_mem, emit_store_mem, ATOMIC in frostjit.cpp, SIMD_LD16, SIMD_ST16)
  — the limit `DIRECT_WINDOW_SIZE − w` is always < 2^32, so a 10-byte movabs
  is dead weight (5-6 bytes saved per memory access). Do NOT "shorten" the
  compare itself to `cmp r64, imm32`: imm32 SIGN-EXTENDS, so 0xFFFFFFF8
  compares against 0xFFFFFFFFFFFFFFF8 and every window hit falls to the slow
  path. 32-bit mov + 64-bit cmp is exact (both operands < 2^32).
- Constant-src2 folding in the JIT: `jit_consts_` (per-block
  `unordered_map<uint16_t,uint64_t>`) records the value of each scratch vreg
  whose defining op is `IROp::IMM` (populated in compile_ir_alu's IMM case,
  erased in translate_block's compile loop when any non-IMM op writes the
  same dest — defensive; the monotonic `VregAlloc::next++` makes vreg
  numbers unique per definition). The ADD/SUB/AND/OR/XOR case and the
  SHL/SHR/SAR/ROR case fold a constant src2 into an x86 immediate form when
  `vreg_last_use_this_op(src2)` (dead after this op) and `dest/src1 != src2`
  (must not be the write target or collide with the src1 value — `kill_vreg`
  before `ensure_vreg(src1)` frees the dead const's reg). CRITICAL rules:
  (a) the JIT ALU ops are ALWAYS 64-bit (guest W-reg results get a separate
  ZEXT from the translator), so a fold is only valid when
  `(int64)c == (int64)(int32)c` — ADD/SUB/AND/OR/XOR imm32 sign-extends;
  this covers 12-bit add/sub immediates, stack offsets, small masks
  (0xFF/0x3F/…), and −1, but NEVER masks ≥ 0x80000000 (e.g. `and x0,x1,#0xFFFFFFF0`
  must stay register-form) or 64-bit values whose low 32 bits don't
  sign-extend back. (b) shift counts fold via `emit_shift_imm8(d, kind,
  cnt & 0x3F)` (mod-64, x86 imm shifts self-mask), with the 32-bit-ROR
  special case emitting a REX.W-free `C1 /1 ib` with `cnt & 0x1F`. A register
  shift with a loop-invariant count does NOT fold (src2 is an ARM reg or a
  live vreg, not a dead IMM) — that's fine, the CL path reuses RCX across
  iterations. (c) the emitted immediate ops clobber RFLAGS exactly like the
  reg form, and clobber_flags() precedes the fold, so flag tracking is
  unaffected. Measured: bench_mips +3.7% (MIPS is addi/ori/andi/lui-heavy),
  bench_matrix/sort/memcpy/fib ±1% (noise). Suite 199/199, quick 194/194,
  regalloc-check 194/194, FWD 194/194, bench_mips byte-identical under
  JIT_VERIFY/JIT_VERIFY_MEM/FWD.

## Work Guidance

- Prefer fixing interpreter + JIT-fallback consistency for SIMD/FP
- Keep GraphicThunk marshalling explicit (stack args, FP args, string
  returns, nested `glShaderSource` pointers)
- Demo target: `ctest_real/test_sdl_gl_triangle.elf` (exit 0 = pass,
  77 = skip when SDL/GL/display unavailable)
- SIMD_DP decode on the JIT side is table-generated. `tools/opgen/simd_dp.txt`
  is the single source of truth for which SIMD_DP op is native and its
  sub-opcode; the interpreter (`interp_fp.cpp`) stays an independent,
  hand-written implementation so interp-vs-JIT divergence stays detectable.
  When adding/modifying a SIMD_DP op: edit the spec, run `make opgen`
  (regenerates `include/opgen_simd.hpp`), then `make opgen-check`
  (CI guard — fails if the committed header drifted from the spec). Both
  `jit_translate.cpp` (instr_will_call_interp) and `ir_translate_fp.cpp`
  classify via `arm64emu::simd::classify()`. Do NOT hand-edit the generated
  header or re-add ad-hoc sub3_noq/fp_key sm masks in those two files.
  FP_SCALAR is NOT table-migrated — it's ftype/opcode logic with FMOV/FCMP
  special cases; leave it hand-tuned.
- GraphicThunk symbols are table-driven the same way: edit
  `tools/opgen/thunk_dp.txt`, run `make opgen-thunk` (regenerates
  `include/opgen_thunk.hpp`), then `make opgen-thunk-check` (CI guard —
  fails if the header drifted from the spec). Do NOT hand-edit the
  generated header or re-add ad-hoc REG_* entries in thunk.cpp.
- GL3.3+/DSA future-game coverage (2026-08): `glBufferStorage` (SIZE
  `arg1`), `glCreateBuffers`, `glTexStorage2D/3D`, `glTexImage3D` (10 args,
  arg9 pixels via `p` = 64 KiB default bounce — a `z` token needs a SIZE
  policy, so pointer args without one must use `p`), `glBlitFramebuffer`,
  `glDrawArrays/ElementsInstanced` (Instanced uses `EL_PTR` on arg3),
  `glBindBufferBase/Range`, `glGetBufferSubData` (SIZE `arg2`),
  `glCopyBufferSubData`, `glCopyTexSubImage2D`, `glClientWaitSync`/`glFenceSync`
  (GLsync round-trips as an opaque integer), `glDrawBuffers`,
  `glBindFragDataLocation`, `glCreateVertexArrays` are all table rows.
  **glMapBuffer/glMapBufferRange/glUnmapBuffer/glFlushMappedBufferRange
  (2026-08) are NATIVE via a guest-window bounce**: the host glMapBuffer
  returns a HOST pointer the guest cannot deref (address-space mismatch),
  so the `MAP_BUFFER` dispatch arm allocates a bounce with
  `Memory::mmap_alloc` (inside the 4 GiB direct window → guest JIT reads/
  writes it fast), seeds it from the host buffer when `GL_MAP_READ_BIT`
  (0x1) is set (skipped under `GL_MAP_INVALIDATE_*`), and returns the
  bounce's GUEST address; `UNMAP_BUFFER` copies the bounce back into the
  host buffer via host `glBufferSubData` when `GL_MAP_WRITE_BIT` (0x2) is
  set, then `untrack_allocation`s it (returns GL_TRUE);
  `FLUSH_BUFFER` (glFlushMappedBufferRange) pushes just the flushed range
  early (GL_MAP_FLUSH_EXPLICIT_BIT / persistent-coherent best-effort).
  Transient map/write/unmap-per-frame works fully; persistent-coherent-
  WITHOUT-explicit-flush stays unsupported (writes never land). Buffer
  size comes from host `glGetBufferParameteriv(GL_BUFFER_SIZE)` resolved
  at init (NOT a registered thunk symbol — resolve via dlsym like the
  GLFW fns). The current target→buffer binding is read from the
  GLStateTracker's general `buffer_bindings_` map (extended to cover ALL
  glBindBuffer/Base/Range targets, not just ARRAY/ELEMENT); `glMapBuffer`
  on an unbound buffer returns NULL (matches GL). Raw host fns
  `glGetBufferParameteriv`/`glGetBufferSubData`/`glBufferSubData` are
  resolved at init alongside the GLFW fns. `glGetBufferParameteriv` is
  ALSO a table row with the `QUERY` policy (falls through to host — the
  tracker doesn't answer GL_BUFFER_SIZE). New guest test:
  `ctest_real/test_sdl_gl_mapbuffer.c` (14 checks, "ALL PASS" pattern,
  exit 77 = skip without GL/SDL/display). Still UNSUPPORTED:
  `glDebugMessageCallback`'s callback is a GUEST function pointer that must
  NOT be handed to the host setter (mirror the `*_CB`/error-callback
  interception pattern) — do not add it as a plain passthrough row.
- **DisplayThunk (Vulkan/Wayland/X11/XCB/GBM/GLX/RandR/Xkb) is
  table-driven too (1.5.3-alpha):** the SAME `tools/opgen/thunk_dp.txt` →
  `opgen_thunk.hpp` pipeline now carries the ~275 display symbols
  (families `VK`/`WL`/`WL_EGL`/`X11`/`X11XCB`/`XCB`/`GBM`/`XEXT`/`GLX`/
  `RANDR`/`XKB`; policies `PROXY`/`VULKAN`/`VK_GET_PROC`/
  `VK_CREATE_INSTANCE`/`VK_CREATE_DEVICE`/`VK_PRESENT`; size kinds
  `X_DRAWSTR`/`X_SETWMPROTO`). The hand-rolled `REG_VK*/REG_WL*/REG_X11*`
  macro ladders in `display_thunk.cpp` are GONE. `DisplayThunk::
  register_known_symbols_` iterates `thunk::specs`, filters to the display
  families, derives the legacy ABI shape (`pointer_args`/`n_stack`/
  `n_float`/`flags`) from the ARGS column, and registers each symbol under
  every soname of its family. ARGS semantics: `'p'/'z'` = translated
  pointer, `'i'` = VERBATIM arg (opaque Vk*/wl_*/XID/Display* handles
  round-trip as the host pointer the host returned); ARGS length > 8 ⇒
  stack args (n_stack = len−8); mixed int+float ⇒ `THUNK_MIXED_FP` with
  n_stack = int arity. Dispatch routes by POLICY (not symbol name) for the
  deep-marshalling entry points (`VK_GET_PROC`, `VK_CREATE_INSTANCE`,
  `VK_CREATE_DEVICE`, `VK_PRESENT`) and sizes bounces by `SizeKind`
  (thunk.cpp's `SizeKind` switch gains a `default:` so new kinds are safe).
  X11/WL host-fallback masks reproduce the pre-migration registrations
  FAITHFULLY (the proxy path is name-driven and authoritative; masks only
  affect the headless host-lib fallback) with two corrections: XCreateWindow
  carries all 12 args (n_stack 4, so its XSetWindowAttributes* stack arg is
  read/translated instead of dropped), and XChangeProperty's old arg-6 size
  override is dropped (dead code — its mask bit marks arg 7). Adding a
  display symbol = one spec row + `make opgen-thunk`; `thunk.cpp` filters
  its own loop to GL/GLES/EGL/SDL/GLFW so it never indexes `kFamilies`
  with a display-family value.
- GLFW callbacks (1.5.3-alpha): the callback setters — `glfwSetCursorPosCallback`
  (`CURSOR_CB`), `glfwSetKeyCallback` (`KEY_CB`), `glfwSetMouseButtonCallback`
  (`MOUSE_CB`), `glfwSetFramebufferSizeCallback` (`FRAMEBUFFER_CB`),
  `glfwSetWindowSizeCallback` (`WINDOW_SIZE_CB`), `glfwSetWindowFocusCallback`
  (`FOCUS_CB`), and `glfwSetErrorCallback` (`ERROR_CB`, global — single arg,
  no window) — all have `*_CB` policies in `tools/opgen/thunk_dp.txt`.
  The dispatch in `thunk.cpp` intercepts them BEFORE the generic host-fn
  path and stores the guest AArch64 callback in `impl_->glfw_cbs_`
  (a per-window `GlfwWindowCbs` struct; `ERROR_CB` in `glfw_error_cb_`) —
  NEVER hand the guest address to the host setter (host would call it as
  x86-64 → SIGSEGV). There is NO `STUB` policy anymore: every former STUB
  setter is a real `*_CB` policy (removing the last STUB rows also removed
  `Policy::STUB` from the generated enum — don't reference it).
  `glfwPollEvents`/`glfwWaitEvents` have policy `GLFW_POLL`; after the
  host call returns, dispatch calls `impl_->deliver_glfw_callbacks_(cpu)`
  which reads the host state via `dlsym`-resolved fns
  (`glfwGetCursorPos`/`glfwGetKey`/`glfwGetMouseButton`/`glfwGetWindowSize`/
  `glfwGetFramebufferSize`/`glfwGetWindowAttrib`) resolved in
  `register_known_symbols_`, and fires each callback ONLY when its value
  changed since the last poll (first poll just seeds so the game sees no
  spurious startup event — GLFW semantics: fire on events only). CRITICAL
  details: `glfwGetKey` only accepts keys >= `GLFW_KEY_SPACE` (32) —
  polling 0-31 makes host GLFW fire `GLFW_INVALID_ENUM` "Invalid key N"
  on EVERY poll; start the key loop at 32. Host GLFW errors are captured
  by a host-side error trampoline installed in `register_known_symbols_`
  (`glfw_set_error_callback_fn_` + a static impl pointer) and forwarded to
  the guest's error callback with change-dedup (a `const char*` desc is
  bounced via `cache_host_string_`), so the game polling invalid keys
  doesn't spam the guest every frame. The borrow-CPU runner ABI is generic:
  `uint64_t(CPU& cpu, uint64_t fn, const int64_t* iargs, size_t n_iargs,
  const double* fargs, size_t n_fargs)` — x0.. = iargs, d0.. = fargs,
  LR = SENTINEL_LR (0x1000), scratch stack (thread-local, one per guest
  thread), save/restore ALL CPU state around the `step()` loop — same
  borrow-CPU pattern as `guest_call_args_` (dynamic_linker), proven
  reentrant from inside the syscall path (dlopen 0x1002 →
  `guest_call_args_`). Wire it via `Emulator::wire_thunk_glfw_cb_runner_`
  right after `thunk->init(mem_)` on BOTH the dynamic-linker (emulator.cpp
  ~237) and static-ELF (~711) paths. `BIFROST_THUNK_TRACE=1` prints
  `[thunk] <name>: window=0x.. cb=0x..` and `[thunk] <kind> cb → 0x..`
  lines for verification. Adding more GLFW callback setters later:
  mirror `*_CB` (store in `GlfwWindowCbs` + deliver in
  `deliver_glfw_callbacks_`), don't leave them STUB unless the game can't
  use them. The `FRAMEBUFFER_CB` delivery is what lets the game's
  `_size_callback` update `window.size` + `glViewport` on resize/fullscreen
  (before it was STUB → HUD stayed at the initial size on the user's
  ultrawide).
- `make check-all` now runs BOTH generation guards (`opgen-check` +
  `opgen-thunk-check`) before the test suite, so spec drift fails CI.

## Verification

- `make` (plain make auto-enables GL/SDL2/EGL thunking)
- `make check-all` — the "everything" target: build + `setup-tests` +
  `setup-rootfs.sh` + `./scripts/run_tests.sh` (default suite = **203 pass /
  0 fail / 0 skip**: unit + integration + toybox + real-world +
  benchmarks + dynamic + interactive). The only historical skip was
  `test_dladdr_glibc`, which must be a glibc-DYNAMIC binary or its dlopen
  stub skips with exit 77.
- `./scripts/run_tests.sh` — the default is the FULL suite
  (interactive + real-world are the standard default) = **203 pass /
  0 fail / 0 skip**. Subsets: `--quick` (no benches, 198),
  `--unit`, `--jit`, `--interp`, `--dynamic`, `--no-rootfs`. Exit 0 =
  all pass, 77 = env-dependent skip (treated as pass).
- `./bifrost-emu ctest/jit_mvni_softfloat.elf`
- `./bifrost-emu ctest/jit_neon_permute.elf`
- `./scripts/run_tests.sh --dynamic` (includes `test_dlopen`)
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`
  (exit 0 = pass, 77 = skip when SDL/GL/display unavailable)

### Test binary toolchains (how `make setup-tests` builds them)

The suite has **203 tests** across categories (unit/JIT/interp, syscalls,
integration, interactive, toybox, real-world, benchmarks, dynamic linking).
Test `.elf` files are gitignored and rebuilt from `ctest/*.c` +
`ctest_real/*.c` by `make setup-tests` (also run by `check-all`). Three
toolchain flavors, per binary:

- **musl-static** (default): `aarch64-linux-musl-gcc -static -O2` — most
  tests; self-contained, no rootfs needed. Includes the dl* tests that
  call the internal syscall directly (`test_dlopen`, `test_dladdr`).
- **glibc-dynamic** (`GLIBC_DYN_SRCS` in the Makefile):
  `aarch64-none-linux-gnu-gcc -O2` WITHOUT `-static`, so they exercise the
  ELF loader, glibc interpreter, and real `dl*`/pthread plumbing:
  `test_dladdr_glibc`, `test_dyn_hello`, `test_dyn_write`, `test_dyn_malloc`,
  `test_dyn_printf`, `test_dyn_pthread_min`, `test_dyn_threads`,
  `test_dyn_pthread_stress`, `test_dyn_pthread_8thread`. Must run with
  `BIFROST_ROOT=rootfs`. DO NOT let setup-tests rebuild these as musl-static
  (they'd silently pass as static stand-ins or skip).
- **musl-dynamic**: `aarch64-linux-musl-gcc -O2` (no `-static`) for the
  `*_musl`/`*_glibc` variants whose `.elf` name differs from the `.c`
  (`hello_dyn_musl.elf`, `hello_dyn_glibc.elf` ← `hello_dyn.c`;
  `test_dyn_full_musl.elf` ← `test_dyn_full.c`).

Dynamic tests need the rootfs (`scripts/setup-rootfs.sh`, builds from the
glibc toolchain's libc) and the glibc cross toolchain
(`tools/fetch-glibc-toolchain.sh`). `setup-tests` fetches both toolchains if
missing. When touching a dynamic test: rebuild with the correct toolchain,
not musl-`-static`.

## Child DOX Index

(none — single-tree emulator; parent Downloads rail indexes this folder)
