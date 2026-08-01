/* test_simd_fmla.c — SIMD vector FP fused 3-source (FMLA/FMLS) tests.
 *
 * Verifies the vector forms of FMLA (Vd = Vd + Vn*Vm) and FMLS
 * (Vd = Vd - Vn*Vm) accumulate correctly on all lanes, in both the
 * single-precision (.4s) and double-precision (.2d) forms. These are
 * the hot ops of NEON-vectorized matrix/convolution kernels (GCC
 * lowers a*b+c loops to fmla v.4s).
 *
 * Build: make cross SRC=ctest_real/test_simd_fmla.c OUT=ctest_real/test_simd_fmla.elf
 * Run:   ./bifrost-emu ctest_real/test_simd_fmla.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* FMLA v.4s: acc += a*b */
    {
        float acc[4] = { 0.5f, 1.5f, 2.5f, 3.5f };
        float a[4]   = { 1.0f, 2.0f, 3.0f, 4.0f };
        float b[4]   = { 10.0f, 20.0f, 30.0f, 40.0f };
        asm volatile(
            "ld1 {v0.4s}, [%[r]]\n"
            "ld1 {v2.4s}, [%[a]]\n"
            "ld1 {v3.4s}, [%[b]]\n"
            "fmla v0.4s, v2.4s, v3.4s\n"
            "st1 {v0.4s}, [%[r]]"
            : : [r]"r"(acc), [a]"r"(a), [b]"r"(b) : "v0","v2","v3","memory");
        CK(acc[0]==0.5f+10 && acc[1]==1.5f+40 && acc[2]==2.5f+90 && acc[3]==3.5f+160,
           "fmla v.4s accumulate correct");
    }
    /* FMLS v.4s: acc -= a*b */
    {
        float acc[4] = { 100.0f, 100.0f, 100.0f, 100.0f };
        float a[4]   = { 10.0f, 10.0f, 10.0f, 10.0f };
        float b[4]   = { 2.0f, 2.0f, 2.0f, 2.0f };
        asm volatile(
            "ld1 {v0.4s}, [%[r]]\n"
            "ld1 {v2.4s}, [%[a]]\n"
            "ld1 {v3.4s}, [%[b]]\n"
            "fmls v0.4s, v2.4s, v3.4s\n"
            "st1 {v0.4s}, [%[r]]"
            : : [r]"r"(acc), [a]"r"(a), [b]"r"(b) : "v0","v2","v3","memory");
        CK(acc[0]==80.0f && acc[1]==80.0f && acc[2]==80.0f && acc[3]==80.0f,
           "fmls v.4s subtract correct");
    }
    /* FMLA .2s (64-bit, Q=0): only low 2 lanes, high 2 zeroed */
    {
        float acc[4] = { 1.0f, 2.0f, 99.0f, 99.0f };
        float a[4]   = { 2.0f, 3.0f, 0.0f, 0.0f };
        float b[4]   = { 4.0f, 5.0f, 0.0f, 0.0f };
        asm volatile(
            "ld1 {v0.2s}, [%[r]]\n"
            "ld1 {v2.2s}, [%[a]]\n"
            "ld1 {v3.2s}, [%[b]]\n"
            "fmla v0.2s, v2.2s, v3.2s\n"
            "st1 {v0.2s}, [%[r]]"
            : : [r]"r"(acc), [a]"r"(a), [b]"r"(b) : "v0","v2","v3","memory");
        CK(acc[0]==1.0f+8 && acc[1]==2.0f+15 && acc[2]==99.0f && acc[3]==99.0f,
           "fmla v.2s 64-bit accumulate correct");
    }
    /* FMLA v.2d (double): acc += a*b */
    {
        double acc[2] = { 0.5, 1.5 };
        double a[2]   = { 2.0, 3.0 };
        double b[2]   = { 4.0, 5.0 };
        asm volatile(
            "ld1 {v0.2d}, [%[r]]\n"
            "ld1 {v2.2d}, [%[a]]\n"
            "ld1 {v3.2d}, [%[b]]\n"
            "fmla v0.2d, v2.2d, v3.2d\n"
            "st1 {v0.2d}, [%[r]]"
            : : [r]"r"(acc), [a]"r"(a), [b]"r"(b) : "v0","v2","v3","memory");
        CK(acc[0]==0.5+8 && acc[1]==1.5+15, "fmla v.2d double accumulate correct");
    }
    /* FMLS v.2d (double): acc -= a*b */
    {
        double acc[2] = { 100.0, 100.0 };
        double a[2]   = { 10.0, 10.0 };
        double b[2]   = { 2.0, 2.0 };
        asm volatile(
            "ld1 {v0.2d}, [%[r]]\n"
            "ld1 {v2.2d}, [%[a]]\n"
            "ld1 {v3.2d}, [%[b]]\n"
            "fmls v0.2d, v2.2d, v3.2d\n"
            "st1 {v0.2d}, [%[r]]"
            : : [r]"r"(acc), [a]"r"(a), [b]"r"(b) : "v0","v2","v3","memory");
        CK(acc[0]==80.0 && acc[1]==80.0, "fmls v.2d double subtract correct");
    }
    if (failures) { printf("test_simd_fmla: FAIL (%d)\n", failures); return 1; }
    printf("test_simd_fmla: ALL PASS\n");
    return 0;
}
