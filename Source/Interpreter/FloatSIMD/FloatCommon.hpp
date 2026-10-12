// FloatCommon.hpp — helpers shared by the FP/SIMD interpreter
// files (Dispatch.cpp, interp_simd_*.cpp, FloatScalar.cpp).
//
// Also used by JIT native SIMD helpers for matching ARM FP special cases.
// Everything here is static inline so each
// translation unit gets its own copy, as when they lived in Dispatch.cpp.
#pragma once
#include "System/Core/Emulator.hpp"
#include "Interpreter/Decoder/Decoder.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Interpreter/FloatSIMD/Crypto.hpp"
#include "Generated/FixedPointTable.hpp"
#include <cmath>
#include <cfenv>
#include <array>
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
// IEEE 754 binary16: 1 sign + 5 exp + 10 mantissa. These follow Arm's
// FPConvert with FPCR.AHP=0 and round-to-nearest-even (Linux defaults);
// FPCR.RMode and FPSR exception flags are not modeled. `dn` is FPCR.DN.
static inline float h2f(uint16_t h, bool dn = false) {
    const uint32_t sign = static_cast<uint32_t>(h >> 15) << 31;
    const uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t fbits;
    if (exp == 0) {
        if (mant == 0) {
            fbits = sign;
        } else {  // subnormal mant * 2^-24: exact as a normal single
            int e = 0;
            while (!(mant & 0x400)) { mant <<= 1; e--; }
            mant &= 0x3FF;
            fbits = sign | static_cast<uint32_t>(127 + e - 14) << 23 | mant << 13;
        }
    } else if (exp == 0x1F) {
        // Infinity, or a NaN: quieted with its payload (FPConvertNaN).
        if (mant == 0) fbits = sign | 0x7F800000u;
        else fbits = dn ? 0x7FC00000u : sign | 0x7FC00000u | mant << 13;
    } else {
        fbits = sign | (exp - 15 + 127) << 23 | mant << 13;
    }
    float f; memcpy(&f, &fbits, 4); return f;
}
// One correctly rounded step (a single's conversion to double is exact).
static inline uint16_t d2h(double d, bool dn = false) {
    uint64_t bits; memcpy(&bits, &d, 8);
    const uint16_t sign = static_cast<uint16_t>(bits >> 48) & 0x8000;
    const int exp = static_cast<int>(bits >> 52) & 0x7FF;
    const uint64_t frac = bits & ((1ULL << 52) - 1);
    if (exp == 0x7FF) {
        if (frac == 0) return sign | 0x7C00;
        // NaN: quiet bit plus the top payload bits.
        return dn ? 0x7E00 : static_cast<uint16_t>(sign | 0x7E00 | (frac >> 42));
    }
    // Doubles below 2^-25 (including subnormal doubles) round to zero.
    const int e = exp - 1023;
    if (exp == 0 || e < -25) return sign;
    const uint64_t m = frac | (1ULL << 52);
    int hexp = e + 15;
    const int shift = hexp > 0 ? 42 : 42 + (1 - hexp);  // keep 11 / fewer bits
    if (hexp <= 0) hexp = 0;
    uint64_t q = m >> shift;
    const uint64_t rem = m & ((1ULL << shift) - 1), halfway = 1ULL << (shift - 1);
    if (rem > halfway || (rem == halfway && (q & 1))) ++q;
    if (hexp == 0) return static_cast<uint16_t>(sign | q);  // 0x400 = smallest normal
    if (q >> 11) { q >>= 1; ++hexp; }
    if (hexp >= 0x1F) return sign | 0x7C00;  // overflow rounds to infinity
    return static_cast<uint16_t>(sign | hexp << 10 | (q & 0x3FF));
}
static inline uint16_t f2h(float f, bool dn = false) {
    return d2h(static_cast<double>(f), dn);
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
// Vector FRINT mode (0=N 1=A 2=M 3=X 4=Z 5=I 6=P) of an encoding with Q
// and sz ignored, or -1 when it is not a vector FRINT.
static inline int arm_frint_vector_mode(uint32_t op) {
    switch (op & 0xBFBFFC00) {
        case 0x0E218800: return 0; case 0x2E218800: return 1;
        case 0x0E219800: return 2; case 0x2E219800: return 3;
        case 0x0EA19800: return 4; case 0x2EA19800: return 5;
        case 0x0EA18800: return 6; default: return -1;
    }
}
// FPRoundInt for one lane. X and I use the host's round-to-nearest mode
// (FPCR.RMode is not modelled); NaNs are quieted, or the default NaN when
// FPCR.DN is set (libm passes signalling NaNs through).
template <typename T> static inline T arm_frint(int mode, T a, bool dn) {
    if (std::isnan(a)) return dn ? fp_default_nan<T>() : fp_quiet(a);
    switch (mode) {
        case 1: return std::round(a);
        case 2: return std::floor(a);
        case 4: return std::trunc(a);
        case 6: return std::ceil(a);
        default: return std::rint(a);
    }
}
enum class ArmFpOp { Add, Sub, Mul, Div, Max, Min, MaxNum, MinNum, MulX, Abd };
template <typename T> static inline T arm_fp_binop_raw(ArmFpOp op, T a, T b) {
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
template <typename T> static inline T arm_fp_muladd_raw(T addend, T a, T b) {
    const bool inf_zero = (std::isinf(a) && b == 0) || (a == 0 && std::isinf(b));
    T nan;
    if (arm_process_nans3(addend, a, b, nan))
        return fp_is_qnan(addend) && inf_zero ? fp_default_nan<T>() : nan;
    if (inf_zero) return fp_default_nan<T>();
    return arm_fp_result(std::fma(a, b, addend));
}
// FPCR.DN: every NaN result of an arithmetic operation is the default NaN.
template <typename T> static inline T arm_fp_dn(T r, bool dn) {
    return dn && std::isnan(r) ? fp_default_nan<T>() : r;
}
template <typename T> static inline T arm_fp_binop(ArmFpOp op, T a, T b, bool dn = false) {
    return arm_fp_dn(arm_fp_binop_raw(op, a, b), dn);
}
template <typename T> static inline T arm_fp_muladd(T addend, T a, T b, bool dn = false) {
    return arm_fp_dn(arm_fp_muladd_raw(addend, a, b), dn);
}
// FPConvert between single and double: host conversion quiets a NaN and
// keeps its sign and top payload bits, as Arm does; DN gives the default.
template <typename To, typename From> static inline To arm_fp_convert(From a, bool dn) {
    return dn && std::isnan(a) ? fp_default_nan<To>() : static_cast<To>(a);
}
// FPSqrt: a NaN operand is processed; a negative non-zero is invalid (+qNaN);
// -0 stays -0.
template <typename T> static inline T arm_fp_sqrt(T a, bool dn = false) {
    if (std::isnan(a)) return arm_fp_dn(fp_is_snan(a) ? fp_quiet(a) : a, dn);
    if (a < 0) return fp_default_nan<T>();
    return std::sqrt(a);
}
// Reciprocal estimate/refinement instructions use FPCR, rather than the
// ambient host rounding mode. Keep the host environment private to this
// instruction and accumulate its exceptions in the guest FPSR.
struct RecipFpEnv {
    CPU& cpu;
    fenv_t saved;
    explicit RecipFpEnv(CPU& c) : cpu(c) {
        feholdexcept(&saved);
        const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
        fesetround(modes[(cpu.fpcr >> 22) & 3]);
    }
    ~RecipFpEnv() {
        int flags = fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW |
                                 FE_UNDERFLOW | FE_INEXACT);
        if (flags & FE_INVALID) cpu.fpsr |= 1u;
        if (flags & FE_DIVBYZERO) cpu.fpsr |= 2u;
        if (flags & FE_OVERFLOW) cpu.fpsr |= 4u;
        if (flags & FE_UNDERFLOW) cpu.fpsr |= 8u;
        if (flags & FE_INEXACT) cpu.fpsr |= 16u;
        fesetenv(&saved);
    }
};
template <typename T> static inline T recip_input(CPU& cpu, T value) {
    if ((cpu.fpcr & (1u << 24)) && std::fpclassify(value) == FP_SUBNORMAL) {
        cpu.fpsr |= 1u << 7; // FPCR.FZ: input denormal cumulative exception
        return fp_from<T>(fp_bits(value) & FpTraits<T>::kSign);
    }
    return value;
}
// FPRecipStepFused / FPRSqrtStepFused: negate the FIRST operand before
// NaN processing (including its NaN sign), and special-case infinity*zero.
template <typename T> static inline T arm_recip_step(CPU& cpu, T a, T b, bool sqrt) {
    a = recip_input(cpu, fp_neg(a));
    b = recip_input(cpu, b);
    T nan;
    if (arm_process_nans(a, b, nan)) {
        if (fp_is_snan(a) || fp_is_snan(b)) cpu.fpsr |= 1u;
        return cpu.fpcr & (1u << 25) ? fp_default_nan<T>() : nan;
    }
    if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b)))
        return sqrt ? T(1.5) : T(2);
    if (!sqrt) return std::fma(a, b, T(2));
    if (std::isinf(a) || std::isinf(b)) return a * b;
    // Halve an operand EXACTLY before the fused add, not a rounded result:
    // fma(a,b,3)/2 can overflow even when the final result is representable.
    // A normal with exponent >= 2, or an even significand, halves exactly.
    constexpr int fraction = sizeof(T) == 4 ? 23 : 52;
    constexpr uint64_t exp_mask = sizeof(T) == 4 ? 255 : 2047;
    auto halves_exactly = [](T v) {
        auto bits = fp_bits(v);
        return ((bits >> fraction) & exp_mask) >= 2 || !(bits & 1);
    };
    if (halves_exactly(a)) return std::fma(std::ldexp(a, -1), b, T(1.5));
    if (halves_exactly(b)) return std::fma(a, std::ldexp(b, -1), T(1.5));
    // Both operands are tiny odd significands: their nonzero product is
    // much smaller than an ulp of 1.5, but still affects directed rounding.
    cpu.fpsr |= 16u;
    const bool negative = ((fp_bits(a) ^ fp_bits(b)) & FpTraits<T>::kSign) != 0;
    const unsigned mode = (cpu.fpcr >> 22) & 3;
    if (!negative && mode == 1)
        return std::nextafter(T(1.5), std::numeric_limits<T>::infinity());
    if (negative && (mode == 2 || mode == 3)) return std::nextafter(T(1.5), T(0));
    return T(1.5);
}
// Arm RecipEstimate / RecipSqrtEstimate have a specified eight-bit
// estimate. Full host division/sqrt is more precise, but is NOT this ISA.
template <typename T> static inline T arm_recip_estimate(CPU& cpu, T input, bool sqrt) {
    using U = typename FpTraits<T>::U;
    constexpr int fraction = sizeof(T) == 4 ? 23 : 52;
    constexpr int max_exp = sizeof(T) == 4 ? 255 : 2047;
    constexpr int bias = max_exp / 2;
    const T x = recip_input(cpu, input);
    const U bits = fp_bits(x), sign = bits & FpTraits<T>::kSign;
    const U abs = bits & ~FpTraits<T>::kSign;
    const U infinity = U(max_exp) << fraction;
    if (std::isnan(x)) {
        if (fp_is_snan(x)) cpu.fpsr |= 1u;
        return cpu.fpcr & (1u << 25) ? fp_default_nan<T>() : fp_quiet(x);
    }
    if (abs == 0) { cpu.fpsr |= 2u; return fp_from<T>(sign | infinity); }
    if (sqrt && sign) { cpu.fpsr |= 1u; return fp_default_nan<T>(); }
    if (abs == infinity) return fp_from<T>(sign);
    int exponent = static_cast<int>(abs >> fraction);
    uint64_t frac = uint64_t(abs & ((U(1) << fraction) - 1)) << (52 - fraction);
    if (!sqrt) {
        if (abs < (U(1) << (fraction - 2))) {
            cpu.fpsr |= 4u | 16u;
            unsigned mode = (cpu.fpcr >> 22) & 3;
            bool to_inf = mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign);
            return fp_from<T>(sign | (to_inf ? infinity : infinity - 1));
        }
        if (exponent >= max_exp - 2 && (cpu.fpcr & (1u << 24))) {
            cpu.fpsr |= 8u;
            return fp_from<T>(sign);
        }
        if (exponent == 0) {
            if (!(frac & (1ull << 51))) { exponent = -1; frac <<= 2; }
            else frac <<= 1;
        }
        unsigned scaled = 256 | ((frac >> 44) & 255);
        unsigned estimate = (((1u << 19) / (2 * scaled + 1)) + 1) >> 1;
        exponent = max_exp - 2 - exponent;
        frac = uint64_t(estimate & 255) << 44;
        if (exponent == 0) frac = (frac >> 1) | (1ull << 51);
        else if (exponent == -1) { frac = (frac >> 2) | (1ull << 50); exponent = 0; }
    } else {
        if (exponent == 0) {
            while (!(frac & (1ull << 51))) { frac <<= 1; --exponent; }
            frac = (frac & ((1ull << 51) - 1)) << 1;
        }
        unsigned scaled = (exponent & 1) ? 128 | ((frac >> 45) & 127)
                                         : 256 | ((frac >> 44) & 255);
        // The integer inequality is the exact midpoint rule from Arm's
        // pseudocode. Build its small lookup table once, shared by lanes.
        static const auto estimates = [] {
            std::array<uint16_t, 384> table{};
            for (unsigned i = 128; i < 512; ++i) {
                unsigned a = i < 256 ? 2 * i + 1 : 2 * ((i & ~1u) + 1);
                unsigned b = 512;
                while (a * (b + 1) * (b + 1) < (1u << 28)) ++b;
                table[i - 128] = (b + 1) / 2;
            }
            return table;
        }();
        exponent = (3 * bias - 1 - exponent) / 2;
        frac = uint64_t(estimates[scaled - 128] & 255) << 44;
    }
    return fp_from<T>(sign | (U(exponent) << fraction) | U(frac >> (52 - fraction)));
}
// Shared dispatch for scalar/vector S/D forms. Half precision and Q=0,D
// remain UNDEFINED; FEAT_FP16/AFP/RPRES are not advertised by this profile.
static inline bool exec_reciprocal(uint32_t op, CPU& cpu) {
    if (!fp_decode::is_reciprocal(op)) return false;
    if (fp_decode::is_reciprocal_fp16(op)) throw DecodeError(cpu.pc, op);
    const uint32_t step = op & 0xBFA0FC00;
    const bool is_step = step == 0x0E20FC00 || step == 0x0EA0FC00 ||
                         step == 0x1E20FC00 || step == 0x1EA0FC00;
    const bool scalar = (op >> 28) & 1;
    const bool q = (op >> 30) & 1;
    const bool dbl = (op >> 22) & 1;
    if (!scalar && dbl && !q) throw DecodeError(cpu.pc, op);
    const bool sqrt = is_step ? ((op >> 23) & 1) : ((op >> 29) & 1);
    const unsigned rd = op & 31, rn = (op >> 5) & 31, rm = (op >> 16) & 31;
    const unsigned bytes = dbl ? 8 : 4;
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / bytes;
    uint8_t a[16], b[16], result[16]{};
    memcpy(a, &cpu.v_lo[rn], 8); memcpy(a + 8, &cpu.v_hi[rn], 8);
    memcpy(b, &cpu.v_lo[rm], 8); memcpy(b + 8, &cpu.v_hi[rm], 8);
    RecipFpEnv env(cpu);
    for (unsigned i = 0; i < lanes; ++i) {
        if (dbl) {
            double x, y, r;
            memcpy(&x, a + i * 8, 8); memcpy(&y, b + i * 8, 8);
            r = is_step ? arm_recip_step(cpu, x, y, sqrt) : arm_recip_estimate(cpu, x, sqrt);
            memcpy(result + i * 8, &r, 8);
        } else {
            float x, y, r;
            memcpy(&x, a + i * 4, 4); memcpy(&y, b + i * 4, 4);
            r = is_step ? arm_recip_step(cpu, x, y, sqrt) : arm_recip_estimate(cpu, x, sqrt);
            memcpy(result + i * 4, &r, 4);
        }
    }
    memcpy(&cpu.v_lo[rd], result, 8); memcpy(&cpu.v_hi[rd], result + 8, 8);
    return true;
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
