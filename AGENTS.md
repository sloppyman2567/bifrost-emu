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
  (SMAXP/SMINP/UMAXP/UMINP) is native in the JIT (`SIMD_PAIRMIN`,
  1.5.3-alpha) AND covered in the interp; the interp sub_noq case needs
  ALL SIXTEEN labels (0x0E*/0x2E* × 0xA400/0xAC00 × sizes 0-3) — the
  old code listed only the UMAXP max forms, so SMAXP/SMINP/UMINP threw
  DecodeError in interp-only mode and the signed forms were invisible to
  JIT_VERIFY (verify compares JIT vs interp, so a missing interp label =
  DecodeError, not a value mismatch). Do not narrow it back to size=0 —
  GCC's vectorized memchr/strchr emit `umaxp v31.4s` (size=2), which a
  size-0-only case label silently DecodeErrors (SIGILL in interp-only,
  SIGABRT via JIT CALL_INTERP). Q=0 pairwise SEMANTICS: BOTH sources
  always contribute — Vd low 8 bytes = {pairwise(Vn), pairwise(Vm)},
  the Vm half is NOT zeroed (real-ARM; the sub3_noq UMINP handler at
  interp_fp.cpp ~2649 always did this, and `test_simd_saddw_uminp` Q=0
  checks depend on it). The interp 0x20A400 handler originally gated the
  Vm half on Q (Q=0 → {Vn pairs, 0,0,0,0}) and the first JIT attempt
  copied that; a punpcklqdq merge for Q=0 ALSO breaks it (pushes Vm's
  bytes into v_hi, which store_vec discards) — Q=0 needs
  `pslldq X5,4; por X4,X5` (each XMM holds its out_bytes=4 results in the
  low bytes, high bytes 0x80'd by the pshufb masks); only Q=1 uses
  punpcklqdq. `ctest/jit_simd_pairmin.c` covers all 4 ops × esizes
  {1,2,4} × both Q, plus the teeworlds `uminp v0.16b,v0.16b,v0.16b`
  self form.
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
  **FIXED (2026-08-18):** the interpreter's FP→int conversions were REWRITTEN
  to route through two file-scope helpers in `interp_fp.cpp`,
  `fp_to_signed_sat()`/`fp_to_unsigned_sat()` (is_64bit param), used by ALL
  five conversion sites: the rounding-mode scalar family (FCVTNS/PS/MS/AS/ZU,
  rmode bits[20:19], is_away bit18, U bit16 — the lambdas now return the
  rounded `double` via `std::nearbyint`/`round`/`ceil`/`floor`, NOT a
  pre-truncated `int64_t`), the scalar FCVTZS/FCVTZU integer variant
  (0x7F3E0000), the fixed-point FCVTZS/FCVTZU (`fpfixed::FIXCONV` subop 1),
  the FP-scalar 64-bit 0x5E group (opcode 0x1B, FP source/dest), and the
  vector FCVTZS/FCVTZU (esize 4 and 8). The bug was `static_cast<uint64_t>(d)`
  of a double in [2^63, 2^64): GCC lowers it to x86 `cvttsd2si` (SIGNED
  convert), which returns the 0x8000000000000000 sentinel for ANY out-of-range
  input, so `fcvtzu_x_d(1e19)` returned 9223372036854775808 instead of
  10000000000000000000. The scalar 64-bit unsigned band needs the
  subtract-2^63-then-add-back trick (`(uint64_t)(v − 2^63) + 0x8000000000000000ULL`)
  exactly like the JIT; the 32-bit unsigned band needs the same trick at 2^31
  (`fp_to_unsigned_sat` returns the low-32 value for `is_64bit=false` — a
  plain `static_cast<uint32_t>(f)` for f ≥ 2^31 hits cvttss2si's 0x80000000
  sentinel too). Do NOT revert these sites to raw casts. Verified:
  `ctest/jit_int_fp_conv.elf` ALL PASS under JIT and `--no-jit`, full suite
  205/205 in BOTH modes (interp previously 204/205 with the fcvtzu fail).
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
  **DISPATCH BUCKET ATTRIBUTION (2026-08-19):** the SIGPROF "dispatch"
  bucket is NOT the block-cache/dispatch machinery — it counts any sample
  whose RIP lands inside a run_block stack frame, so it INCLUDES all host
  work done mid-block: the thunk path (jit_thunk_svc → GraphicThunk::
  dispatch → host GL/SDL) and the mmap/munmap churn (Memory::mmap_alloc's
  reused-window `memset`, ~1.2 GB/s ≈ 4% of a core on the minecraft game —
  NOT a 24% hotspot; a per-call byte counter refuted the earlier crude
  caller-probe attribution). "Dispatch" samples resolving to libc memcpy/
  syscall/sem_trywait are host Mesa/GL driver work running inside the
  run_block frame, not emulator overhead. The emulator core is fast: chunk
  loads are <20ms and render FPS spikes are host frame pacing. Measure
  these with a direct byte counter (g_memset-style), never a stack-scan
  caller probe. The thunk name-hash cost WAS real and is fixed: a string-
  keyed `unordered_set` lookup in GLStateTracker::tracks_state ran on
  EVERY thunk call (~1.5M/s) until `tracks_state` was precomputed as a
  bool on SymbolEntry at registration (thunk_common.hpp + the three
  dispatch sites in thunk.cpp) — dispatch samples in the string hashtable
  went 31/389 → 0.
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
  `jit_translate.cpp` right after the prologue's `mov rbx,rdi`/emu-slot
  store (BEFORE the R10 window load and vec prologue loads). Chain slots
  patch to
  `chain_entry` instead of `fn`, so a chain edge skips the predecessor's
  frame teardown AND the successor's push/frame/reg-setup (~17 instrs/edge).
  The chain ROOT allocates ONE unified 32 KB frame
  (`kChainSkipFrameBytes = 0x8000`, the vreg-space ceiling: 4095 vregs × 8 B)
  that every block in the chain reuses; successors enter at `chain_entry`
  without allocating. The epilogue becomes a lease: [state flush: flags +
  flush_all_vregs + vec writeback + store PC + mov rdi/rbx rsi=<emu slot>]
  [5-NOP
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
  garbage (the second bug: RIP=0x555500000008). (3) RBX needs no setup on
  the chain edge (reserved reg, never reassigned in a body); R14 IS
  allocatable (10th alloc reg, 1.5.4-alpha) so emu does NOT flow through a
  register — the predecessor's epilogue loads RSI from the shared chain
  frame's FIXED emu slot (emu_slot_off() = -(kChainSkipFrameBytes-8),
  below every block's per-block vreg/lazy-slot ceiling of -32512), and a
  successor that skips its own prologue reads that same slot (the value is
  the constant Emulator*). In non-chain-skip mode the emu slot is per-block
  at -8*(num_stack_slots_+1), inside the +64-byte frame cushion. Do NOT
  change the emu slot to a per-block offset under chain-skip — different
  blocks in a chain have different vreg slot layouts that overlap another
  block's per-block emu offset. (4) vec blocks
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
- **Thunk return-value polarity (audio, 1.5.5-alpha)**: the shared SVC
  chain treats `return 0` as "handled, do NOT write x0". AudioThunk arms
  instead return the REAL guest x0 (0 is often success), so BOTH call
  sites' AUDIO branch must write x0 whenever dispatch != -ENOENT. Any new
  AudioThunk arm must follow this; do not reintroduce an `r==0` fast path.
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
- **Flag-helper scratch contract (2026-08-21, csel-cmov-repair):**
  `emit_load_flags_from_pstate` (x86_backend.cpp) clobbers ONLY
  RAX/RCX/RDX (+RFLAGS) — the C^from_sub extraction is computed in RDX
  alone via `X = pstate ^ (pstate << 2)` then `>>29 &1` (a LEFT shift:
  `(pstate << 2)`'s bit 29 is pstate's bit 27 = from_sub; a right shift
  would XOR C with N instead and silently corrupt every carry-reading
  condition — CS/CC/HI/LS — after a pstate load; PL/LE/GT-style tests
  still pass, so only carry-condition tests catch it). Do NOT add another
  scratch register to this helper or to `emit_normalize_cf_to_sub_
  convention` / `emit_materialize_flags`: every other allocatable reg
  (R8/R9/R11/R12+) may hold a live vreg at the call site. The historical
  bug: the loader used R8 for the from_sub extraction while the CSEL/
  CCMP/BRCOND_SKIP/ADCS-SBCS callers flush ONLY FLAGS3 (BRCOND, tier2
  BRCOND, and FP_CSEL knew and over-flushed R8). Because FCMP
  materializes NZCV to pstate (`flags_in_host_=false`) instead of
  leaving x86 flags in host, every conditional select after an FCMP ran
  the loader — the cmov-CSEL rewrite stages its ELSE value in a fresh
  allocatable reg, which landed in R8 and was silently destroyed, making
  every `cset` after an `fcmp` return 0 (minecraft's `sign()` → DDA
  `step=(0,0,0)` → `_ivec3s2dir` assert at tick ~15 + ground clipping;
  the integer-producer fuzz never saw it because SUBS/ADDS/ANDS keep
  flags in host and skip the loader entirely). Regression coverage:
  the FCMP-producer section in `ctest/jit_csel.c` (sign interleave +
  cs/cc/hi after fcmp + csinc/csinv/csneg after f64 fcmp).
- **CSEL family native CMOVcc lowering (csel-cmov-repair branch,
  2026-08-21)**: CSEL/CSINC/CSINV/CSNEG lower to `cmovcc` — flags ensured
  via the targeted FLAGS3 flush when loading from pstate (register-
  residency preserving; the pre-cmov emitter's flush_all_vregs nuked the
  cache around every select), operands via `ensure_two_vregs`, fresh dest
  via `alloc_reg_excluding`, else-value into dest, single `cmovcc d, s1`.
  The IR translator DECOMPOSES CSINC/CSINV/CSNEG into plain
  `IROp::CSEL` with a pre-computed transform (ADD/NOT/NEG of Rm), so the
  emitter's internal CSINC/CSINV/CSNEG branches are dead at the IR level;
  XZR operands arrive as `load_imm(b,0)` vregs (vreg 32 never reaches the
  emitter — the `z1/z2 == 32` checks are dead code). Direct-carry HI/LS
  (flags from ADD/TST still in host) round-trips via pstate to normalize
  CF (270bb8a). Measured ~+10.7% tier2 CoreMark cumulative with the
  indexed-window-addressing reapply. Merge gate (60s+ clean game) MET
  after the R8 fix: 75s run, zero asserts, world tick 4488.

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
  Transient map/write/unmap-per-frame works fully.
  **PCWFC (Persistent-Coherent Writeback For Coherence, 2026-08-15):**
  persistent+coherent mappings (`GL_MAP_PERSISTENT_BIT` 0x40 +
  `GL_MAP_COHERENT_BIT` 0x80 + `GL_MAP_WRITE_BIT`) are now SUPPORTED — the
  "serious engine" streaming pattern (map once, write every frame through
  the returned pointer, never unmap). `UNMAP_BUFFER` keeps persistent
  mappings alive (GL_ARB_buffer_storage: a persistent mapping stays valid
  after glUnmapBuffer) instead of erasing the bounce; `dispatch()` calls
  `sync_persistent_mappings_()` right before every buffer-consuming call
  (glDraw* family, glCopyBufferSubData, glGetBufferSubData, glTexBuffer/
  glTexBufferRange) to push all live persistent bounces back to the host —
  the practical coherence guarantee (host sees writes at the moment the GPU
  would read them). This is the ONLY release point for persistent bounces:
  `glDeleteBuffers` frees any still-live mappings. Do NOT try to sync on
  map/unmap alone — the whole point is the guest never unmaps. Buffer
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
  `ctest_real/test_sdl_gl_mapbuffer.c` (21 checks, "ALL PASS" pattern,
  exit 77 = skip without GL/SDL/display). Still UNSUPPORTED:
  `glDebugMessageCallback`'s callback is a GUEST function pointer that must
  NOT be handed to the host setter (mirror the `*_CB`/error-callback
  interception pattern) — do not add it as a plain passthrough row.
- **Modern GL 3.3+/4.x "AAA future-proofing" rows (2026-08-15)**: uniform
  blocks (`glGetUniformBlockIndex` `ip`, `glUniformBlockBinding` `iii`,
  `glGetActiveUniformBlockiv` `iiip`, `glGetActiveUniformBlockName`
  `iiipp`), shader introspection (`glGetActiveUniform`/`glGetActiveAttrib`
  `iiipppp`), instancing divisor (`glVertexAttribDivisor` `ii`), the
  modern draw/batch entry points (`glDrawElementsBaseVertex` `iiiii`
  `EL_PTR` arg3, `glDrawRangeElements` `iiiiii` generic — indices at arg5,
  outside EL_PTR's arg3-only scope, `glPrimitiveRestartIndex` `i`),
  query objects (`glGenQueries`/`glDeleteQueries` `ip`, `glIsQuery` `i`,
  `glBeginQuery`/`glEndQuery`, `glGetQueryiv`/`glGetQueryObjectiv`/
  `glGetQueryObjectuiv` `iip`), sampler objects (`glGenSamplers`/
  `glDeleteSamplers` `ip`, `glBindSampler` `ii`, `glSamplerParameteri`
  `iii`, `glSamplerParameterf` `iif` mixed-FP, `glSamplerParameteriv`/
  `glSamplerParameterfv` `iip`), compute (`glDispatchCompute` `iii`,
  `glMemoryBarrier` `i`, `glBindImageTexture` `iiiiiii`), and transform
  feedback (`glBeginTransformFeedback` `i`, `glEndTransformFeedback` `-`,
  `glTransformFeedbackVaryings`). `glTransformFeedbackVaryings` uses the
  NEW `TF_VARYINGS` policy: its `varyings` arg is a NESTED array of
  C-string pointers (like glShaderSource's strings, but with a `count`
  not a lengths array — args are `iipi`), so the dispatch arm
  (`THUNK_TF_VARYINGS` flag, set in register_known_symbols_ like
  `THUNK_SHADER_SOURCE`) reads `count` pointers from the translated
  args[2] guest array, bounces each string, and passes `args[3]` as the
  `bufferMode` GLenum verbatim. ARGS-count pitfall: count the POINTER args
  too — glGetActiveUniform/Attrib take 7 args (4 pointers) and
  glGetActiveUniformBlockName takes 5 (2 pointers); undercounting them
  (`iiippp`/`iiip`) left the trailing `name` pointer untranslated. New
  guest test: `ctest_real/test_sdl_gl_modern.c` (17 checks, "ALL PASS"
  pattern, exit 77 = skip without GL >= 3.3/SDL/display). Host-specific
  note: this sdl2-compat host errors (INVALID_ENUM 1280) on
  `glSamplerParameteri(GL_TEXTURE_MIN_FILTER, GL_NEAREST)` but accepts
  `GL_TEXTURE_MAG_FILTER`/`GL_TEXTURE_WRAP_T` — host and guest agree, so
  it's not a thunk bug; the test uses the accepted params.
  Still UNSUPPORTED: `glDebugMessageCallback`'s callback is a GUEST
  function pointer that must NOT be handed to the host setter (mirror the
  `*_CB`/error-callback interception pattern) — do not add it as a plain
  passthrough row.
- **DisplayThunk (Vulkan/Wayland/X11/XCB/GBM/GLX/RandR/Xkb) is
  table-driven too (1.5.3-alpha):** the SAME `tools/opgen/thunk_dp.txt` →
  `opgen_thunk.hpp` pipeline now carries the ~275 display symbols
  (families `VK`/`WL`/`WL_EGL`/`X11`/`X11XCB`/`XCB`/`GBM`/`XEXT`/`GLX`/
  `RANDR`/`XKB`; policies `PROXY`/`VULKAN`/`VK_GET_PROC`/
  `VK_CREATE_INSTANCE`/`VK_CREATE_DEVICE`/`VK_PRESENT`/`VK_SUBMIT`/
  `VK_CREATE_RENDERPASS`/`VK_CREATE_FRAMEBUFFER`/`VK_BEGIN_RENDERPASS`;
  size kinds
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
  `VK_CREATE_DEVICE`, `VK_PRESENT`, `VK_SUBMIT`, `VK_CREATE_RENDERPASS`,
  `VK_CREATE_FRAMEBUFFER`, `VK_BEGIN_RENDERPASS`) and sizes bounces by `SizeKind`
  (thunk.cpp's `SizeKind` switch gains a `default:` so new kinds are safe).
  The four command/submit policies (added 2026-08-19 for real-frame
  rendering) deep-marshal structs that NEST guest pointers, which the
  generic bounce can't translate: `VK_SUBMIT` (vkQueueSubmit's
  pWaitSemaphores/pWaitDstStageMask/pCommandBuffers/pSignalSemaphores
  arrays), `VK_CREATE_RENDERPASS` (the whole VkRenderPassCreateInfo tree,
  recursing into per-subpass attachment/reference/preserve arrays),
  `VK_CREATE_FRAMEBUFFER` (image-view handle array), and
  `VK_BEGIN_RENDERPASS` (clear-value array). All four arms zero `pNext`,
  cap counts (≤16 submits / ≤32 subpasses/attachments) inside the
  `VkStage` bounce, write back OUT handles at arg 3, and are driven by the
  frozen-layout structs (`VkSubmitInfoH`/`VkRenderPassCreateInfoH`/
  `VkSubpassDescriptionH`/`VkAttachmentDescriptionH`/`VkAttachmentReferenceH`/
  `VkSubpassDependencyH`/`VkFramebufferCreateInfoH`/`VkRenderPassBeginInfoH`)
  placed right after `VkPresentInfoH` — verified byte-for-byte against the
  vendored vulkan_core.h on natural AArch64 alignment. Flat-struct `vkCmd*`
  rows (barriers, clears, viewport/scissor, binds, draws, copies) need NO
  deep marshal — only structs containing pointer members do. Regression:
  `ctest_real/test_vulkan_swapchain.elf` records + submits a clear-color
  frame (render pass → image view → framebuffer → begin → end → submit →
  present) and passes under JIT and `--no-jit`.
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
- **Vulkan graphics-pipeline stage (2026-08-21)**: seven more deep-marshal
  policies — `VK_CREATE_SHADER_MODULE` (nested pCode, staging vector
  reserved for codeSize BEFORE any pointer is taken — a later resize
  reallocates and dangles every pointer VkStage handed out), 
  `VK_CREATE_GRAPHICS_PIPELINES` (the full state tree: stages with pName
  strings + specialization blobs, vertex-input flat arrays, viewport/
  scissor, sample mask, blend attachments, dynamic states; flat-after-
  pNext states copied verbatim; reserve ~512 KiB up front), 
  `VK_CREATE_PIPELINE_LAYOUT`, `VK_CREATE_DESCRIPTOR_POOL`,
  `VK_CREATE_DESCRIPTOR_SET_LAYOUT` (+ pImmutableSamplers),
  `VK_ALLOC_DESCRIPTOR_SETS` (OUT set-handle array), and
  `VK_UPDATE_DESCRIPTOR_SETS`. DescriptorType classification in the
  update arm: image infos = types 1/2/3/10, texel buffer views = 4/5,
  buffer infos = 6/7/8/9 — do NOT use a 1..6 range for images (6 is
  UNIFORM_BUFFER; the misroute nulled the info pointers and segfaulted
  RADV on the first UBO write). ALL H-struct layouts were verified
  byte-for-byte against the vendored vulkan_core.h via a static_assert
  checker (offsetof+sizeof per field) before being added — keep that
  discipline for any new struct. Two latent table bugs fixed:
  `vkCmdUpdateBuffer` and `vkCmdCopyBuffer` had SIX arg tokens for
  5-parameter functions — the extra 'i' shifted the pointer mask so
  pData was passed as a raw guest pointer to the host (segfault at the
  guest .rodata address inside libc memcpy). `test_vulkan_swapchain.elf`
  now draws a real triangle: glslc-built SPIR-V embedded via
  `ctest_real/test_vulkan_spv.h`, vertex+UBO buffers uploaded with
  vkCmdUpdateBuffer (avoids vkMapMemory — its host-pointer bounce is the
  NEXT milestone, mirror the GL MAP_BUFFER/PCWFC pattern), descriptor
  set UBO, D32 depth attachment, per-image framebuffers + command
  buffers, 3-frame draw loop. Passes under JIT and --no-jit on RADV.
  `make cross` now takes `CROSS_EXTRA=` for extra include paths.
- **vkMapMemory guest-window bounce (2026-08-21, the Vulkan PCWFC)**:
  `VK_MAP_MEMORY` maps on the host with a scratch pointer, allocates a
  bounce via `Memory::mmap_alloc` INSIDE the direct window, seeds it
  from the host mapping, and writes the BOUNCE's guest address into
  ppData — the guest derefs it at full JIT speed (the host mapping
  address is 48-bit host heap, useless in the guest). Bookkeeping in
  `DisplayThunkImpl::vk_maps_` (handle → host_ptr/bounce/offset/size)
  + `vk_allocs_` (handle → allocationSize, recorded by the
  `VK_ALLOC_MEMORY` arm so VK_WHOLE_SIZE maps resolve). Coherence —
  the practical HOST_COHERENT guarantee for BOTH coherent and
  non-coherent memory (over-pushing non-coherent is harmless):
  **push** all bounces → host mappings before vkQueueSubmit and
  vkQueuePresentKHR; **pull** host → bounces after successful
  vkDeviceWaitIdle/vkQueueWaitIdle/vkWaitForFences (GPU readback);
  `VK_FLUSH_MAPPED` moves explicit ranges bounce→host before the host
  flush; `VK_INVALIDATE_MAPPED` host-invalidates then copies host→
  bounce; `VK_UNMAP_MEMORY` pushes back, `untrack_allocation`s the
  window range (page-rounded), then host-unmaps; `VK_FREE_MEMORY`
  defensively releases a still-mapped bounce before freeing (the
  glDeleteBuffers pattern). Remapping an already-mapped handle returns
  the existing bounce. Sanity caps: 1 GiB per map, 64 ranges per
  flush/invalidate, 64 fences per wait. The test asserts the mapped
  pointer is nonzero and < 4 GiB, writes the UBO through it directly,
  flushes explicitly, and renders 3 frames (76 checks, JIT + interp).

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

## Verification

- `make` (plain make auto-enables GL/SDL2/EGL thunking)
- `make check-all` — the "everything" target: build + `setup-tests` +
  `setup-rootfs.sh` + `./scripts/run_tests.sh` (default suite = **211 pass /
  0 fail / 0 skip**: unit + integration + toybox + real-world +
  benchmarks + dynamic + interactive). The only historical skip was
  `test_dladdr_glibc`, which must be a glibc-DYNAMIC binary or its dlopen
  stub skips with exit 77.
- `./scripts/run_tests.sh` — the default is the FULL suite
  (interactive + real-world are the standard default) = **211 pass /
  0 fail / 0 skip**. Subsets: `--quick` (no benches, 205),
  `--unit`, `--jit`, `--interp`, `--dynamic`, `--no-rootfs`. Exit 0 =
  all pass, 77 = env-dependent skip (treated as pass).
- `./bifrost-emu ctest/jit_mvni_softfloat.elf`
- `./bifrost-emu ctest/jit_neon_permute.elf`
- `./scripts/run_tests.sh --dynamic` (includes `test_dlopen`)
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`
  (exit 0 = pass, 77 = skip when SDL/GL/display unavailable)

### Test binary toolchains (how `make setup-tests` builds them)

The suite has **208 tests** across categories (unit/JIT/interp, syscalls,
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
