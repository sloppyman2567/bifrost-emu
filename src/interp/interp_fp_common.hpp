// interp/interp_fp_common.hpp — helpers shared by the FP/SIMD interpreter
// files (interp_fp.cpp, interp_simd_*.cpp, interp_fp_scalar.cpp).
//
// Private to src/interp/. Everything here is static inline so each
// translation unit gets its own copy, as when they lived in interp_fp.cpp.
#pragma once
#include "core/emulator.h"
#include "decoder.hpp"
#include "debug_flags.h"
#include "interp/interp_crypto.hpp"
#include "opgen_fpfixed.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <set>
namespace arm64emu {
// ── FP register access helpers (file-scope, no per-dispatch allocation) ──
// These were previously local lambdas inside the FP_SCALAR case, which
// meant they were reconstructed on every FP instruction dispatch. Moving
// them to file scope eliminates that overhead.
static inline double read_fp_d(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint64_t bits = cpu.v_lo[r];
    double d; memcpy(&d, &bits, 8); return d;
}
static inline float read_fp_s(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint32_t bits = static_cast<uint32_t>(cpu.v_lo[r]);
    float f; memcpy(&f, &bits, 4); return f;
}
static inline void write_fp_d(CPU& cpu, int r, double d) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    if (dbg().nan_trace && std::isnan(d))
        fprintf(stderr, "[NAN-D] r%d = %.17g pc=0x%llx\n", r, d, (unsigned long long)cpu.pc);
    uint64_t bits; memcpy(&bits, &d, 8);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
static inline void write_fp_s(CPU& cpu, int r, float f) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    if (dbg().nan_trace && std::isnan(f))
        fprintf(stderr, "[NAN-S] r%d = %.9g pc=0x%llx\n", r, f, (unsigned long long)cpu.pc);
    uint32_t bits; memcpy(&bits, &f, 4);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
// ── Half-precision (FP16) helpers ──────────────────────────────────────
// IEEE 754 binary16: 1 sign + 5 exp + 10 mantissa.
static inline float h2f(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t fbits;
    if (exp == 0) {
        if (mant == 0) {
            fbits = sign << 31;
        } else {
            int e = -1;
            while (!(mant & 0x400)) { mant <<= 1; e--; }
            mant &= 0x3FF;
            fbits = (sign << 31) | ((127 + e - 14) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        fbits = (sign << 31) | (0xFFu << 23) | (mant << 13);
    } else {
        fbits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &fbits, 4); return f;
}
static inline uint16_t f2h(float f) {
    uint32_t fbits; memcpy(&fbits, &f, 4);
    uint32_t sign = (fbits >> 31) & 1;
    int32_t  exp  = static_cast<int32_t>((fbits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (fbits & 0x7FFFFF) >> 13;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign << 15);
        mant |= 0x400;
        mant >>= (1 - exp);
        return static_cast<uint16_t>((sign << 15) | mant);
    } else if (exp >= 0x1F) {
        return static_cast<uint16_t>((sign << 15) | (0x1F << 10));
    }
    return static_cast<uint16_t>((sign << 15) | (exp << 10) | mant);
}
static inline uint16_t d2h(double d) {
    return f2h(static_cast<float>(d));
}
// ── FP→int conversion with ARM saturation semantics ──────────────────
// A C++ static_cast<int64_t>/<uint64_t> of a double in [2^63, 2^64) (or
// outside the signed range) lowers to x86 cvttsd2si, which returns the
// 0x8000000000000000 sentinel for ANY out-of-range input — NOT the
// architectural saturate. Range-check first; the unsigned [2^63, 2^64)
// band is handled with the subtract-2^63-then-add-back trick so the
// sentinel never surfaces (mirrors the JIT; AGENTS.md).
static inline int64_t fp_to_signed_sat(double v, bool is_64bit) {
    double hi = is_64bit ? 9223372036854775808.0 : 2147483648.0;
    double lo = -hi;
    if (std::isnan(v)) return 0;
    if (v >= hi) return is_64bit ? INT64_MAX : INT32_MAX;
    if (v < lo)  return is_64bit ? INT64_MIN : INT32_MIN;
    return is_64bit ? static_cast<int64_t>(v) : static_cast<int32_t>(v);
}
static inline uint64_t fp_to_unsigned_sat(double v, bool is_64bit) {
    double hi = is_64bit ? 18446744073709551616.0 : 4294967296.0;
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= hi) return is_64bit ? ~0ULL : 0xFFFFFFFFu;
    if (v >= 9223372036854775808.0) {
        uint64_t r = static_cast<uint64_t>(v - 9223372036854775808.0);
        return is_64bit ? r + 0x8000000000000000ULL : static_cast<uint32_t>(r);
    }
    return is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(v);
}
// ── Round-to-Odd double→float narrowing (FCVTXN / FCVTXN2) ─────────
// FPRound(FPRounding_ODD): round_up = FALSE (truncate toward zero),
// then if the result is inexact and the retained mantissa LSB is 0,
// increment it — i.e. truncate, then force an odd LSB when any bit
// was discarded. Overflow saturates to FPMaxNormal (never ±inf).
// A plain nearest-even cast followed by "|1" is WRONG when the cast
// rounded AWAY from the exact value (e.g. 0x3FEFFFFFFFFFFFFF casts up
// to 1.0f; OR'ing makes it larger than the input instead of
// truncating to 0x3F7FFFFF).
static inline float fp_to_f32_rto(double dv) {
    float fv = static_cast<float>(dv);
    if (!std::isfinite(dv)) return fv;               // NaN / ±inf pass through
    if (std::isinf(fv)) {                            // overflow → FPMaxNormal
        uint32_t u = std::signbit(fv) ? 0xFF7FFFFFu : 0x7F7FFFFFu;
        memcpy(&fv, &u, 4);
        return fv;
    }
    if (static_cast<double>(fv) != dv) {
        // Nearest-even rounded away from zero past the exact value →
        // step back one ulp toward zero to get the truncated value.
        if ((fv > 0 && (double)fv > dv) || (fv < 0 && (double)fv < dv))
            fv = std::nextafter(fv, 0.0f);
        uint32_t u;
        memcpy(&u, &fv, 4);
        u |= 1;  // round-to-odd: odd LSB if any bit was discarded
        memcpy(&fv, &u, 4);
    }
    return fv;
}
// ── ARM FP special cases (FPCR.DN=0, FZ=0, as Linux runs guests) ───────
// Plain C++ arithmetic follows x86: with two NaN operands the result takes
// whichever payload the generated code's operand order selects (and the
// compiler may commute a + b), and invalid operations return x86's
// negative default NaN. ARM ARM FPProcessNaNs/FPProcessNaNs3 return the
// first signalling NaN in operand order, else the first quiet NaN,
// quietened; FPDefaultNaN is +qNaN. Lanes routed through these helpers
// therefore do not depend on host code generation.
template <typename T> struct FpTraits;
template <> struct FpTraits<float> {
    using U = uint32_t;
    static constexpr U kQuiet = 0x00400000u, kDefault = 0x7FC00000u,
                       kSign = 0x80000000u, kTwo = 0x40000000u;
};
template <> struct FpTraits<double> {
    using U = uint64_t;
    static constexpr U kQuiet = 0x0008000000000000ull,
                       kDefault = 0x7FF8000000000000ull,
                       kSign = 0x8000000000000000ull,
                       kTwo = 0x4000000000000000ull;
};
template <typename T> static inline typename FpTraits<T>::U fp_bits(T v) {
    typename FpTraits<T>::U u; memcpy(&u, &v, sizeof u); return u;
}
template <typename T> static inline T fp_from(typename FpTraits<T>::U u) {
    T v; memcpy(&v, &u, sizeof v); return v;
}
template <typename T> static inline bool fp_is_snan(T v) {
    return std::isnan(v) && !(fp_bits(v) & FpTraits<T>::kQuiet);
}
template <typename T> static inline bool fp_is_qnan(T v) {
    return std::isnan(v) && (fp_bits(v) & FpTraits<T>::kQuiet);
}
template <typename T> static inline T fp_quiet(T v) {
    return fp_from<T>(fp_bits(v) | FpTraits<T>::kQuiet);
}
template <typename T> static inline T fp_default_nan() {
    return fp_from<T>(FpTraits<T>::kDefault);
}
// FPNeg / FPAbs act on the sign bit, NaNs included.
template <typename T> static inline T fp_neg(T v) {
    return fp_from<T>(fp_bits(v) ^ FpTraits<T>::kSign);
}
template <typename T> static inline T fp_abs(T v) {
    return fp_from<T>(fp_bits(v) & ~FpTraits<T>::kSign);
}
// FPProcessNaNs: true, with the NaN to return, when an operand is a NaN.
template <typename T> static inline bool arm_process_nans(T a, T b, T& out) {
    if (fp_is_snan(a)) { out = fp_quiet(a); return true; }
    if (fp_is_snan(b)) { out = fp_quiet(b); return true; }
    if (std::isnan(a)) { out = a; return true; }
    if (std::isnan(b)) { out = b; return true; }
    return false;
}
// FPProcessNaNs3: signalling NaNs first, each tier in operand order.
template <typename T> static inline bool arm_process_nans3(T a, T b, T c, T& out) {
    if (fp_is_snan(a)) { out = fp_quiet(a); return true; }
    if (fp_is_snan(b)) { out = fp_quiet(b); return true; }
    if (fp_is_snan(c)) { out = fp_quiet(c); return true; }
    if (std::isnan(a)) { out = a; return true; }
    if (std::isnan(b)) { out = b; return true; }
    if (std::isnan(c)) { out = c; return true; }
    return false;
}
// A NaN computed from NaN-free operands is an invalid operation.
template <typename T> static inline T arm_fp_result(T r) {
    return std::isnan(r) ? fp_default_nan<T>() : r;
}
enum class ArmFpOp { Add, Sub, Mul, Div, Max, Min, MaxNum, MinNum, MulX, Abd };
template <typename T> static inline T arm_fp_binop(ArmFpOp op, T a, T b) {
    if (op == ArmFpOp::MaxNum || op == ArmFpOp::MinNum) {
        // FPMaxNum/FPMinNum: a lone quiet NaN becomes -inf/+inf so the
        // number wins; signalling NaNs still propagate through FPMax/FPMin.
        const T inf = op == ArmFpOp::MaxNum ? -std::numeric_limits<T>::infinity()
                                            : std::numeric_limits<T>::infinity();
        if (fp_is_qnan(a) && !fp_is_qnan(b)) a = inf;
        else if (!fp_is_qnan(a) && fp_is_qnan(b)) b = inf;
        op = op == ArmFpOp::MaxNum ? ArmFpOp::Max : ArmFpOp::Min;
    }
    T nan;
    if (arm_process_nans(a, b, nan)) return op == ArmFpOp::Abd ? fp_abs(nan) : nan;
    switch (op) {
        case ArmFpOp::Add: return arm_fp_result(a + b);
        case ArmFpOp::Sub: return arm_fp_result(a - b);
        case ArmFpOp::Mul: return arm_fp_result(a * b);
        case ArmFpOp::Div: return arm_fp_result(a / b);
        case ArmFpOp::Abd: return fp_abs(arm_fp_result(a - b));
        case ArmFpOp::MulX:
            // FPMulX: inf * 0 is 2.0 with the XOR of the signs.
            if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b)))
                return fp_from<T>(((fp_bits(a) ^ fp_bits(b)) & FpTraits<T>::kSign) |
                                  FpTraits<T>::kTwo);
            return arm_fp_result(a * b);
        case ArmFpOp::Max:
            // +0 is greater than -0.
            if (a == 0 && b == 0) return (std::signbit(a) && std::signbit(b)) ? a : T(0);
            return a > b ? a : b;
        default:  // Min
            if (a == 0 && b == 0) return (std::signbit(a) || std::signbit(b)) ? -T(0) : T(0);
            return a < b ? a : b;
    }
}
// FPMulAdd (FMLA/FMLS/FMADD): addend + a*b with a single rounding. A quiet
// NaN addend does not mask an invalid inf*0, which gives the default NaN.
template <typename T> static inline T arm_fp_muladd(T addend, T a, T b) {
    const bool inf_zero = (std::isinf(a) && b == 0) || (a == 0 && std::isinf(b));
    T nan;
    if (arm_process_nans3(addend, a, b, nan))
        return fp_is_qnan(addend) && inf_zero ? fp_default_nan<T>() : nan;
    if (inf_zero) return fp_default_nan<T>();
    return arm_fp_result(std::fma(a, b, addend));
}
// ── Integer saturation helpers (SQADD/UQADD/SQSUB/UQSUB/SQSHL/SQABS/
//    SQNEG/SUQADD/USQADD family, added 2026-08-22). Width-generic via
//    `bits` (8/16/32/64); sign-extension through int64 so every width
//    computes in a common type. __int128 for the 64-bit add/sub so the
//    overflow case never wraps before the clamp.
static inline uint64_t sext_lane(uint64_t v, int bits) {
    return (bits >= 64) ? v
                        : static_cast<uint64_t>(static_cast<int64_t>(v << (64 - bits)) >>
                                                (64 - bits));
}
static inline uint64_t sat_signed_i128(__int128 v, int bits) {
    const __int128 hi = (bits >= 64) ? (__int128)INT64_MAX
                                     : ((__int128)1 << (bits - 1)) - 1;
    const __int128 lo = -(static_cast<__int128>(1) << (bits - 1));
    if (v > hi) return static_cast<uint64_t>(hi);
    if (v < lo) return static_cast<uint64_t>(lo);
    return static_cast<uint64_t>(static_cast<int64_t>(v));
}
// SQADD / SQSUB: signed saturating add/sub of two lane values.
// CRITICAL: sext_lane returns a uint64_t BIT PATTERN; casting it straight
// to __int128 ZERO-extends (unsigned→unsigned value conversion), turning
// e.g. -128 into +1.8e19 → bogus clamp to +max. Route through int64_t so
// __int128 receives the SIGN-extended value.
static inline uint64_t sat_add_s(uint64_t a, uint64_t b, int bits) {
    return sat_signed_i128(
        static_cast<__int128>(static_cast<int64_t>(sext_lane(a, bits))) +
        static_cast<__int128>(static_cast<int64_t>(sext_lane(b, bits))),
        bits);
}
static inline uint64_t sat_sub_s(uint64_t a, uint64_t b, int bits) {
    return sat_signed_i128(
        static_cast<__int128>(static_cast<int64_t>(sext_lane(a, bits))) -
        static_cast<__int128>(static_cast<int64_t>(sext_lane(b, bits))),
        bits);
}
// UQADD / UQSUB: unsigned saturating add/sub.
static inline uint64_t sat_add_u(uint64_t a, uint64_t b, int bits) {
    const uint64_t max = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t r = a + b;                 // wrap-safe: detect via <
    return (r < a || r < b || r > max) ? max : r;
}
static inline uint64_t sat_sub_u(uint64_t a, uint64_t b, int /*bits*/) {
    return (a >= b) ? a - b : 0;
}
// SQABS / SQNEG: saturating to the SIGNED range — unlike plain ABS/NEG,
// INT_MIN saturates to INT_MAX instead of wrapping back to itself.
static inline uint64_t sat_abs_s(uint64_t v, int bits) {
    int64_t s = static_cast<int64_t>(sext_lane(v, bits));
    return sat_signed_i128(s < 0 ? -static_cast<__int128>(s)
                                 : static_cast<__int128>(s),
                           bits);
}
static inline uint64_t sat_neg_s(uint64_t v, int bits) {
    return sat_signed_i128(
        -static_cast<__int128>(static_cast<int64_t>(sext_lane(v, bits))),
        bits);
}
// Signed max/min lane values (SQSHL saturation limits).
static inline uint64_t smax_lane(int bits) {
    return (bits >= 64) ? static_cast<uint64_t>(INT64_MAX)
                        : (1ULL << (bits - 1)) - 1;
}
static inline uint64_t smin_lane(int bits) {
    return sext_lane(static_cast<uint64_t>(1) << (bits - 1), bits);
}
// SIMD_DP fields decoded once by execute_fp and shared by every group.
struct SimdDpFields {
    uint32_t op;
    bool Q, U;
    uint8_t size, rm, rn, rd;
    // bits[31:24] + bits[20:10], and the same with Q stripped.
    uint32_t sub, sub_noq;
    explicit SimdDpFields(uint32_t raw)
        : op(raw), Q((raw >> 30) & 1), U((raw >> 29) & 1),
          size((raw >> 22) & 3), rm((raw >> 16) & 0x1F),
          rn((raw >> 5) & 0x1F), rd(raw & 0x1F),
          sub(raw & 0xFFE0FC00), sub_noq((raw & 0xFFE0FC00) & ~(1u << 30)) {}
};
} // namespace arm64emu
