/*
 * jit_int_fp_conv.c — JIT native int↔FP conversion tests.
 *
 * Background (beta.3):
 *   The JIT and interpreter both had a critical bug where `scvtf s0, w0`
 *   (32-bit GPR → single-precision FP) was misdecoded as `fmov w0, s0`
 *   (FP→GPR bit copy) because the FMOV (32-bit) check used mask
 *   0xFFE0FC00 with value 0x1E200000, which also matches SCVTF
 *   (0x1E220000) and UCVTF (0x1E230000). The 64-bit FMOV check had a
 *   `(op & (1u << 18))` guard, but the 32-bit check was missing it.
 *
 *   This broke musl's __floatscan inf/nan detection: strtod("-inf")
 *   returned -nan because the sign computation does `scvtf s1, w23`
 *   with w23=-1 and expects s1=-1.0f, but the misdecoded FMOV copied
 *   the old s0 (zero) into w23 instead.
 *
 *   Additionally, UCVTF (0x1E230000) and FCVTZU (0x1E390000) did not
 *   match their respective SCVTF/FCVTZS checks because the mask
 *   0x7F3F0000 included bit 16 (the U/S selector). The fix is to use
 *   mask 0x7F3E0000 which excludes bit 16.
 *
 *   The JIT's FP_I2F also had two bugs:
 *     1. Always used 64-bit CVTSI2SD/SS (didn't handle 32-bit GPR source)
 *     2. Used 0x43E0000000000000 (double 2^63) even for single-precision
 *        unsigned path (should be 0x5F000000)
 *
 * This test exercises all 8 variants of int↔FP conversion:
 *   SCVTF/UCVTF × (32-bit/64-bit GPR) × (single/double FP)
 *   FCVTZS/FCVTZU × (32-bit/64-bit GPR) × (single/double FP)
 *
 * Uses pure C with noinline functions. The compiler emits the relevant
 * ARM64 instructions for these patterns.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

/* ── Vector UCVTF (u32 lanes → f32), one rounding ─────────────────
 * The JIT's (float)(x>>1)*2 + (float)(x&1) rounded twice: odd inputs whose
 * half is a tie came out one ULP low (0x324500a1 gave 0x4e491402). The
 * expected bits are host round-to-nearest conversions. */
__attribute__((noinline)) void ucvtf_4s(const uint32_t in[4], uint32_t out[4]) {
    asm volatile("ld1 {v0.4s}, [%0]\n ucvtf v0.4s, v0.4s\n st1 {v0.4s}, [%1]"
                 :: "r"(in), "r"(out) : "v0", "memory");
}
__attribute__((noinline)) void ucvtf_2s(const uint32_t in[2], uint32_t out[4]) {
    asm volatile("movi v0.2d, #0xffffffffffffffff\n ld1 {v0.d}[0], [%0]\n"
                 " ucvtf v0.2s, v0.2s\n st1 {v0.4s}, [%1]"
                 :: "r"(in), "r"(out) : "v0", "memory");
}
static const uint32_t ucvtf_cases[][2] = {
    {0x324500a1, 0x4e491403}, {0x80000081, 0x4f000001}, {0x80000181, 0x4f000002},
    {0x01000001, 0x4b800000}, {0x01000003, 0x4b800002}, {0xffffffff, 0x4f800000},
    {0x7fffffc1, 0x4f000000}, {0x00ffffff, 0x4b7fffff}, {0xfffffe81, 0x4f7fffff},
    {0x40000041, 0x4e800001},
};

/* ── SCVTF (signed int → FP) ─────────────────────────────────────── */
__attribute__((noinline)) float scvtf_w(int32_t i) {
    float f;
    asm volatile("scvtf %s0, %w1" : "=w"(f) : "r"(i));
    return f;
}
__attribute__((noinline)) double scvtf_w_d(int32_t i) {
    double d;
    asm volatile("scvtf %d0, %w1" : "=w"(d) : "r"(i));
    return d;
}
__attribute__((noinline)) float scvtf_x(int64_t i) {
    float f;
    asm volatile("scvtf %s0, %x1" : "=w"(f) : "r"(i));
    return f;
}
__attribute__((noinline)) double scvtf_x_d(int64_t i) {
    double d;
    asm volatile("scvtf %d0, %x1" : "=w"(d) : "r"(i));
    return d;
}

/* ── UCVTF (unsigned int → FP) ───────────────────────────────────── */
__attribute__((noinline)) float ucvtf_w(uint32_t i) {
    float f;
    asm volatile("ucvtf %s0, %w1" : "=w"(f) : "r"(i));
    return f;
}
__attribute__((noinline)) double ucvtf_w_d(uint32_t i) {
    double d;
    asm volatile("ucvtf %d0, %w1" : "=w"(d) : "r"(i));
    return d;
}
__attribute__((noinline)) float ucvtf_x(uint64_t i) {
    float f;
    asm volatile("ucvtf %s0, %x1" : "=w"(f) : "r"(i));
    return f;
}
__attribute__((noinline)) double ucvtf_x_d(uint64_t i) {
    double d;
    asm volatile("ucvtf %d0, %x1" : "=w"(d) : "r"(i));
    return d;
}

/* ── FCVTZS (FP → signed int, toward zero) ───────────────────────── */
__attribute__((noinline)) int32_t fcvtzs_w_s(float f) {
    int32_t i;
    asm volatile("fcvtzs %w0, %s1" : "=r"(i) : "w"(f));
    return i;
}
__attribute__((noinline)) int64_t fcvtzs_x_s(float f) {
    int64_t i;
    asm volatile("fcvtzs %x0, %s1" : "=r"(i) : "w"(f));
    return i;
}
__attribute__((noinline)) int32_t fcvtzs_w_d(double d) {
    int32_t i;
    asm volatile("fcvtzs %w0, %d1" : "=r"(i) : "w"(d));
    return i;
}
__attribute__((noinline)) int64_t fcvtzs_x_d(double d) {
    int64_t i;
    asm volatile("fcvtzs %x0, %d1" : "=r"(i) : "w"(d));
    return i;
}

/* ── FCVTZU (FP → unsigned int, toward zero) ─────────────────────── */
__attribute__((noinline)) uint32_t fcvtzu_w_s(float f) {
    uint32_t i;
    asm volatile("fcvtzu %w0, %s1" : "=r"(i) : "w"(f));
    return i;
}
__attribute__((noinline)) uint64_t fcvtzu_x_s(float f) {
    uint64_t i;
    asm volatile("fcvtzu %x0, %s1" : "=r"(i) : "w"(f));
    return i;
}
__attribute__((noinline)) uint32_t fcvtzu_w_d(double d) {
    uint32_t i;
    asm volatile("fcvtzu %w0, %d1" : "=r"(i) : "w"(d));
    return i;
}
__attribute__((noinline)) uint64_t fcvtzu_x_d(double d) {
    uint64_t i;
    asm volatile("fcvtzu %x0, %d1" : "=r"(i) : "w"(d));
    return i;
}

/* Bit-pattern helpers for exact FP comparison */
static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

int main(void) {
    /* ── SCVTF (signed) ─────────────────────────────────────────── */
    CHECK(fbits(scvtf_w(-1))    == 0xBF800000ULL, "scvtf_w(-1)");
    CHECK(fbits(scvtf_w(0))     == 0x00000000ULL, "scvtf_w(0)");
    CHECK(fbits(scvtf_w(1))     == 0x3F800000ULL, "scvtf_w(1)");
    CHECK(fbits(scvtf_w(42))    == 0x42280000ULL, "scvtf_w(42)");
    CHECK(fbits(scvtf_w(-42))   == 0xC2280000ULL, "scvtf_w(-42)");
    CHECK(fbits(scvtf_w(0x7FFFFFFF)) == 0x4F000000ULL, "scvtf_w(INT32_MAX)");

    CHECK(dbits(scvtf_w_d(-1))  == 0xBFF0000000000000ULL, "scvtf_w_d(-1)");
    CHECK(dbits(scvtf_w_d(42))  == 0x4045000000000000ULL, "scvtf_w_d(42)");

    CHECK(fbits(scvtf_x(-1))    == 0xBF800000ULL, "scvtf_x(-1)");
    CHECK(fbits(scvtf_x(42))    == 0x42280000ULL, "scvtf_x(42)");

    CHECK(dbits(scvtf_x_d(-1))  == 0xBFF0000000000000ULL, "scvtf_x_d(-1)");
    CHECK(dbits(scvtf_x_d(42))  == 0x4045000000000000ULL, "scvtf_x_d(42)");

    /* ── UCVTF (unsigned) ───────────────────────────────────────── */
    CHECK(fbits(ucvtf_w(0u))         == 0x00000000ULL, "ucvtf_w(0)");
    CHECK(fbits(ucvtf_w(1u))         == 0x3F800000ULL, "ucvtf_w(1)");
    CHECK(fbits(ucvtf_w(0xFFFFFFFFu))== 0x4F800000ULL, "ucvtf_w(UINT32_MAX)");

    CHECK(dbits(ucvtf_w_d(0xFFFFFFFFu)) == 0x41EFFFFFFFE00000ULL, "ucvtf_w_d(UINT32_MAX)");

    CHECK(fbits(ucvtf_x(0xFFFFFFFFFFFFFFFFULL)) == 0x5F800000ULL, "ucvtf_x(UINT64_MAX)");

    CHECK(dbits(ucvtf_x_d(0xFFFFFFFFFFFFFFFFULL)) == 0x43F0000000000000ULL, "ucvtf_x_d(UINT64_MAX)");

    /* ── FCVTZS (FP → signed, toward zero) ─────────────────────── */
    CHECK(fcvtzs_w_s(3.14f)   == 3,  "fcvtzs_w_s(3.14)");
    CHECK(fcvtzs_w_s(-3.14f)  == -3, "fcvtzs_w_s(-3.14)");
    CHECK(fcvtzs_w_s(0.0f)    == 0,  "fcvtzs_w_s(0)");
    CHECK(fcvtzs_w_s(42.99f)  == 42, "fcvtzs_w_s(42.99)");
    CHECK(fcvtzs_w_s(-42.99f) == -42,"fcvtzs_w_s(-42.99)");

    CHECK(fcvtzs_x_s(3.14f)   == 3,  "fcvtzs_x_s(3.14)");
    CHECK(fcvtzs_x_s(-3.14f)  == -3, "fcvtzs_x_s(-3.14)");

    CHECK(fcvtzs_w_d(3.14)    == 3,  "fcvtzs_w_d(3.14)");
    CHECK(fcvtzs_w_d(-3.14)   == -3, "fcvtzs_w_d(-3.14)");

    CHECK(fcvtzs_x_d(3.14)    == 3,  "fcvtzs_x_d(3.14)");
    CHECK(fcvtzs_x_d(-3.14)   == -3, "fcvtzs_x_d(-3.14)");
    CHECK(fcvtzs_x_d(1e10)    == 10000000000LL, "fcvtzs_x_d(1e10)");

    /* ── FCVTZU (FP → unsigned, toward zero) ───────────────────── */
    CHECK(fcvtzu_w_s(3.14f)   == 3u,  "fcvtzu_w_s(3.14)");
    CHECK(fcvtzu_w_s(0.0f)    == 0u,  "fcvtzu_w_s(0)");
    CHECK(fcvtzu_w_s(42.99f)  == 42u, "fcvtzu_w_s(42.99)");

    CHECK(fcvtzu_x_s(3.14f)   == 3u,  "fcvtzu_x_s(3.14)");

    CHECK(fcvtzu_w_d(3.14)    == 3u,  "fcvtzu_w_d(3.14)");

    CHECK(fcvtzu_x_d(3.14)    == 3u,  "fcvtzu_x_d(3.14)");
    CHECK(fcvtzu_x_d(1e10)    == 10000000000ULL, "fcvtzu_x_d(1e10)");
    CHECK(fcvtzu_x_d(1e19)    == 10000000000000000000ULL, "fcvtzu_x_d(1e19)");

    /* Every case in each lane; the loop warms the block into the JIT. */
    enum { NCASES = sizeof(ucvtf_cases) / sizeof(ucvtf_cases[0]) };
    int vector_ok = 1;
    for (int repeat = 0; repeat < 64; repeat++)
        for (int c = 0; c < NCASES; c++) {
            uint32_t in[4], out[4];
            for (int lane = 0; lane < 4; lane++) in[lane] = ucvtf_cases[(c + lane) % NCASES][0];
            ucvtf_4s(in, out);
            for (int lane = 0; lane < 4; lane++)
                if (out[lane] != ucvtf_cases[(c + lane) % NCASES][1]) {
                    if (vector_ok) printf("ucvtf 4s %08x -> %08x\n", in[lane], out[lane]);
                    vector_ok = 0;
                }
            ucvtf_2s(in, out);
            for (int lane = 0; lane < 2; lane++)
                if (out[lane] != ucvtf_cases[(c + lane) % NCASES][1]) {
                    if (vector_ok) printf("ucvtf 2s %08x -> %08x\n", in[lane], out[lane]);
                    vector_ok = 0;
                }
            if (out[2] || out[3]) {  // a 64-bit vector result clears the upper half
                if (vector_ok) printf("ucvtf 2s upper %08x %08x\n", out[2], out[3]);
                vector_ok = 0;
            }
        }
    CHECK(vector_ok, "ucvtf v.4s/v.2s ties");

    /* ── Summary ────────────────────────────────────────────────── */
    if (fails == 0) printf("\nALL TESTS PASSED\n");
    else            printf("\n%d TESTS FAILED\n", fails);
    return fails ? 1 : 0;
}
