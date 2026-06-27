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

    /* ── Summary ────────────────────────────────────────────────── */
    if (fails == 0) printf("\nALL TESTS PASSED\n");
    else            printf("\n%d TESTS FAILED\n", fails);
    return fails ? 1 : 0;
}
