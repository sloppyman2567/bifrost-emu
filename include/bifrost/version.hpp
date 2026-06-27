// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0-rc.1 (2026-06-27): Release candidate — production hardening.
//   - 39/39 JIT tests pass (also pass under interpreter).
//   - JIT is the default execution mode (6.4x speedup on compute).
//   - Production-ready signal delivery: proper siginfo_t/ucontext_t,
//     rt_sigprocmask, sigaltstack, SA_RESTART/RESETHAND/NODEFER/SIGINFO.
//   - Dynamic linker: DT_NEEDED, TLS relocations, GOT/PLT, symbol resolution.
//   - Native SIMD JIT: ADD/SUB/MUL/CMEQ (vector), BIC/ORN/EON (logical),
//     MOVI (all cmode values), STP/LDP Q (128-bit), LD1/ST1 multi-reg.
//   - fork() + execve() support for running external commands.
//   - toybox sh works: builtins, scripting, variables, arithmetic, if/for/
//     while/case, functions, exit codes, interactive mode.
//   - --jit-threshold flag for hybrid interp/JIT mode on I/O-bound workloads.
//   - 40+ toybox commands verified working (echo, sort, wc, seq, factor,
//     md5sum, sha256sum, base64, cut, cmp, cat, ls, stat, date, etc.).
//   - ASan+UBSan clean on all 39 tests.
//   - rc.1: JIT mmap error handling, fork JIT cleanup, FP bounds checks,
//     signal trampoline fork safety, W^X failure path hardening,
//     --jit-threshold input validation, documentation refresh.
constexpr const char* VERSION  = "1.4.0-rc.1";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
