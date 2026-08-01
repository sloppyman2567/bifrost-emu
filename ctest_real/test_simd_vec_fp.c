/* test_simd_vec_fp.c — SIMD vector FP 2-source tests.
 *
 * Verifies the vector (0x0E/0x2E group) forms of FADD/FSUB/FMUL/FDIV/
 * FMAX/FMIN/FABD/FMULX operate correctly on all lanes, in both the
 * single-precision (.4s/.2s) and double-precision (.2d) forms. These
 * are used by NEON-vectorized code (libc, image/audio processing).
 *
 * Build: make cross SRC=ctest_real/test_simd_vec_fp.c OUT=ctest_real/test_simd_vec_fp.elf
 * Run:   ./bifrost-emu ctest_real/test_simd_vec_fp.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* Aligned buffers for ld1/st1. */
static float va[4] __attribute__((aligned(16)));
static float vb[4] __attribute__((aligned(16)));
static float vr[4] __attribute__((aligned(16)));

#define RUN(insn, an, bn, exp0, exp1) do { \
    memcpy(va, an, 16); memcpy(vb, bn, 16); \
    register float* rp asm("x3") = vr; \
    asm volatile("ld1 {v0.4s}, [%[a]]\n" \
                 "ld1 {v2.4s}, [%[b]]\n" \
                 insn " v0.4s, v0.4s, v2.4s\n" \
                 "st1 {v0.4s}, [%[r]]" \
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp) \
                 : "v0","v2","memory"); \
    printf("%-6s: [%.1f, %.1f] (want [%.1f, %.1f])\n", insn, vr[0], vr[1], (float)(exp0), (float)(exp1)); \
    CK(vr[0]==(float)(exp0) && vr[1]==(float)(exp1), insn " v.4s correct"); \
} while(0)

int main(void) {
    float a4[4] = {2.0f, 4.0f, 6.0f, 8.0f};
    float b4[4] = {1.0f, 8.0f, 2.0f, 3.0f};
    RUN("fadd", a4, b4, 3.0f, 12.0f);
    RUN("fsub", a4, b4, 1.0f, -4.0f);
    RUN("fmul", a4, b4, 2.0f, 32.0f);
    RUN("fmax", a4, b4, 2.0f, 8.0f);
    RUN("fmin", a4, b4, 1.0f, 4.0f);
    RUN("fabd", a4, b4, 1.0f, 4.0f);
    /* fdiv */
    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp2 asm("x3") = vr;
    asm volatile("ld1 {v0.4s}, [%[a]]\n ld1 {v2.4s}, [%[b]]\n"
                 "fdiv v0.4s, v0.4s, v2.4s\n st1 {v0.4s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp2) : "v0","v2","memory");
    printf("fdiv:  [%.1f, %.1f] (want [2.0, 0.5])\n", vr[0], vr[1]);
    CK(vr[0]==2.0f && vr[1]==0.5f, "fdiv v.4s correct");

    /* fmaxnm / fminnm / fmulx (v.4s). */
    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp3 asm("x3") = vr;
    asm volatile("ld1 {v0.4s}, [%[a]]\n ld1 {v2.4s}, [%[b]]\n"
                 "fmaxnm v0.4s, v0.4s, v2.4s\n st1 {v0.4s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp3) : "v0","v2","memory");
    printf("fmaxnm:[%.1f, %.1f] (want [2.0, 8.0])\n", vr[0], vr[1]);
    CK(vr[0]==2.0f && vr[1]==8.0f, "fmaxnm v.4s correct");

    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp4 asm("x3") = vr;
    asm volatile("ld1 {v0.4s}, [%[a]]\n ld1 {v2.4s}, [%[b]]\n"
                 "fminnm v0.4s, v0.4s, v2.4s\n st1 {v0.4s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp4) : "v0","v2","memory");
    printf("fminnm:[%.1f, %.1f] (want [1.0, 4.0])\n", vr[0], vr[1]);
    CK(vr[0]==1.0f && vr[1]==4.0f, "fminnm v.4s correct");

    /* .2s (Q=0) variant — ensure 2-lane single works. */
    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp5 asm("x3") = vr;
    asm volatile("ld1 {v0.2s}, [%[a]]\n ld1 {v2.2s}, [%[b]]\n"
                 "fadd v0.2s, v0.2s, v2.2s\n st1 {v0.2s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp5) : "v0","v2","memory");
    printf("fadd.2s:[%.1f, %.1f] (want [3.0, 12.0])\n", vr[0], vr[1]);
    CK(vr[0]==3.0f && vr[1]==12.0f, "fadd v.2s correct");

    /* fmulx v.4s — multiply-extended; for normal operands equals fmul. */
    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp6 asm("x3") = vr;
    asm volatile("ld1 {v0.4s}, [%[a]]\n ld1 {v2.4s}, [%[b]]\n"
                 "fmulx v0.4s, v0.4s, v2.4s\n st1 {v0.4s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp6) : "v0","v2","memory");
    printf("fmulx.4s:[%.1f, %.1f] (want [2.0, 32.0])\n", vr[0], vr[1]);
    CK(vr[0]==2.0f && vr[1]==32.0f, "fmulx v.4s correct");

    /* fmulx v.2s (Q=0). */
    memcpy(va, a4, 16); memcpy(vb, b4, 16);
    register float* rp7 asm("x3") = vr;
    asm volatile("ld1 {v0.2s}, [%[a]]\n ld1 {v2.2s}, [%[b]]\n"
                 "fmulx v0.2s, v0.2s, v2.2s\n st1 {v0.2s}, [%[r]]"
                 : : [a] "r"(va), [b] "r"(vb), [r] "r"(rp7) : "v0","v2","memory");
    printf("fmulx.2s:[%.1f, %.1f] (want [2.0, 32.0])\n", vr[0], vr[1]);
    CK(vr[0]==2.0f && vr[1]==32.0f, "fmulx v.2s correct");

    /* Double-precision .2d variants. */
    static double da[2] __attribute__((aligned(16)));
    static double db[2] __attribute__((aligned(16)));
    static double dr[2] __attribute__((aligned(16)));
    double a2[2] = {2.0, 4.0};
    double b2[2] = {1.0, 8.0};
    memcpy(da, a2, 16); memcpy(db, b2, 16);
    register double* rp8 asm("x3") = dr;
    asm volatile("ld1 {v0.2d}, [%[a]]\n ld1 {v2.2d}, [%[b]]\n"
                 "fadd v0.2d, v0.2d, v2.2d\n st1 {v0.2d}, [%[r]]"
                 : : [a] "r"(da), [b] "r"(db), [r] "r"(rp8) : "v0","v2","memory");
    CK(dr[0]==3.0 && dr[1]==12.0, "fadd v.2d correct");

    memcpy(da, a2, 16); memcpy(db, b2, 16);
    register double* rp9 asm("x3") = dr;
    asm volatile("ld1 {v0.2d}, [%[a]]\n ld1 {v2.2d}, [%[b]]\n"
                 "fsub v0.2d, v0.2d, v2.2d\n st1 {v0.2d}, [%[r]]"
                 : : [a] "r"(da), [b] "r"(db), [r] "r"(rp9) : "v0","v2","memory");
    CK(dr[0]==1.0 && dr[1]==-4.0, "fsub v.2d correct");

    memcpy(da, a2, 16); memcpy(db, b2, 16);
    register double* rp10 asm("x3") = dr;
    asm volatile("ld1 {v0.2d}, [%[a]]\n ld1 {v2.2d}, [%[b]]\n"
                 "fmul v0.2d, v0.2d, v2.2d\n st1 {v0.2d}, [%[r]]"
                 : : [a] "r"(da), [b] "r"(db), [r] "r"(rp10) : "v0","v2","memory");
    CK(dr[0]==2.0 && dr[1]==32.0, "fmul v.2d correct");

    memcpy(da, a2, 16); memcpy(db, b2, 16);
    register double* rp11 asm("x3") = dr;
    asm volatile("ld1 {v0.2d}, [%[a]]\n ld1 {v2.2d}, [%[b]]\n"
                 "fdiv v0.2d, v0.2d, v2.2d\n st1 {v0.2d}, [%[r]]"
                 : : [a] "r"(da), [b] "r"(db), [r] "r"(rp11) : "v0","v2","memory");
    CK(dr[0]==2.0 && dr[1]==0.5, "fdiv v.2d correct");

    memcpy(da, a2, 16); memcpy(db, b2, 16);
    register double* rp12 asm("x3") = dr;
    asm volatile("ld1 {v0.2d}, [%[a]]\n ld1 {v2.2d}, [%[b]]\n"
                 "fmulx v0.2d, v0.2d, v2.2d\n st1 {v0.2d}, [%[r]]"
                 : : [a] "r"(da), [b] "r"(db), [r] "r"(rp12) : "v0","v2","memory");
    CK(dr[0]==2.0 && dr[1]==32.0, "fmulx v.2d correct");

    if (failures) { printf("test_simd_vec_fp: FAIL (%d)\n", failures); return 1; }
    printf("test_simd_vec_fp: ALL PASS\n");
    return 0;
}
