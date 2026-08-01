/* test_fabd.c — FABD (Floating-point Absolute Difference) test.
 *
 * FABD (fabd Sd, Sn, Sm) computes |Sn - Sm|. musl's fabsf(got - want)
 * is lowered to `fabd` by the compiler, so a broken FABD breaks float
 * comparisons silently (returns the first operand instead of |a-b|).
 *
 * This test forces FABD via inline asm and checks both single and
 * double precision, including the equal-operand (|a-a|=0) and
 * negative-difference (sign must be cleared) cases.
 *
 * Build: make cross SRC=ctest_real/test_fabd.c OUT=ctest_real/test_fabd.elf
 * Run:   ./bifrost-emu ctest_real/test_fabd.elf
 * Pass:  exit 0.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static uint32_t fb32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t fb64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* Helper that forces FABD (double) — the compiler lowers |a-b| to fabd. */
double fabd_d(double a, double b) {
    return fabs(a - b);
}

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* ── Single-precision FABD ────────────────────────────────────── */
    float s0 = 0.1f, s1 = 0.1f, sr;
    asm volatile("fabd %s0, %s1, %s2" : "=w"(sr) : "w"(s0), "w"(s1));
    printf("fabd.s(0.1,0.1)=0x%08x (want 0x00000000)\n", fb32(sr));
    CK(fb32(sr) == 0x00000000u, "FABD.s equal operands → +0.0f");

    float s2 = 0.1f, s3 = 0.3f, sr2;
    asm volatile("fabd %s0, %s1, %s2" : "=w"(sr2) : "w"(s2), "w"(s3));
    float expect_s = fabsf(0.1f - 0.3f);
    printf("fabd.s(0.1,0.3)=0x%08x (want 0x%08x)\n", fb32(sr2), fb32(expect_s));
    CK(fb32(sr2) == fb32(expect_s), "FABD.s |0.1-0.3| == fabsf result");
    CK((fb32(sr2) & 0x80000000u) == 0, "FABD.s result sign bit cleared");

    /* negative first operand */
    float s4 = -2.5f, s5 = 1.0f, sr3;
    asm volatile("fabd %s0, %s1, %s2" : "=w"(sr3) : "w"(s4), "w"(s5));
    float expect_s2 = fabsf(-2.5f - 1.0f);
    printf("fabd.s(-2.5,1.0)=0x%08x (want 0x%08x)\n", fb32(sr3), fb32(expect_s2));
    CK(fb32(sr3) == fb32(expect_s2), "FABD.s |-2.5-1.0| == 3.5");

    /* ── Double-precision FABD ────────────────────────────────────── */
    /* fabd via a helper so the compiler loads both operands from
     * memory cleanly (no asm constraint wrestling). */
    extern double fabd_d(double a, double b);
    double dr = fabd_d(0.1, 0.3);
    double expect_d = fabs(0.1 - 0.3);
    printf("fabd.d(0.1,0.3)=0x%016llx (want 0x%016llx)\n",
           (unsigned long long)fb64(dr), (unsigned long long)fb64(expect_d));
    CK(fb64(dr) == fb64(expect_d), "FABD.d |0.1-0.3| == fabs result");
    CK((fb64(dr) & 0x8000000000000000ULL) == 0, "FABD.d result sign bit cleared");

    if (failures) {
        printf("test_fabd: FAIL (%d checks failed)\n", failures);
        return 1;
    }
    printf("test_fabd: ALL PASS\n");
    return 0;
}
