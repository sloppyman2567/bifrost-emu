// jit_fcmp_flags.c — NZCV after FCMP, FCMPE and FCCMP.
//
// ARM sets NZCV from an FP comparison as: less N, equal Z C, greater C, unordered C V. The JIT derives it from
// UCOMISS/UCOMISD's ZF/PF/CF. Every pair of {-inf, -2.5, -0, +0, 3, +inf, quiet NaN, signalling NaN} is compared
// in single and double precision, with a register operand and with #0.0, as FCMP and FCMPE, and as FCCMP with the
// condition both true (the compare runs) and false (the immediate NZCV is taken). The expected values come from the
// definition above, so the same binary also runs under QEMU.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(ok, ...) do { ++checks; if (!(ok)) { ++fails; if (fails < 30) { printf("NG " __VA_ARGS__); printf("\n"); } } } while (0)

static unsigned nzcv_of(double a, double b) {
    if (isnan(a) || isnan(b)) return 3;  // C V
    if (a < b) return 8;                 // N
    if (a == b) return 6;                // Z C
    return 2;                            // C
}

#define NZCV(n) (unsigned)(((n) >> 28) & 15)
static unsigned cmp_d(double a, double b) { uint64_t n; __asm__ volatile("fcmp %d1, %d2\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return NZCV(n); }
static unsigned cmpe_d(double a, double b) { uint64_t n; __asm__ volatile("fcmpe %d1, %d2\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return NZCV(n); }
static unsigned cmp0_d(double a) { uint64_t n; __asm__ volatile("fcmp %d1, #0.0\n mrs %0, nzcv" : "=r"(n) : "w"(a) : "cc"); return NZCV(n); }
static unsigned cmpe0_d(double a) { uint64_t n; __asm__ volatile("fcmpe %d1, #0.0\n mrs %0, nzcv" : "=r"(n) : "w"(a) : "cc"); return NZCV(n); }
static unsigned cmp_s(float a, float b) { uint64_t n; __asm__ volatile("fcmp %s1, %s2\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return NZCV(n); }
static unsigned cmpe_s(float a, float b) { uint64_t n; __asm__ volatile("fcmpe %s1, %s2\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return NZCV(n); }
static unsigned cmp0_s(float a) { uint64_t n; __asm__ volatile("fcmp %s1, #0.0\n mrs %0, nzcv" : "=r"(n) : "w"(a) : "cc"); return NZCV(n); }
// FCCMP: with c == 0 the EQ condition holds and the compare runs; otherwise NZCV becomes the immediate 0b0101.
static unsigned ccmp_d(double a, double b, int c) { uint64_t n; __asm__ volatile("cmp %w3, #0\n fccmp %d1, %d2, #5, eq\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b), "r"(c) : "cc"); return NZCV(n); }
static unsigned ccmp_s(float a, float b, int c) { uint64_t n; __asm__ volatile("cmp %w3, #0\n fccmp %s1, %s2, #5, eq\n mrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b), "r"(c) : "cc"); return NZCV(n); }
// A compare consumed by a branch in the same block: the common shape in game code.
static int branch_lt_d(double a, double b) { int r; __asm__ volatile("mov %w0, #0\n fcmp %d1, %d2\n b.pl 1f\n mov %w0, #1\n 1:" : "=&r"(r) : "w"(a), "w"(b) : "cc"); return r; }
static int branch_le_d(double a, double b) { int r; __asm__ volatile("mov %w0, #0\n fcmp %d1, %d2\n b.hi 1f\n mov %w0, #1\n 1:" : "=&r"(r) : "w"(a), "w"(b) : "cc"); return r; }
static int cset_gt_s(float a, float b) { int r; __asm__ volatile("fcmp %s1, %s2\n cset %w0, gt" : "=r"(r) : "w"(a), "w"(b) : "cc"); return r; }
static int cset_vs_s(float a, float b) { int r; __asm__ volatile("fcmp %s1, %s2\n cset %w0, vs" : "=r"(r) : "w"(a), "w"(b) : "cc"); return r; }

static double dval(int i) {
    static const double v[6] = {-INFINITY, -2.5, -0.0, 0.0, 3.0, INFINITY};
    if (i < 6) return v[i];
    uint64_t bits = i == 6 ? 0x7ff8000000000000ull : 0x7ff4000000000000ull;  // quiet and signalling NaN
    double d; memcpy(&d, &bits, 8); return d;
}
static float sval(int i) {
    static const float v[6] = {-INFINITY, -2.5f, -0.0f, 0.0f, 3.0f, INFINITY};
    if (i < 6) return v[i];
    uint32_t bits = i == 6 ? 0x7fc00000u : 0x7fa00000u;
    float f; memcpy(&f, &bits, 4); return f;
}

int main(void) {
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            for (int rep = 0; rep < 3; rep++) {  // repeat so the JIT's compiled form runs, not its first pass
                const double a = dval(i), b = dval(j);
                const float fa = sval(i), fb = sval(j);
                const unsigned want = nzcv_of(a, b);
                CHECK(cmp_d(a, b) == want, "fcmp d %d,%d: %x want %x", i, j, cmp_d(a, b), want);
                CHECK(cmpe_d(a, b) == want, "fcmpe d %d,%d: %x want %x", i, j, cmpe_d(a, b), want);
                CHECK(cmp_s(fa, fb) == want, "fcmp s %d,%d: %x want %x", i, j, cmp_s(fa, fb), want);
                CHECK(cmpe_s(fa, fb) == want, "fcmpe s %d,%d: %x want %x", i, j, cmpe_s(fa, fb), want);
                CHECK(ccmp_d(a, b, 0) == want, "fccmp d taken %d,%d: %x want %x", i, j, ccmp_d(a, b, 0), want);
                CHECK(ccmp_d(a, b, 1) == 5, "fccmp d skipped %d,%d: %x", i, j, ccmp_d(a, b, 1));
                CHECK(ccmp_s(fa, fb, 0) == want, "fccmp s taken %d,%d: %x want %x", i, j, ccmp_s(fa, fb, 0), want);
                CHECK(ccmp_s(fa, fb, 1) == 5, "fccmp s skipped %d,%d: %x", i, j, ccmp_s(fa, fb, 1));
                // b.pl is taken unless N is set: only an ordered a < b falls through. b.hi is taken for C=1 and Z=0.
                CHECK(branch_lt_d(a, b) == (nzcv_of(a, b) == 8), "b.pl %d,%d", i, j);
                CHECK(branch_le_d(a, b) == !(nzcv_of(a, b) == 2 || nzcv_of(a, b) == 3), "b.hi %d,%d", i, j);
                CHECK(cset_gt_s(fa, fb) == (nzcv_of(a, b) == 2), "cset gt %d,%d", i, j);
                CHECK(cset_vs_s(fa, fb) == (nzcv_of(a, b) == 3), "cset vs %d,%d", i, j);
            }
    for (int i = 0; i < 8; i++) {
        const double a = dval(i);
        const unsigned want = nzcv_of(a, 0.0);
        CHECK(cmp0_d(a) == want, "fcmp d,#0 %d: %x want %x", i, cmp0_d(a), want);
        CHECK(cmpe0_d(a) == want, "fcmpe d,#0 %d: %x want %x", i, cmpe0_d(a), want);
        CHECK(cmp0_s(sval(i)) == want, "fcmp s,#0 %d: %x want %x", i, cmp0_s(sval(i)), want);
    }
    if (fails) { printf("jit_fcmp_flags: FAILED (%d of %d)\n", fails, checks); return 1; }
    printf("jit_fcmp_flags: ALL PASS (%d checks)\n", checks);
    return 0;
}
