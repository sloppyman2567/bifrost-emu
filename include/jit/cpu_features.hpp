// cpu_features.hpp — Runtime x86 CPU feature detection for FMV dispatch.
//
// bifrost-emu's JIT emits x86-64 code into a runtime code buffer. The
// available instruction set varies by host CPU:
//
//   baseline     : SSE2 (guaranteed on x86-64)
//   Westmere+    : SSE4.1, SSE4.2, POPCNT, AES-NI            (2010+)
//   Sandy Bridge+: AVX                                       (2011+)
//   Haswell+     : AVX2, BMI1, BMI2, FMA3                    (2013+)
//   Skylake-X+   : AVX512F, AVX512BW, AVX512DQ, ...          (2017+)
//
// The JIT can use Function Multi-Versioning (FMV): generate multiple
// code paths for hot operations (e.g. FMADD) and dispatch to the best
// available at runtime. This is more flexible than `-mfma` (compile-time)
// because the same binary runs on every x86-64 host.
//
// ── Detection mechanism ────────────────────────────────────────────
//
// CPUID is the standard x86 feature-detection instruction. On Linux,
// we can either call __get_cpuid() from <cpuid.h> (GCC/Clang intrinsic
// that wraps the asm), or read /proc/cpuinfo. CPUID is faster (~10ns)
// and works in any context (no filesystem access).
//
// AVX/AVX512 require an extra check beyond CPUID: the OS must enable
// the SIMD unit via XCR0 (CR_OSXSAVE) and signal #XF on invalid use
// via the proper XCR0 bits. Otherwise the AVX instructions will fault
// even though CPUID says they're present.
//
// Reference: Intel SDM Vol 2A, CPUID instruction; Intel SDM Vol 1,
// ch. 14 (Programming with AVX). We follow the standard algorithm
// recommended by Intel.
#pragma once
#include <cstdint>
namespace arm64emu {
// Bit flags for detected x86 features. Picked to be powers of two so
// callers can test `(features & HAS_FMA3)` cheaply.
//
// Ordering: most-impactful first (FMA3 is what we use most; AVX512
// is nice-to-have but rarely worth the code-size cost in the JIT).
struct CpuFeatures {
    uint32_t bits = 0;
    bool sse41        : 1;  // SSE4.1 — paddq, pblendw, roundss/roundsd, pmaxsb, ...
    bool sse42        : 1;  // SSE4.2 — pcmpestri, popcnt on XMM
    bool popcnt       : 1;  // POPCNT instruction (LZCNT lives in ABM/BMI1 separately)
    bool avx          : 1;  // AVX — 256-bit YMM, VEX-encoded SSE
    bool avx2         : 1;  // AVX2 — 256-bit integer SIMD
    bool fma3         : 1;  // FMA3 — vfmadd132/213/231 {ss,sd,ps,pd} (fused mul-add)
    bool bmi1         : 1;  // BMI1 — andn, blsr, blsi, tzcnt
    bool bmi2         : 1;  // BMI2 — bzhi, pdep, pext, mulx, shlx/shrx/sarx
    bool avx512f      : 1;  // AVX-512 Foundation — 512-bit ZMM, mask registers
    bool avx512bw     : 1;  // AVX-512 Byte/Word — vpaddb/zmm, vpcmpb/w
    bool avx512dq     : 1;  // AVX-512 DWord/Double — vpmullq, fp-class
    bool avx512ifma   : 1;  // AVX-512 IFMA — 52-bit integer mul-add (crypto)
    bool lzcnt        : 1;  // LZCNT (ABM) — leading-zero count, distinct from BMI1
    bool aesni        : 1;  // AES-NI — aesenc/aesdec/aesimc/aesmc (Westmere+)
    bool pclmulqdq    : 1;  // PCLMULQDQ — carry-less multiply (Westmere+)
    bool sha          : 1;  // SHA-NI — sha1rnds4/sha256rnds2 (Goldmont+)
    // True iff the JIT should emit FMA3 codegen for FMADD/FMSUB/
    // FNMADD/FNMSUB. Requires both FMA3 and AVX support (FMA3 ops use
    // VEX encoding; AVX2 is not strictly required but always present
    // on shipping FMA3 CPUs).
    bool has_fma3() const { return fma3 && avx; }
    // True iff the JIT should emit AVX2 (256-bit) codegen. We require
    // AVX + AVX2 + OS support; AVX-512 is even more expensive in power
    // and not used by the JIT today.
    bool has_avx2() const { return avx && avx2; }
    // True iff the JIT can use SSE4.1 codegen (roundss/roundsd,
    // pblendw, etc.). This is the most common "modern" baseline.
    bool has_sse41() const { return sse41; }
    // 1.5.3-alpha: AES-NI / PCLMULQDQ / SHA-NI for native crypto codegen.
    bool has_aesni()     const { return aesni; }
    bool has_pclmulqdq() const { return pclmulqdq; }
    bool has_sha()       const { return sha; }
};
// Detect the host CPU's features via CPUID + XGETBV.
//
// This is called once per FrostJIT instance and cached — the result
// doesn't change for the process lifetime.
//
// On any detection failure (e.g., unsupported CPUID leaf), the
// returned CpuFeatures is the SSE2-only baseline. This ensures the
// JIT can still emit baseline SSE2 code on old CPUs.
CpuFeatures detect_cpu_features();
// Human-readable summary, e.g. "sse4.1 sse4.2 avx avx2 fma3 bmi2".
// Used by --verbose startup banner and the JIT stats dump.
const char* cpu_features_string(const CpuFeatures& f);
} // namespace arm64emu
