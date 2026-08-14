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
  esize=1 and 64-bit SSRA/SRSRA via CALL_INTERP). USRA masks to 0x2F001400 —
  do not confuse it with the rounding variants URSRA (0x2F003400) / SRSRA
  (0x0F003400): those add the round-half-up top discarded bit, computed via
  (Vn >> sh) + ((Vn >> (sh-1)) & 1) (isolation: PSRL (sh-1) then PSLL/PSRL
  (esize*8-1) round-trip, no mask constant). URSRA sh==esize*8 → top-bit
  test; SRSRA sh==esize*8 → 0 (sign-fill cancels the round carry).
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
  `setup-rootfs.sh` + `./scripts/run_tests.sh` (default suite = **190 pass /
  0 fail / 0 skip**: unit + integration + toybox + real-world +
  benchmarks + dynamic). The only historical skip was `test_dladdr_glibc`,
  which must be a glibc-DYNAMIC binary or its dlopen stub skips with
  exit 77.
- `./scripts/run_tests.sh --test-all` — default suite + 5 interactive
  stdin tests = **195 pass / 0 fail / 0 skip**, incl. downloaded
  real-world binaries. Subsets: `--quick` (no benches),
  `--unit`, `--jit`, `--interp`, `--dynamic`, `--no-rootfs`. Exit 0 =
  all pass, 77 = env-dependent skip (treated as pass).
- `./bifrost-emu ctest/jit_mvni_softfloat.elf`
- `./bifrost-emu ctest/jit_neon_permute.elf`
- `./scripts/run_tests.sh --dynamic` (includes `test_dlopen`)
- `DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf`
  (exit 0 = pass, 77 = skip when SDL/GL/display unavailable)

### Test binary toolchains (how `make setup-tests` builds them)

The suite has **195 tests** across categories (unit/JIT/interp, syscalls,
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
