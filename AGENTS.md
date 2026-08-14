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
  `BIFROST_XTRACE` / `BIFROST_FUTEX_BT` / `BIFROST_PPOLL_PEEK` / etc. still
  override. Add new diagnostic switches there, NOT as ad-hoc `getenv()`
  checks in hot syscall/interp paths.

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
  ops get dragged down to interpreter speed. Diagnose with `BIFROST_PROF=1`
  (SIGPROF sampler: jit/dispatch/translate/interp/other bucket histogram
  printed at exit; installs lazily on first run_block so
  install_host_signal_handlers doesn't overwrite it — a naive install
  crashes the game because SIGPROF is guest-forwarded) and
  `BIFROST_CLASS_PROF=1` (per-class dynamic histogram in the interpreter;
  with JIT ON it shows exactly which instruction classes run through the
  interp fallback). `BIFROST_STATS_PERIOD=N` prints rolling MIPS every N
  seconds past startup/world-gen phases.
- Block dispatch has THREE layers: a single-entry last-block cache, an
  inlined 256-slot direct-mapped inline cache (hash
  `((pc >> 2) ^ (pc >> 17)) & 255`), then the shared-mutex + unordered_map
  slow path. The fast paths are trimmed to a bare call/ret — the global
  safety-valve watchdog is a THREAD-LOCAL counter checked against
  `GLOBAL_BLOCK_LIMIT` (1e12), incremented on every dispatch with NO
  atomic (`lock xadd` was ~15-25 cycles per transition at 20M
  dispatches/sec). Do NOT re-add a per-dispatch atomic, a per-PC watchdog,
  or a `cpu.pc = next_pc` store to the fast paths.
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
- The prologue's 10-byte `movabs r10, window_base` (WIN_REG) is emitted
  LAZILY: only for blocks containing LOAD_MEM/STORE_MEM/ATOMIC/
  SIMD_LD16/SIMD_ST16. Don't unconditionally re-emit it — it's ~3-4 cycles
  of setup on every entry for blocks that never touch the direct window.

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
- `make check-all` now runs BOTH generation guards (`opgen-check` +
  `opgen-thunk-check`) before the test suite, so spec drift fails CI.

## Verification

- `make` (plain make auto-enables GL/SDL2/EGL thunking)
- `make check-all` — the "everything" target: build + `setup-tests` +
  `setup-rootfs.sh` + `./scripts/run_tests.sh` (default suite = **198 pass /
  0 fail / 0 skip**: unit + integration + toybox + real-world +
  benchmarks + dynamic + interactive). The only historical skip was
  `test_dladdr_glibc`, which must be a glibc-DYNAMIC binary or its dlopen
  stub skips with exit 77.
- `./scripts/run_tests.sh` — the default is the FULL suite
  (interactive + real-world are the standard default) = **198 pass /
  0 fail / 0 skip**. Subsets: `--quick` (no benches, 193),
  `--unit`, `--jit`, `--interp`, `--dynamic`, `--no-rootfs`. Exit 0 =
  all pass, 77 = env-dependent skip (treated as pass).
- `./bifrost-emu ctest/jit_mvni_softfloat.elf`
- `./bifrost-emu ctest/jit_neon_permute.elf`
- `./scripts/run_tests.sh --dynamic` (includes `test_dlopen`)
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`
  (exit 0 = pass, 77 = skip when SDL/GL/display unavailable)

### Test binary toolchains (how `make setup-tests` builds them)

The suite has **198 tests** across categories (unit/JIT/interp, syscalls,
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
