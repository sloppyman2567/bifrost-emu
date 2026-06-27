# Roadmap

This document tracks the planned development trajectory for bifrost-emu.
For completed work, see [CHANGELOG.md](CHANGELOG.md). For current test
status, see [TESTS.md](TESTS.md).

---

## v1.4.0 (final release — after rc.1 stabilization)

1. **~~Fix the `toybox sh` regression.~~** ✅ FIXED in rc.0.
   Root cause: MOVI (vector immediate) handler in the interpreter
   only matched cmode=0xE. `MOVI V0.4S, #0` (cmode=0) was silently
   ignored, leaving V0 non-zero, corrupting stack data when used
   with `STP Q0, Q0` for zeroing. Fixed by matching all cmode
   values. toybox sh now works: echo, variables, arithmetic, if/for/
   while/case, functions, exit codes, string tests, pwd, interactive
   mode, fork+execve for external AArch64 commands.

2. **Stabilize.** No new features — just bug fixes from the rc.1
   feedback. rc.1 includes production hardening: JIT mmap error
   handling, fork JIT cleanup, FP bounds checks, signal trampoline
   fork safety, W^X failure path hardening, --jit-threshold input
   validation. Once all `ctest_real/` and `toybox` non-sh programs
   pass under both interpreter and JIT, cut the final 1.4.0.

3. **Fix the NEON/SIMD bug** that breaks `strtok`/`strtok_r` in
   some musl code paths. Trace the `strspn` bitset construction
   to pinpoint the exact instruction. Likely a `STR Qn`/`LDR Qn`
   byte-order mismatch or a 128-bit shift/extract high-half
   handling bug.

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

## v1.4.x (feature work)

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

3. **Full game support** — framebuffer/DRM, audio, input. Long-term
   goal: statically-linked ARM64 SDL2 games at playable framerates.
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
