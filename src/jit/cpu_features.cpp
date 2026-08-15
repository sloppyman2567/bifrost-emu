// cpu_features.cpp — x86 CPU feature detection via CPUID + XGETBV.
//
// Used by the FrostJIT constructor to decide which codegen variants
// (baseline SSE2 vs. AVX2+FMA3) are available. The result is cached
// for the JIT's lifetime.
//
// Algorithm (per Intel SDM Vol 1, ch. 14):
//   1. Check CPUID.1:ECX[OSXSAVE] = 1 → OS supports XGETBV
//   2. If yes, XGETBV(0) → read XCR0 (feature enable bitmap)
//      - XCR0[1] = XMM state enabled (SSE)
//      - XCR0[2] = YMM state enabled (AVX/AVX2)
//      - XCR0[5:7] = OPMASK/ZMM_Hi256/Hi16_ZMM (AVX-512)
//   3. Check CPUID.1:ECX for SSE4.1/SSE4.2/POPCNT/AVX/FMA3 (leaf 1)
//   4. Check CPUID.7:EBX for BMI1/BMI2/AVX2 (leaf 7 subleaf 0)
//   5. Check CPUID.7:EBX/EDX for AVX-512 features (leaf 7 subleaf 0)
//
// We use __get_cpuid() and __cpuid_count() from <cpuid.h> (GCC/Clang
// intrinsic) — portable across g++ and clang++.
//
// NOTE: __cpuid_count() is a macro that expands to inline asm and
// returns void (it does NOT indicate whether the leaf is supported).
// We use __get_cpuid_max() to check leaf availability first.
#include "jit/cpu_features.hpp"
#include <cpuid.h>
#include <cstddef>  // for size_t
namespace arm64emu {
// XCR0 feature bit positions (Intel SDM Vol 1, Table 13-1).
//   bit 0:  x87          (always set on x86-64)
//   bit 1:  XMM          (SSE — required for XMM state)
//   bit 2:  YMM          (AVX — high 128 bits of YMM)
//   bit 5:  OPMASK       (AVX-512 k0..k7)
//   bit 6:  ZMM_Hi256    (AVX-512 high 256 bits of ZMM)
//   bit 7:  Hi16_ZMM     (AVX-512 ZMM16..ZMM31)
static constexpr uint32_t XCR0_XMM       = 1u << 1;
static constexpr uint32_t XCR0_YMM       = 1u << 2;
static constexpr uint32_t XCR0_OPMASK    = 1u << 5;
static constexpr uint32_t XCR0_ZMM_HI256 = 1u << 6;
static constexpr uint32_t XCR0_HI16_ZMM  = 1u << 7;
// Read the XCR (extended control register) at index `idx`.
// XGETBV is the instruction; we wrap it in inline asm because <cpuid.h>
// doesn't expose it. ECX = idx (typically 0 for XCR0). Returns EDX:EAX.
static inline uint64_t xgetbv(uint32_t idx) {
    uint32_t eax, edx;
    __asm__ __volatile__("xgetbv"
                         : "=a"(eax), "=d"(edx)
                         : "c"(idx));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}
CpuFeatures detect_cpu_features() {
    CpuFeatures f{};
    // CPUID leaf 1: SSE4.1/SSE4.2/POPCNT/AVX/FMA3/OSXSAVE.
    // __get_cpuid returns 1 on success, 0 on unsupported leaf.
    // On ancient CPUs (pre-Pentium 4), leaf 1 may not be present.
    // XCR0 is read once and reused for both AVX and AVX-512 checks.
    uint64_t xcr0 = 0;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        // ECX bits (CPUID.01H:ECX):
        //   bit 19: SSE4.1
        //   bit 20: SSE4.2
        //   bit 23: POPCNT
        //   bit 26: XSAVE (XGETBV available)
        //   bit 27: OSXSAVE (OS enabled XSAVE)
        //   bit 28: AVX
        //   bit 29: F16C (half-precision conversion)
        //   bit 12: FMA3 (Fused Multiply-Add, three-operand VEX form)
        const uint32_t ecx_ssse3   = 1u << 9;
        const uint32_t ecx_sse41   = 1u << 19;
        const uint32_t ecx_sse42   = 1u << 20;
        const uint32_t ecx_popcnt  = 1u << 23;
        const uint32_t ecx_osxsave = 1u << 27;
        const uint32_t ecx_avx     = 1u << 28;
        const uint32_t ecx_fma3    = 1u << 12;
        const uint32_t ecx_aesni   = 1u << 25;  // AES-NI (Westmere+)
        const uint32_t ecx_pclmulqdq = 1u << 1; // PCLMULQDQ (Westmere+)
        f.ssse3  = (c & ecx_ssse3)  != 0;
        f.sse41  = (c & ecx_sse41)  != 0;
        f.sse42  = (c & ecx_sse42)  != 0;
        f.popcnt = (c & ecx_popcnt) != 0;
        f.aesni  = (c & ecx_aesni)  != 0;
        f.pclmulqdq = (c & ecx_pclmulqdq) != 0;
        // AVX / FMA3 require OS support (OSXSAVE) and YMM state in XCR0.
        // Cache the XCR0 value — it's also needed for AVX-512 below.
        bool osxsave = (c & ecx_osxsave) != 0;
        xcr0 = osxsave ? xgetbv(0) : 0;
        if (osxsave) {
            bool ymm_enabled = (xcr0 & (XCR0_XMM | XCR0_YMM))
                               == (XCR0_XMM | XCR0_YMM);
            if (ymm_enabled) {
                f.avx  = (c & ecx_avx)  != 0;
                f.fma3 = (c & ecx_fma3) != 0;
            }
        }
    }
    // CPUID leaf 7 subleaf 0: AVX2/BMI1/BMI2/AVX-512.
    // __cpuid_count() is a macro that expands to inline asm — it does
    // NOT return a value indicating whether the leaf is supported.
    // We use __get_cpuid_max() to check leaf availability first.
    // Leaf 7 is structured-extended, introduced with Haswell; all CPUs
    // that support it also support AVX/BMI1/BMI2/AVX2.
    unsigned int max_basic = __get_cpuid_max(0, 0);
    if (max_basic >= 7) {
        unsigned int a7=0, b7=0, c7=0, d7=0;
        __cpuid_count(7, 0, a7, b7, c7, d7);
        (void)a7; (void)c7; (void)d7;  // only EBX (b7) is used below
        b = b7;
        // EBX bits (CPUID.07H:EBX:0):
        //   bit 3:  BMI1 (andn, blsr, blsi, tzcnt)
        //   bit 5:  AVX2 (256-bit integer SIMD)
        //   bit 6:  FDP_BSDP (FPU data pointer is precise on fault)
        //   bit 8:  BMI2 (bzhi, pdep, pext, mulx, shlx/shrx/sarx)
        //   bit 16: AVX512F (Foundation)
        //   bit 17: AVX512DQ
        //   bit 21: AVX512_IFMA (52-bit integer FMA)
        //   bit 30: AVX512BW (Byte/Word)
        // ECX bits:
        //   bit 5:  WAITPKG
        //   bit 8:  GFNI
        //   bit 22: LA57 (5-level paging, for ASLR — not used here)
        // EDX bits:
        //   bit 4:  FSRM (Fast Short REP MOV)
        const uint32_t ebx_bmi1    = 1u << 3;
        const uint32_t ebx_avx2    = 1u << 5;
        const uint32_t ebx_bmi2    = 1u << 8;
        const uint32_t ebx_avx512f = 1u << 16;
        const uint32_t ebx_avx512dq= 1u << 17;
        const uint32_t ebx_avx512ifma = 1u << 21;
        const uint32_t ebx_avx512bw = 1u << 30;
        // EBX bit 29: SHA-NI (Intel Goldmont+ / AMD Zen+)
        const uint32_t ebx_sha     = 1u << 29;
        f.bmi1 = (b & ebx_bmi1) != 0;
        f.bmi2 = (b & ebx_bmi2) != 0;
        f.sha  = (b & ebx_sha)  != 0;
        // AVX2 requires AVX + YMM state (already checked for f.avx).
        bool avx_ok = f.avx;  // AVX implies YMM-enabled XCR0
        f.avx2 = avx_ok && (b & ebx_avx2) != 0;
        // LZCNT is part of ABM (CPUID.80000001H:ECX[5]) — separate from
        // BMI1. Check via extended CPUID leaf 0x80000001.
        unsigned int e2a=0, e2b=0, e2c=0, e2d=0;
        if (__get_cpuid(0x80000001, &e2a, &e2b, &e2c, &e2d)) {
            const uint32_t ecx_lzcnt = 1u << 5;
            f.lzcnt = (e2c & ecx_lzcnt) != 0;
        }
        // AVX-512 requires OS support for the OPMASK/ZMM state.
        if (f.avx) {
            bool zmm_enabled = (xcr0 & (XCR0_OPMASK | XCR0_ZMM_HI256 |
                                         XCR0_HI16_ZMM))
                                == (XCR0_OPMASK | XCR0_ZMM_HI256 |
                                    XCR0_HI16_ZMM);
            if (zmm_enabled) {
                f.avx512f    = (b & ebx_avx512f)    != 0;
                f.avx512dq   = (b & ebx_avx512dq)   != 0;
                f.avx512ifma = (b & ebx_avx512ifma) != 0;
                f.avx512bw   = (b & ebx_avx512bw)   != 0;
            }
        }
    }
    return f;
}
// ── Pretty-printer ────────────────────────────────────────────────────
// Used by the startup banner and --verbose stats. Returns a pointer to
// a thread-local buffer so callers don't need to free it.
const char* cpu_features_string(const CpuFeatures& f) {
    thread_local char buf[256];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1;
    auto append = [&](const char* s) {
        size_t n = 0; while (s[n]) n++;
        if (p + n + 1 < end) {
            if (p != buf) *p++ = ' ';
            size_t k = 0;
            while (k < n) { *p++ = s[k]; k++; }
        }
    };
    if (f.ssse3)     append("ssse3");
    if (f.sse41)     append("sse4.1");
    if (f.sse42)     append("sse4.2");
    if (f.popcnt)    append("popcnt");
    if (f.lzcnt)     append("lzcnt");
    if (f.aesni)     append("aesni");
    if (f.pclmulqdq) append("pclmulqdq");
    if (f.sha)       append("sha");
    if (f.avx)       append("avx");
    if (f.avx2)      append("avx2");
    if (f.fma3)      append("fma3");
    if (f.bmi1)      append("bmi1");
    if (f.bmi2)      append("bmi2");
    if (f.avx512f)   append("avx512f");
    if (f.avx512bw)  append("avx512bw");
    if (f.avx512dq)  append("avx512dq");
    if (f.avx512ifma)append("avx512ifma");
    if (p == buf) {
        // No features detected — this only happens on ancient CPUs
        // without SSE4.1 (pre-2008). The JIT falls back to baseline
        // SSE2 codegen in this case.
        append("baseline-sse2");
    }
    *p = '\0';
    return buf;
}
} // namespace arm64emu
