// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0 (2026-07-03): Stable release.
//   - 72/72 tests pass under JIT, interpreter, AND FWD mode (BIFROST_ENABLE_FWD=1).
//   - C API: 22/22 checks pass (ctest/test_capi.c, compiled as pure C).
//   - JIT is the default execution mode (6.4x speedup on compute, 571 MIPS).
//   - 30+ bug fixes across syscall layer (13 wrong syscall numbers, 8 logic
//     bugs), VFS (4 fixes), interpreter (5 fixes), and futex (3 fixes).
//   - FWD-mode LSE atomic fix: load-forwarding disabled for atomic blocks.
//   - C API implemented (api/bifrost_capi.cpp, 300+ lines, 25+ functions).
//   - Hot-path scalability: BlockEntry store_infos → shared_ptr, getenv cached.
//   - Production-ready signal delivery: proper siginfo_t/ucontext_t,
//     rt_sigprocmask, sigaltstack, SA_RESTART/RESETHAND/NODEFER/SIGINFO.
//   - Dynamic linker: DT_NEEDED, TLS relocations, GOT/PLT, symbol resolution.
//   - Native SIMD JIT: ADD/SUB/MUL/CMEQ (vector), BIC/ORN/EON (logical),
//     MOVI (all cmode values), STP/LDP Q (128-bit), LD1/ST1 multi-reg.
//   - NEON/SIMD fixes: 10 bugs fixed (immh extraction, MOVI/shift collision,
//     REV64/REV32 size-aware, USRA/SSRA/SLI/SRI handlers, INS/UMOV v_hi,
//     32-bit ROR). SHA-1/256/384/512 + CRC32 + MD5 now produce correct hashes.
//   - Function Multi-Versioning (FMV): runtime CPUID detection of SSE4.1/
//     AVX/AVX2/FMA3/BMI1/BMI2/AVX-512. Native FMA3 codegen for
//     FMADD/FMSUB/FNMADD/FNMSUB on FMA3-capable hosts; decomposed mul+add/sub
//     fallback otherwise (BIFROST_NO_FMA3=1 forces the decomposed path).
//   - Native LSE atomics: CAS/LDADD/STADD/SWP/STSET/STCLR/LDSET/LDCLR/LDEOR
//     via lock-prefixed x86 instructions (~20x over CALL_INTERP).
//   - Shared-JIT (default): spawned threads share the main's FrostJIT,
//     saving 64 MiB per thread. Sharded exclusive monitor (16 stripes).
//   - fork() + execve() support for running external commands.
//   - toybox sh works: builtins, scripting, variables, arithmetic, if/for/
//     while/case, functions, exit codes, interactive mode.
//   - --jit-threshold flag for hybrid interp/JIT mode on I/O-bound workloads.
//   - 40+ toybox commands verified working (echo, sort, wc, seq, factor,
//     sha256sum, md5sum, sha1sum, base64, cut, cmp, cat, ls, stat, date, etc.).
//   - ASan+UBSan clean on all 72 tests.
//   - ~170 Linux AArch64 syscalls (file I/O, threading, signals, timing,
//     fork+execve, event loops, filesystem operations).
constexpr const char* VERSION  = "1.4.0";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
