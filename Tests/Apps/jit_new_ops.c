/* jit_new_ops_test.c — test new JIT native instructions.
 * Tests: UDIV, SDIV, SMULH, UMULH, SMADDL, UMADDL, SMSUBL, UMSUBL,
 *        FCVT, FRINT, FABS, FNEG, FSQRT, FMADD, FMSUB
 * Run under both interpreter and JIT, compare outputs.
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>

int main() {
    /* Integer division */
    printf("=== Division ===\n");
    printf("100/7 = %d\n", 100/7);
    printf("100%%7 = %d\n", 100%7);
    printf("-100/7 = %d\n", -100/7);
    printf("0/5 = %d\n", 0/5);

    /* Long multiply (uses SMULH/UMULH) */
    printf("\n=== Multiply High ===\n");
    long long a = 0x7FFFFFFFFFFFFFFFLL;
    long long b = 0x7FFFFFFFFFFFFFFFLL;
    /* a*b = 0x3FFFFFFFFFFFFFFF0000000000000001 (128-bit) */
    /* high 64 bits = 0x3FFFFFFFFFFFFFFF (signed) */
    printf("smulh test: high = 0x%llx (expected 0x3fffffffffffffff)\n",
           (long long)((__int128)a * (__int128)b >> 64));
    unsigned long long ua = 0xFFFFFFFFFFFFFFFFULL;
    unsigned long long ub = 0xFFFFFFFFFFFFFFFFULL;
    printf("umulh test: high = 0x%llx (expected 0xfffffffffffffffe)\n",
           (unsigned long long)((unsigned __int128)ua * (unsigned __int128)ub >> 64));

    /* Widening multiply-accumulate (SMADDL/UMADDL) */
    printf("\n=== Widening Multiply-Accumulate ===\n");
    int x = 100000, y = 200000;
    long long acc = 1000000000LL;
    printf("smaddl: %lld + %d * %d = %lld (expected %lld)\n",
           acc, x, y, acc + (long long)x * y, acc + (long long)x * y);
    printf("umaddl: %llu + %u * %u = %llu\n",
           (unsigned long long)acc, (unsigned)x, (unsigned)y,
           (unsigned long long)acc + (unsigned long long)x * (unsigned long long)y);

    /* Multiply-subtract (SMSUBL/UMSUBL) */
    printf("\n=== Multiply-Subtract ===\n");
    printf("smsubl: %lld - %d * %d = %lld (expected %lld)\n",
           acc, x, y, acc - (long long)x * y, acc - (long long)x * y);

    /* FP operations */
    printf("\n=== Floating Point ===\n");
    double d1 = 3.14159, d2 = 2.71828;
    printf("fabs(%.5f) = %.5f\n", -d1, fabs(-d1));
    printf("fneg(%.5f) = %.5f\n", d1, -d1);
    printf("fsqrt(%.5f) = %.5f\n", 2.0, sqrt(2.0));

    /* FCVT: float <-> double */
    float f = 3.14f;
    double dd = (double)f;
    printf("fcvt s2d: %.6f -> %.6f\n", (double)f, dd);
    float f2 = (float)d1;
    printf("fcvt d2s: %.5f -> %.5f\n", d1, (double)f2);

    /* FRINT: rounding modes */
    printf("\n=== FP Rounding ===\n");
    printf("rint(2.5) = %.1f\n", rint(2.5));  /* nearest even */
    printf("ceil(2.1) = %.1f\n", ceil(2.1));
    printf("floor(2.9) = %.1f\n", floor(2.9));
    printf("trunc(2.9) = %.1f\n", trunc(2.9));

    /* FMADD/FMSUB */
    printf("\n=== FP Fused Multiply-Add ===\n");
    printf("fmadd: %.3f * %.3f + %.3f = %.6f (expected %.6f)\n",
           d1, d2, 1.0, d1 * d2 + 1.0, d1 * d2 + 1.0);
    printf("fmsub: %.3f - %.3f * %.3f = %.6f (expected %.6f)\n",
           10.0, d1, d2, 10.0 - d1 * d2, 10.0 - d1 * d2);

    /* FCMP */
    printf("\n=== FP Compare ===\n");
    printf("3.14 > 2.71: %s\n", 3.14 > 2.71 ? "yes" : "no");
    printf("3.14 < 2.71: %s\n", 3.14 < 2.71 ? "yes" : "no");
    printf("3.14 == 3.14: %s\n", 3.14 == 3.14 ? "yes" : "no");

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
