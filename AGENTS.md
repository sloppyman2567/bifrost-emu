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
  (0x2E20AC00) sub3_noq groups are covered. TBL/TBX all four forms
  (TBL1 0x0E000000, TBL2 0x0E002000, TBX1 0x0E001000, TBX2 0x0E003000;
  op2=bit12, L=bit13) are in interp — GCC lowers `vextq_u8` to TBL2 +
  `ins v.b[i], v.b[j]` index building. INS (element, vector) shares the
  EXT prefix `(op & 0xBFE00000) == 0x2E000000`; distinguish by bit10
  (INS=1, EXT=0). RBIT/NOT/CNT all collapse to sub2 0x0E205800 — RBIT is
  size=1, NOT is U=1; RBIT bit-reverses per byte. Vector FCVTZS/FCVTZU
  share sub3 with ABS/NEG (0x0E20B800/0x2E20B800) but set bit16 (0x10000);
  FCVTZU clamps negatives to 0. The vector shift-by-immediate
  family (SHL/USHR/SSHR/USRA/SSRA/SLI/SRI) is native in the JIT (AVX2
  VEX 256-bit, `BIFROST_NO_AVX2` disables; SSE2 128-bit fallback; esize=1
  and 64-bit SSRA via CALL_INTERP). USRA masks to 0x2F001400 — do not
  confuse it with the rounding variants URSRA (0x2F003400) / SRSRA
  (0x0F003400), which are still unimplemented.
- Vector FMOV immediate (cmode=0xF in the AdvSIMD modified-immediate block,
  e.g. `fmov v31.2d, #20.0` = 0x6F01F69F) is NOT a NOP: expand via
  AdvSIMDExpandImm. 64-bit (op bit29 set): `(imm8&0x3f)<<48`, sign bit →
  bit63, exponent = `imm8&0x40 ? 0x3FC0000000000000 : 0x4000000000000000`,
  replicated to both 64-bit lanes. 32-bit (op clear): sign→bit31,
  exponent = `imm8&0x40 ? 0x1F000000 : 0x40000000`, mantissa `(imm8&0x3f)<<19`,
  replicated per 32-bit lane. A NOP here corrupts Qt QRectF values built
  with `fmov v.2d,#imm` + `str q` (NaN rects → broken rounded rect).
  JIT falls back to CALL_INTERP for this (not in the simd_dp table).
- FP-FMA semantics: FMADD = c + a*b, FMSUB = c − a*b, FNMADD = −(a*b + c),
  FNMSUB = a*b − c. FNMADD/FNMSUB are NOT −a*b±c aliases — encoding those
  wrong corrupts any value computed via `-(a*b+c)` / `a*b−c` (musl `pow`,
  `rgba_lerp`'s lab conversions). Keep interp, JIT FMA3 map, and IR in sync.
- UBFM/LSR pitfall: `lsr Xd, Xn, #0` (UBFM #0, #(datasize−1)) is a NO-OP —
  a shift by zero returns the source. Do not special-case it to 0; compilers
  emit `lsr w3, x19, #0` to grab the low 32 bits of a 64-bit constant during
  vec3/vec4 struct packing (returning 0 zeroed the z component of colors).
  The general UBFM extract path already yields the correct value.

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

## Verification

- `make USE_SDL2=1 USE_THUNK_GL=1`
- `./scripts/run_tests.sh --unit --quick`
- `./bifrost-emu ctest/jit_mvni_softfloat.elf`
- `./bifrost-emu ctest/jit_neon_permute.elf`
- `./scripts/run_tests.sh --dynamic` (includes `test_dlopen`)
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`

## Child DOX Index

(none — single-tree emulator; parent Downloads rail indexes this folder)
