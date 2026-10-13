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

/* Separate seed, scalar and observation blocks so the scalar block can
 * cache FP registers. Fourteen live registers also exercise partial pinning
 * and high host XMM registers. FMOV must preserve every low-lane bit (even
 * sNaNs) while clearing the other three lanes and retaining NZCV. */
__attribute__((noinline)) static int scalar_writeback(uint32_t bits) {
    uint32_t lanes[14][4];
    unsigned zero;
#define SEED(n) "movi v" #n ".16b, #0xa5\n"
#define COPY(n) "fmov s" #n ", s31\n"
#define SAVE(n, off) "str q" #n ", [%[out], #" #off "]\n"
    __asm__ volatile(
        SEED(0) SEED(1) SEED(2) SEED(3) SEED(4) SEED(5) SEED(6)
        SEED(7) SEED(8) SEED(9) SEED(10) SEED(11) SEED(12)
        "dup v31.4s, %w[bits]\n"
        "cmp %w[bits], #0\n"
        "b 1f\n1:\n"
        COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5) COPY(6)
        COPY(7) COPY(8) COPY(9) COPY(10) COPY(11) COPY(12)
        "fmov s0, s0\n"
        "b 2f\n2:\n"
        "cset %w[zero], eq\n"
        SAVE(0, 0) SAVE(1, 16) SAVE(2, 32) SAVE(3, 48) SAVE(4, 64)
        SAVE(5, 80) SAVE(6, 96) SAVE(7, 112) SAVE(8, 128) SAVE(9, 144)
        SAVE(10, 160) SAVE(11, 176) SAVE(12, 192) SAVE(31, 208)
        : [zero] "=&r"(zero)
        : [bits] "r"(bits), [out] "r"(lanes)
        : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v8", "v9", "v10", "v11", "v12", "v31", "cc", "memory");
#undef SEED
#undef COPY
#undef SAVE
    if (zero != (bits == 0)) return 0;
    for (unsigned r = 0; r < 14; ++r)
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t want = r == 13 || lane == 0 ? bits : 0;
            if (lanes[r][lane] != want) {
                printf("scalar writeback r=%u lane=%u got=%08x want=%08x\n",
                       r, lane, lanes[r][lane], want);
                return 0;
            }
        }
    return 1;
}

/* The ordinary FP hot path must retain live integer temporaries. NaN and
 * negative-square-root cases take the cold interpreter path, whose saved
 * GPR mappings must survive as well. */
__attribute__((noinline)) static int scalar_live_gpr(uint64_t value, uint32_t bits) {
    uint64_t got;
    __asm__ volatile(
        "fmov s1, %w[bits]\n"
        "fmov s2, #1.0\n"
        "add x9, %[value], #37\n"
        "fadd s0, s1, s2\n"
        "fsqrt s3, s1\n"
        "fmov s4, s0\n"
        "add %[got], x9, #1\n"
        : [got] "=r"(got)
        : [value] "r"(value), [bits] "r"(bits)
        : "x9", "v0", "v1", "v2", "v3", "v4", "cc");
    return got == value + 38;
}

int main(void) {
    const uint32_t patterns[] = {0, 0x80000000u, 1, 0x007fffffu,
        0x3fc00000u, 0x7f800000u, 0xff800000u, 0x7fc12345u,
        0xffc12345u, 0x7f812345u, 0xff812345u, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i)
        CHECK(scalar_writeback(patterns[i]), "scalar_writeback_bits_and_upper_lanes");
    const uint64_t integers[] = {0, 1, 0x123456789abcdef0ull, ~0ull};
    for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i)
        for (unsigned j = 0; j < sizeof(integers) / sizeof(integers[0]); ++j)
            CHECK(scalar_live_gpr(integers[j], patterns[i]), "scalar_live_gpr_hot_and_cold");

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
