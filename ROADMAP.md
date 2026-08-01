# Roadmap

This document tracks the planned development trajectory for bifrost-emu.
For completed work, see [CHANGELOG.md](CHANGELOG.md). For current test
status, see [TESTS.md](TESTS.md).

---

## Current Focus — SDL2/OpenGL demo readiness (2026-07-25)

### Done

1. **AdvSIMD modified immediate rewrite** — correct MOVI/MVNI/ORR/BIC/MSL
   (incl. MVNI all-ones masks) with soft-float `__muldf3` still green.
   Regression: `ctest/jit_mvni_softfloat.elf`.

2. **SIMD permute / widen** — ZIP/UZP/TRN + SSHLL/USHLL; MOVI vs SHLL
   distinguished via immh bits[22:19]. Regression: `ctest/jit_neon_permute.elf`.

3. **GraphicThunk marshalling** — AAPCS64 stack args (`glTexImage2D`),
   FP args (`glClearColor`/`glVertex3f`), guest string cache, nested
   `glShaderSource`, SDL pointer bounce for high-stack `SDL_Event`,
   trampoline `ret` after `svc`, static-ELF thunk dlopen, reject
   host-arch `.so` as guest code.

4. **Demo** — `ctest_real/test_sdl_gl_triangle.elf` (SDL2 window +
   immediate-mode triangle, 30 frames). Build with
   `make USE_SDL2=1 USE_THUNK_GL=1`.

### Planned

5. **AArch32 (32-bit ARM) support.**
6. ~~**vDSO emulation.**~~ ✅ DONE in v1.5.1-alpha — the vDSO is loaded,
   `AT_SYSINFO_EHDR` is set, and (new) clock calls from inside the vDSO
   take a **direct fast-path** that reads the host clock without going
   through the syscall dispatcher (genuine speedup for tight
   `clock_gettime` / `std::chrono::now()` loops). Verified with
   `BIFROST_SYSCALL_TRACE_ALL` showing 0 clock syscalls in both JIT and
   interpreter modes.
7. **More Vulkan handle-table coverage** (beyond DisplayThunk PoC).
8. **More real-world binary testing.**

### v1.5.1-alpha additions (in-progress)

- **GL state tracker fixed** (was crashing with SIGSEGV + 57 failures;
  now ALL PASS under JIT & interpreter). `ctest_real/test_gl_state.elf`.
- **FABS/FNEG single-precision JIT codegen** — the sign mask was
  double-width (cleared bit 63, not bit 31), so `fabsf()` of a negative
  float left it negative. Regression: `ctest_real/test_fabs2.elf`.
- **FABD (Floating-point Absolute Difference) implemented** — was
  completely unimplemented; musl's `fabsf(a-b)` lowers to `fabd`, so a
  broken FABD silently returned the first operand, breaking float
  comparisons. Regression: `ctest_real/test_fabd.elf`.
- **SIMD vector FP 2-source ops implemented** (FADD/FSUB/FMUL/FDIV/
  FMAX/FMIN/FABD/FMAXNM/FMINNM/FMULX, `.2s`/`.4s`/`.2d`/`.1d`) — were
  completely unimplemented; NEON-vectorized FP silently produced wrong
  results. Now native SSE codegen in the JIT (no interpreter fallback)
  for single precision; double precision runs through the interpreter
  handler via the JIT fallback. Also fixed the **FMULX vector encoding**
  (was mis-encoded as a different opcode) and added the missing
  **double-precision (`.2d`/`.1d`) interpreter case labels**. Fixed the
  **LD1/ST1 single-vs-multi classification** in the decoder (bit[24]
  discriminates the 0x0C/0x0D bases; the old bit[12] heuristic
  misdecoded `LD1 {V0.4S}` as a single-element load, dropping the high
  64 bits) and corrected the multi-structure register count (now from
   the opcode field bits[15:12], not bits[14:13] which is the size).
   Regression: `ctest_real/test_simd_vec_fp.elf`.
- **Unhandled SIMD is now a loud failure instead of a silent NOP.** The
  interpreter's `SIMD_DP` catch-all used to silently skip any op it
  didn't model ("incorrect but lets glibc continue", wrong results for
  anything that depended on the op). It now logs under
  `BIFROST_SIMD_TRACE=1` and throws a `DecodeError` (→ SIGILL), so a real
  game/libc run surfaces exactly which NEON ops are still missing. The
  audit found two ops actually used by shipped code — both implemented:
  **SADDW/SADDW2** (sign-extended widening add, `v.4s`/`v.2d` forms,
  low/high half via Q) and **UMINP** (pairwise unsigned min, `8B/16B`,
  `4H/8H`, `2S/4S`). Regression: `ctest_real/test_simd_saddw_uminp.elf`.
  `rw_busybox_df` and `rw_iperf3_version` pass under the strict mode.
- **dladdr() enabled** — was deliberately disabled; now overridden and
  works through the real glibc `dladdr@GLIBC_2.34` symbol.
  Regressions: `ctest_real/test_dladdr.elf`, `test_dladdr_glibc.elf`.
- **vDSO emulation** — `AT_SYSINFO_EHDR` was 0 (no vDSO); now an
  embedded AArch64 vDSO ELF (`tools/vdso/`) is loaded into guest
  memory at startup. The vDSO provides `gettimeofday@LINUX_2.6`,
  `clock_gettime@LINUX_2.6.39`, `clock_getres@LINUX_2.6.39`,
  `__kernel_rt_sigreturn@LINUX_2.6.39` stubs that trap to the
  emulator's syscall handler. **Honest status:** this is a
  compatibility/correctness win (glibc takes the same vDSO code path
  it does on a real kernel, and `__kernel_rt_sigreturn` is available
  for the canonical signal trampoline), NOT a perf win today — the
  stubs route to the same syscall handler the direct-syscall fallback
  uses, so there's no measurable speedup yet. The fast-path
  optimization (read host clock directly, skip syscall dispatch) is
  future work and is what this vDSO enables.

---

## v1.5.0.alpha — SHIPPED (2026-07-03)

**1.5.0.alpha** is the first feature release after the 1.4.0 stable.
It adds native SSE2 codegen for SIMD vector shifts (SHL/USHR/SSHR),
fixes a missing SSHR-by-immediate handler in the interpreter, and keeps
all 72 tests passing under JIT, interpreter, and FWD mode.

### Completed in 1.5.0.alpha

1. **Native SIMD vector shift codegen.** SHL/USHR/SSHR (vector, by
   immediate) now emit native SSE2 `psllw/pslld/psllq`,
   `psrlw/psrld/psrlq`, and `psraw/psrad` respectively. Previously
   these fell back to `CALL_INTERP` (~20% overhead on SIMD-heavy
   workloads). 8-bit element shifts fall back (no `psllb` in SSE2);
   64-bit SSHR falls back (needs AVX-512 `psraq`).

2. **Missing SSHR-by-immediate interpreter handler.** The vector
   SSHR-by-immediate instruction (encoding `0x0F000400` with immh!=0)
   was silently NOP'd in the interpreter — the dispatcher's MOVI check
   at the same encoding only fires for immh==0, and the fall-through had
   no SSHR handler. This broke `sshr v0.8h, v0.8h, #2` etc. in both
   interpreter and JIT (JIT falls back to the interpreter via
   `CALL_INTERP`). Now implemented as a proper arithmetic-shift-right
   per-lane handler.

3. **New IR ops: `SIMD_SHL`, `SIMD_USHR`, `SIMD_SSHR`** with executor
   support for verify-mode comparison.

4. **New test: `ctest/jit_neon_advanced.elf`** — 11 checks covering
   SHL/USHR/SSHR for 16/32/64-bit elements, shift-by-zero,
   shift-by-max, and a combined shift+add pattern. All 11 pass under
   both JIT and interpreter (was 9/11 before the SSHR fix).

5. **Version consistency sweep.** All version references in
   `version.hpp`, `main.cpp`, `api/bifrost.h`, `Makefile`, `README.md`,
   `TESTS.md`, `ROADMAP.md`, `src/graphics/graphics.cpp`, and
   `ctest/test_capi.c` now say `1.5.0.alpha`. Previously many still
   said `1.4.0`, causing `test_capi` to fail its version check.

---

## v1.4.0 — SHIPPED (2026-07-03)

**1.4.0 stable release.** All 72 tests pass under JIT, interpreter, and
FWD mode. C API (22/22 checks) implemented and verified. See
[CHANGELOG.md](CHANGELOG.md) for the full release history.

### Completed in 1.4.0

1. **~~Fix the `toybox sh` regression.~~** ✅ FIXED in rc.0.
   Root cause: MOVI (vector immediate) handler in the interpreter
   only matched cmode=0xE. `MOVI V0.4S, #0` (cmode=0) was silently
   ignored, leaving V0 non-zero, corrupting stack data when used
   with `STP Q0, Q0` for zeroing. Fixed by matching all cmode
   values. toybox sh now works: echo, variables, arithmetic, if/for/
   while/case, functions, exit codes, string tests, pwd, interactive
   mode, fork+execve for external AArch64 commands.

2. **~~Stabilize.~~** ✅ DONE — 1.4.0 shipped (2026-07-03). All 72 tests
   pass under JIT, interpreter, and FWD mode. Production hardening
   includes JIT mmap error handling, fork JIT cleanup, FP bounds checks,
   signal trampoline fork safety, W^X failure path hardening,
   --jit-threshold input validation, Function Multi-Versioning (FMV) +
   native FMA3 codegen, verify-mode self-loop un-patch fix + verify-once
   optimization, FNMADD/FNMSUB silent-NOP fix, FP 2-source vs FMA
   encoding collision fix, FCMPE #0.0 misdecode fix, CCMP scratch vreg
   spill fix, native IR ops for fixed-point FCVTZS/SCVTF variants, and
   the FWD LSE atomic fix (block-level FWD disable for atomic blocks).

3. **~~Fix the NEON/SIMD bug~~** that breaks `strtok`/`strtok_r` —
   ✅ RESOLVED via the rc.1 NEON/SIMD overhaul (10 bugs fixed).

4. **~~Complete signal delivery.~~** ✅ DONE in rc.0 — proper
   `siginfo_t`/`ucontext_t`, `SA_RESTART`, signal masks,
   `sigaltstack`, and cross-thread delivery are all implemented.
   See CHANGELOG.md for details.

5. **Fix `strtod("-nan")` sign-bit loss.** NOT AN EMULATOR BUG —
   this is a musl bug. In musl's `src/internal/floatscan.c`, the
   `__floatscan` function returns `NAN` (line 472) without applying
   the sign, unlike `sign * INFINITY` (line 465) for infinity. The
   sign is parsed correctly (`sign -= 2*(c=='-')` at line 454), but
   the NaN return path ignores it. This affects all musl-based
   programs, not just under bifrost-emu. Cannot be fixed in the
   emulator without patching the guest binary.

---

## v1.4.x (feature work — most items shipped in 1.4.0 or 1.5.0.alpha)

1. **SDL2 audio + input** on top of the v1.4.0-alpha SDL2 video
   backend. Build with `make USE_SDL2=1` to enable the window backend;
   audio output currently goes through OSS `/dev/dsp` passthrough.

2. **Sub-decode the SIMD DP and FP scalar catch-all groups.**
   Currently these are routed as generic `SIMD_DP` / `FP_SCALAR` and
   re-dispatched in the interpreter. The hierarchical decoder
   structure makes adding dedicated `InstClass` values for each a
   clean refactor — and would make the NEON bug above easier to
   isolate.

3. **~~Real fork support~~** ✅ DONE in rc.0 — fork() via host fork()
   with CoW memory + execve() for running external AArch64 commands.
   Child disables JIT, inherits CoW copy. Parent's wait4() works.

4. **JIT I/O performance.** The JIT is ~9% slower than the
   interpreter for I/O-bound workloads (seq 1 10000) because the
   block-translation overhead (942 blocks for seq) is not amortized
   when most time is in syscalls. Consider a hybrid mode: interpreter
   for the first N instructions of each block, then switch to JIT
   only for hot blocks.

5. **~~Native FMA3 codegen for FMADD/FMSUB/FNMADD/FNMSUB.~~**
   ✅ DONE in rc.1 — Function Multi-Versioning (FMV) framework added
   (`include/jit/cpu_features.hpp`, `src/jit/cpu_features.cpp`). The
   FrostJIT constructor detects host CPU features via CPUID + XGETBV
   (SSE4.1/SSE4.2/POPCNT/AVX/AVX2/FMA3/BMI1/BMI2/AVX-512). On
   FMA3-capable hosts, FMADD/FMSUB/FNMADD/FNMSUB emit native
   `vfmadd231ss/sd`, `vfnmadd231ss/sd`, `vfnmsub231ss/sd` — both
   correct (single-rounded per IEEE 754) and ~1 cycle faster per
   instruction. Falls back to decomposed mul+add/sub on non-FMA3
   hosts; `BIFROST_NO_FMA3=1` forces the decomposed path for
   debugging. This addresses the FMA correctness (decomposed path is
   double-rounded) and performance (FMA3 single-rounded) items for
   FMA3-capable hosts.
    Future FMV work: BMI2 (pdep/pext for bit-permutation), AVX-512
    (masked operations).

6. **~~NEON/SIMD shift and REV fixes.~~** ✅ DONE in rc.1 — Fixed 10
   NEON bugs: 32-bit ROR wrap-bit loss, vector SHL/USHR/SHRN immh
   extraction (off by one bit), element-size rule, SHL constant,
   MOVI/shift encoding collision, REV64/REV32 mask + size-awareness,
   added USRA/SSRA/SLI/SRI handlers, fixed INS/UMOV v_hi routing for
   Q=1. SHA-1/224/256/384/512 and CRC32 now produce correct hashes.
   MD5 now produces correct hashes (fixed via the FCVTZU fixed-point
   variant fix). Added `ctest/jit_neon.elf` regression test.

7. **~~AVX2 256-bit SIMD codegen.~~** ✅ DONE in v1.5.1-alpha — the FMV
   framework now gates a native AVX2 path for the vector
   shift-by-immediate family (SHL/USHR/SSHR/USRA/SSRA/SLI/SRI). On AVX2
   hosts both 64-bit halves are packed into one YMM and processed with a
   single 256-bit VEX instruction; `BIFROST_NO_AVX2=1` forces the SSE2
   128-bit per-half fallback (mirroring `BIFROST_NO_FMA3`). Also fixed a
   Q=0 `v_hi`-zeroing JIT/interpreter divergence and interpreter UB at
   `shift == esize*8`. Regression: `ctest_real/test_simd_shift.elf`.

---

## v1.5.0.alpha and beyond (future feature work)

Items below this point were NOT in 1.5.0.alpha and are open for future
feature releases.

1. **SDL2 audio + input** on top of the v1.4.0-alpha SDL2 video
   backend. Build with `make USE_SDL2=1` to enable the window backend;
   audio output currently goes through OSS `/dev/dsp` passthrough.

2. **~~VFS bug fixes.~~** ✅ DONE in 1.5.0.alpha (Turn 35, Yggdrasil
   rename). `/dev/random` vs `/dev/urandom` now use distinct pools via
   `getrandom(GRND_RANDOM)` vs `getrandom(0)`. `O_NONBLOCK` on virtual
   fds works via the host-fd passthrough in `fcntl F_SETFL`. `lseek`
   on memfd-backed virtual files works (and triggers lazy regeneration
   on `SEEK_SET 0` for `/proc/self/maps` etc.). procfs `status` field
   truncation was already fixed in Turn 29. The remaining "VFS edge
   cases" item is now `DirNode` support for nested virtual directories
   (e.g. `/proc/self/fd`, `/proc/net/*`) — see "More procfs coverage"
   below.

3. **Better JIT performance.** Two areas: (a) implement true LRU
   eviction in the register allocator (currently FIFO), and (b) use
   the FMV framework to emit AVX2 256-bit SIMD codegen for vector ops
   that currently fall back to the interpreter (USRA/SSRA/SLI/SRI etc.).

4. **More SIMD coverage.** Add a comprehensive `ctest/jit_neon_advanced.elf`
   covering SIMD instructions not in the current `jit_neon.elf`:
   EXT, TBL/TBX, UZP/ZIP/TRN, and the narrowing/widening shifts
   (SHRN/SSHLL/USHLL).

---

## v2.0 (major release)

The v2.0 line will focus on expanding the set of runnable software
beyond musl-static binaries. This is a significant architectural
expansion.

1. **Full dynamic linking support.** Bifrost-emu already has limited
   dynamic linking: it loads the PT_INTERP dynamic linker ELF, maps
   its segments, and uses its entry point (so the linker's own code
   handles DT_NEEDED, relocations, etc. via our syscalls). This works
   for simple dynamically-linked musl binaries. v2.0 will expand this
   to full dynamic linking: proper DT_NEEDED processing, runtime
   relocation application, PLT/GOT resolution, and glibc's dynamic
   linker support. This significantly expands the set of runnable
   software — most real-world ARM64 Linux distributions ship
   dynamically-linked binaries.

2. **glibc support.** Currently only musl-static binaries are
   supported; glibc 2.36+ static binaries hit a decode error on an
   unhandled instruction after mallocng init. v2.0 will add full
   glibc compatibility — both static and dynamic — so binaries from
   Debian/Ubuntu/Fedora ARM64 systems run without modification. This
   requires expanding the syscall surface (glibc uses many more
   syscalls than musl) and handling glibc's initialization sequence.

3. **Full interactive application support** — framebuffer/DRM, audio, input.
   Long-term goal: statically-linked ARM64 SDL2 applications at interactive
   framerates.
   Builds on the v1.4.x SDL2 audio + input work.

4. **ASLR** — binaries currently load at their preferred vaddr;
   randomizing load addresses would catch guest programs that
   accidentally depend on absolute addressing. Required for full
   PIE binary support.

---

## Long-term goals

- **Multi-threaded guest support** — currently `clone()` with
  `CLONE_VM` creates a new vCPU but true SMP semantics (atomic
  memory ordering, futex wakeups across vCPUs) need work.
- **AArch32 (32-bit ARM) support** — bifrost-emu currently only
  handles AArch64. AArch32 would expand compatibility with older
  ARM Linux binaries.
- **Non-Linux guest OS support** — FreeBSD, OpenBSD user-mode
  emulation. The decoder is OS-agnostic; only the syscall layer
  would need a backend swap.
