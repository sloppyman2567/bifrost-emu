// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.5-alpha (2026-07-04): First feature release after 1.4.0 stable.
//   - Native SIMD vector shift codegen (SHL/USHR/SSHR) via SSE2
//     psllw/pslld/psllq, psrlw/psrld/psrlq, psraw/psrad. Previously
//     these fell back to CALL_INTERP (~20% overhead on SIMD-heavy
//     workloads). 8-bit element shifts still fall back (no psllb in
//     SSE2). 64-bit SSHR falls back (needs AVX-512 psraq).
//   - Fixed missing SSHR-by-immediate handler in the interpreter
//     (vector SSHR was silently NOP'd; now properly arithmetic-shifts
//     per lane).
//   - New IR ops: SIMD_SHL, SIMD_USHR, SIMD_SSHR.
//   - New test: ctest/jit_neon_advanced.elf (SHL/USHR/SSHR 16/32/64-bit).
//     All 11 checks pass under JIT and interpreter (was 9/11 before
//     the SSHR fix).
//   - VFS subsystem renamed to Yggdrasil (the world-tree from Norse
//     cosmology, matching the bifrost + FrostJIT theme). Class hierarchy
//     renamed: VFS→Yggdrasil, VNode→Node, HostVNode→HostNode, etc.
//     File layout split from src/vfs/ (6 files) into src/yggdrasil/
//     (12 files, one per concern).
//   - New DirNode class: ls /proc, ls /dev, ls /proc/self now work
//     (previously returned nothing — no Node existed for the
//     directories themselves).
//   - Lazy regeneration for /proc/self/maps and /proc/self/status:
//     content is re-rendered on each SEEK_SET 0 so re-reads reflect
//     live state.
//   - /dev/random vs /dev/urandom distinction: distinct entropy pools
//     via getrandom(GRND_RANDOM) vs getrandom(0).
//   - Ioctl dispatch moved from the syscall layer (130-line if-else
//     chain) into the Node hierarchy — FbNode owns framebuffer ioctls,
//     HostNode/StdioNode own terminal ioctls.
//   - O_NONBLOCK on virtual fds now works (via host-fd passthrough).
//   - lseek on memfd-backed virtual files works (was ESPIPE in some
//     paths); triggers lazy regeneration on SEEK_SET 0.
//   - Version consistency sweep: all version refs across the tree now
//     say 1.4.5-alpha (previously many said 1.4.0; test_capi was
//     checking for "1.4.0" and failing).
//   - All 1.4.0 features retained: 72/72 tests pass under JIT+interp+FWD.
constexpr const char* VERSION  = "1.4.5-alpha";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
