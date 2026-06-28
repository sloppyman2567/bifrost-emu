// bifrost/version.hpp — version constants for bifrost-emu.
//
// Public header: safe to include from any consumer (C++ or C-binding).
#pragma once

namespace arm64emu {

// ── Version ────────────────────────────────────────────────────────────
// 1.4.0-rc.1 (2026-06-27): Release candidate — production hardening.
//   - 41/41 JIT tests pass (also pass under interpreter).
//   - JIT is the default execution mode (6.4x speedup on compute).
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
//     FMADD/FMSUB/FNMADD/FNMSUB (vfmadd231ss/sd, vfnmadd231ss/sd,
//     vfnmsub231ss/sd) on FMA3-capable hosts; decomposed mul+add/sub
//     fallback otherwise (BIFROST_NO_FMA3=1 forces the decomposed path).
//   - fork() + execve() support for running external commands.
//   - toybox sh works: builtins, scripting, variables, arithmetic, if/for/
//     while/case, functions, exit codes, interactive mode.
//   - --jit-threshold flag for hybrid interp/JIT mode on I/O-bound workloads.
//   - 40+ toybox commands verified working (echo, sort, wc, seq, factor,
//     sha256sum, md5sum, sha1sum, base64, cut, cmp, cat, ls, stat, date, etc.).
//   - ASan+UBSan clean on all 41 tests.
//   - rc.1 final: FMV/FMA3 codegen, verify-mode self-loop un-patch fix +
//     verify-once optimization, FNMADD/FNMSUB silent-NOP fix, FP 2-source
//     vs FMA encoding collision fix, is_double=(width!=0) latent bug fix,
//     FNMSUB movq REX.W fix, NEON/SIMD shift+REV+INS+USRA+SLI/SRI fixes,
//     syscall number conflict fixes (36/37/39/41/42/69/88/115/206),
//     fork+exec JIT crash fix, utimensat guest-pointer security fix,
//     ret_errno() macro sweep, vreg bounds checks, W^X depth leak fix,
//     optimizer/SSE silent-fallthrough fixes, NUM_HOST_REGS constant,
//     documentation refresh.
//   - rc.1 MD5 fix: FCVTZS/FCVTZU/SCVTF/UCVTF fixed-point variants were
//     silently NOP'd (integer-variant mask required bit 21 = 1; fixed-point
//     variant has bit 21 = 0 with a 6-bit scale field). Toybox MD5 K-table
//     init uses `fcvtzu w1, d0, #32` to compute floor(|sin(i+1)| * 2^32);
//     the NOP left K[i] filled with stack garbage. Fixed by adding native
//     interpreter handlers with saturating semantics; IR translator routes
//     to CALL_INTERP.
constexpr const char* VERSION  = "1.4.0-rc.1";
constexpr const char* CODENAME = "bifrost-emu";

} // namespace arm64emu
