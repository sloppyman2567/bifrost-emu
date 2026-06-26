# Roadmap

This document tracks the planned development trajectory for bifrost-emu.
For completed work, see [CHANGELOG.md](CHANGELOG.md). For current test
status, see [TESTS.md](TESTS.md).

---

## v1.4.0-rc.0 (next)

1. **Fix the `toybox sh` regression.** `toybox sh -c 'echo hi'`
   currently aborts with `UnmappedMemory` at `0x7473657463006883`.
   The standalone `sh.elf` (a simpler REPL) works fine — the issue
   is specific to toybox's `sh` binary, likely involving stack
   pointer setup or a signal-frame layout assumption. Trace with
   `./bifrost-emu -d ctest_real/toybox sh -c 'echo hi'` to find
   the faulting instruction.

2. **Stabilize.** No new features — just bug fixes from the beta.3
   feedback. Once all `ctest_real/` and `toybox` non-sh programs
   pass under both interpreter and JIT, cut rc.0.

3. **Fix the NEON/SIMD bug** that breaks `strtok`/`strtok_r` in
   some musl code paths. Trace the `strspn` bitset construction
   to pinpoint the exact instruction. Likely a `STR Qn`/`LDR Qn`
   byte-order mismatch or a 128-bit shift/extract high-half
   handling bug.

4. **Complete signal delivery.** Add `siginfo_t`/`ucontext_t`
   contents, `SA_RESTART`, signal masks, `sigaltstack`, and
   cross-thread delivery. The signal frame plumbing and host-to-guest
   forwarding landed in v1.4.0-alpha / v1.4.0-alpha.1; this is the
   remaining work to make it useful for real signal-heavy programs.

5. **Fix `strtod("-nan")` sign-bit loss.** `strtod("-nan")` returns
   `nan` (sign bit dropped). The `-inf`/`+inf`/`infinity` paths all
   work (fixed beta.3); `-nan` is a separate code path in musl's
   `__floatscan` sign propagation.

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

3. **Real fork support** (copy-on-write guest memory) so toybox `sh`
   and other fork-heavy programs work fully. Currently `clone()`
   without `CLONE_VM` returns 0 (vfork semantics), which is enough
   for `sh -c` but not for true multi-process pipelines.

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
