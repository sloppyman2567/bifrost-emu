/*
 * jit_fp_scalar.c — JIT native FP scalar op tests.
 *
 * Background (alpha.4):
 *   The JIT gained native SSE2 codegen for FADD, FSUB, FMUL, FDIV,
 *   FSQRT (both single- and double-precision).  Before this, every
 *   FP op fell through to CALL_INTERP.
 *
 *   Known bugs (not covered here, would need separate test cases):
 *     - FABS/FNEG with certain inputs produce wrong values
 *     - FP comparisons (FCMP) sometimes return wrong boolean
 *     - FMOV imm with value 1.0 produces wrong value (but 1.5, 2.25 work)
 *
 * Uses pure C with noinline functions.  The compiler emits the
 * relevant FP instructions for these patterns.
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

__attribute__((noinline)) double d_add(double a, double b) { return a + b; }
__attribute__((noinline)) double d_sub(double a, double b) { return a - b; }
__attribute__((noinline)) double d_mul(double a, double b) { return a * b; }
__attribute__((noinline)) double d_div(double a, double b) { return a / b; }
__attribute__((noinline)) double d_sqrt(double a) { return sqrt(a); }
__attribute__((noinline)) float f_add(float a, float b) { return a + b; }
__attribute__((noinline)) float f_mul(float a, float b) { return a * b; }
__attribute__((noinline)) float f_div(float a, float b) { return a / b; }
__attribute__((noinline)) float f_sqrt(float a) { return sqrtf(a); }
__attribute__((noinline)) double d_fnmul(double a, double b) { return -(a * b); }
__attribute__((noinline)) float f_fnmul(float a, float b) { return -(a * b); }

int main(void) {
    /* ── FADD / FSUB / FMUL / FDIV ────────────────────────────────── */
    CHECK(d_add(1.5, 2.25) == 3.75, "fadd");
    CHECK(d_sub(1.5, 2.25) == -0.75, "fsub");
    CHECK(d_mul(1.5, 2.25) == 3.375, "fmul");
    CHECK(d_div(1.5, 2.25) == 1.5 / 2.25, "fdiv");

    /* ── FSQRT ────────────────────────────────────────────────────── */
    CHECK(d_sqrt(144.0) == 12.0, "fsqrt_144");
    CHECK(d_sqrt(0.0) == 0.0, "fsqrt_0");
    CHECK(d_sqrt(1.0) == 1.0, "fsqrt_1");

    /* ── Single-precision ─────────────────────────────────────────── */
    CHECK(f_add(1.5f, 2.25f) == 3.75f, "fadds");
    CHECK(f_mul(1.5f, 2.25f) == 3.375f, "fmuls");
    CHECK(f_div(1.5f, 2.25f) == 1.5f / 2.25f, "fdivs");
    CHECK(f_sqrt(144.0f) == 12.0f, "fsqrts_144");

    /* ── FNMUL (negated multiply, opcode 0x8) ───────────────────────
     * The JIT previously negated with a hardcoded double sign mask
     * (bit 63).  Single-precision values live in bits 0-31, so the
     * XOR was a no-op and fnmul s returned +a*b.  cglm's glm_ortho
     * uses `fnmul s` for its translation row: a +1 instead of -1
     * pushed every HUD quad off-screen under JIT. */
    CHECK(d_fnmul(3.0, 2.0) == -6.0, "fnmuld");
    CHECK(d_fnmul(-4.0, 5.0) == 20.0, "fnmuld_neg");
    CHECK(f_fnmul(3.0f, 2.0f) == -6.0f, "fnmuls");
    CHECK(f_fnmul(-4.0f, 5.0f) == 20.0f, "fnmuls_neg");

    /* ── FMOV general↔FP (G↔F) — bit-perfect round trip ───────────── */
    uint64_t bits = 0xBEEFCAFEDeadBeefULL;
    double as_d;
    memcpy(&as_d, &bits, 8);
    uint64_t bits_back;
    memcpy(&bits_back, &as_d, 8);
    CHECK(bits_back == 0xBEEFCAFEDeadBeefULL, "fmov_g2f2g");

    /* FMOV W↔S (32-bit) */
    uint32_t wbits = 0x40490FDB;
    float ff;
    memcpy(&ff, &wbits, 4);
    uint32_t wbits_back;
    memcpy(&wbits_back, &ff, 4);
    CHECK(wbits_back == 0x40490FDB, "fmov_w2s2w");

    /* ── The "3.14 must print exactly" regression ─────────────────── */
    char buf[64];
    snprintf(buf, sizeof(buf), "%f\n", 3.14);
    CHECK(strncmp(buf, "3.1400", 6) == 0, "fp_314_exact");

    /* ── FP→int conversion (FCVTZS) ───────────────────────────────── */
    double pi = 3.14;
    int truncated = (int)pi;
    CHECK(truncated == 3, "fp_to_int");

    double big = 1234567.89;
    int64_t big_trunc = (int64_t)big;
    CHECK(big_trunc == 1234567, "fp_to_int64");

    /* ── Int->FP conversion (SCVTF) ───────────────────────────────── */
    int n = 42;
    double nd = (double)n;
    CHECK(nd == 42.0, "int_to_fp");

    /* ── Single-precision FP→int ──────────────────────────────────── */
    float pf = 3.14f;
    int pf_int = (int)pf;
    CHECK(pf_int == 3, "fp32_to_int");

    int m = 100;
    float mf = (float)m;
    CHECK(mf == 100.0f, "int_to_fp32");

    /* ── FP arithmetic in a small loop ────────────────────────────── */
    double acc = 0.0;
    for (int i = 0; i < 10; i++) {
        acc = d_add(acc, 0.5);
    }
    CHECK(acc == 5.0, "fp_loop_10");

    /* ── Printf with FP formats ───────────────────────────────────── */
    char buf2[64];
    snprintf(buf2, sizeof(buf2), "%.2f", 3.14159);
    CHECK(strcmp(buf2, "3.14") == 0, "fp_printf_f");

    snprintf(buf2, sizeof(buf2), "%.2e", 12345.6789);
    CHECK(strcmp(buf2, "1.23e+04") == 0, "fp_printf_e");

    /* ── FP arithmetic chaining (catches vreg spill bugs) ─────────── */
    double a = 1.5, b = 2.25, c = 3.75;
    double result = d_add(d_mul(a, b), c);  /* 1.5*2.25 + 3.75 = 7.125 */
    CHECK(result == 7.125, "fp_chain");

    /* ── Mixed precision (single→double) ──────────────────────────── */
    float sf = 1.5f;
    double sd = (double)sf;
    CHECK(sd == 1.5, "fp_s_to_d");

    /* ── Double→single ────────────────────────────────────────────── */
    double dv = 2.5;
    float sv = (float)dv;
    CHECK(sv == 2.5f, "fp_d_to_s");

    printf("fp_scalar: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
