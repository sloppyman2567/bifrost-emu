/* jit_fp_pw_elem.c — scalar-pairwise FP (FADDP/FMAXP/FMINP) and
 * scalar-x-indexed-element (FMLA/FMLS/FMUL/FMULX) regression test.
 *
 * Both families were silently NOP'd by the interpreter's [FP-NOP]
 * fallback: compiler-rt's vectorized sin() emits `faddp d4,v4.2d`
 * and `fmul d4,d2,v3.d[1]`, corrupting sin() results for any guest
 * built with it (rudolf-cart audio was garbage).
 *
 * Encodings are emitted as .word because the assembler in some musl
 * cross toolchains rejects these mnemonics in inline asm.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;

static void check(const char *name, double got, double want) {
    int ok = got == want;
    printf("%-24s got %.17g want %.17g %s\n", name, got, want,
           ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* ---- scalar pairwise: op writes d19 from v18.2d / v18.2s ---- */
#define PW_FN(name, WORD)                                         \
static uint64_t name(const void *xy) {                            \
    uint64_t obits = 0;                                           \
    register const void *p __asm__("x0") = xy;                    \
    register void *o __asm__("x1") = &obits;                      \
    __asm__ volatile("ldr q18,[x0]\n\t.word " #WORD               \
                     "\n\tstr d19,[x1]"                           \
        : : "r"(p), "r"(o) : "memory", "q18", "d19");             \
    return obits;                                                 \
}
PW_FN(pw_faddp_d, 0x7E70DA53)
PW_FN(pw_faddp_s, 0x7E30DA53)
PW_FN(pw_fmaxp_d, 0x7E70FA53)
PW_FN(pw_fminp_d, 0x7EF0FA53)
PW_FN(pw_fmaxp_s, 0x7E30FA53)
PW_FN(pw_fminp_s, 0x7EB0FA53)

static void test_pairwise(void) {
    double dd[2], r;
    float ff[2], rf;
    dd[0] = 1.5;  dd[1] = 2.25;
    memcpy(&r, &(uint64_t){pw_faddp_d(dd)}, 8); check("faddp d(1.5,2.25)", r, 3.75);
    ff[0] = 1.5f; ff[1] = 2.25f;
    memcpy(&rf, &(uint64_t){pw_faddp_s(ff)}, 4); check("faddp s(1.5,2.25)", rf, 3.75);
    dd[0] = 10.5; dd[1] = -3.25;
    memcpy(&r, &(uint64_t){pw_fmaxp_d(dd)}, 8); check("fmaxp d(10.5,-3.25)", r, 10.5);
    memcpy(&r, &(uint64_t){pw_fminp_d(dd)}, 8); check("fminp d(10.5,-3.25)", r, -3.25);
    ff[0] = 10.5f; ff[1] = -3.25f;
    memcpy(&rf, &(uint64_t){pw_fmaxp_s(ff)}, 4); check("fmaxp s(10.5,-3.25)", rf, 10.5);
    memcpy(&rf, &(uint64_t){pw_fminp_s(ff)}, 4); check("fminp s(10.5,-3.25)", rf, -3.25);
    dd[0] = -5.0; dd[1] = -2.0;
    memcpy(&r, &(uint64_t){pw_fminp_d(dd)}, 8); check("fminp d(-5,-2)", r, -5.0);
    /* NaN rules: one-NaN -> other operand */
    dd[0] = 5.0; dd[1] = __builtin_nan("");
    memcpy(&r, &(uint64_t){pw_fmaxp_d(dd)}, 8); check("fmaxp d(5,NaN)", r, 5.0);
    memcpy(&r, &(uint64_t){pw_fminp_d(dd)}, 8); check("fminp d(5,NaN)", r, 5.0);
}

/* ---- scalar x indexed element: rm=v18(=10010b), rn=v2, rd=v4 ---- */
#define EL_FN(name, WORD)                                             \
static uint64_t name(const void *m) {                                 \
    uint64_t obits = 0;                                               \
    register const void *p __asm__("x0") = m;                         \
    register void *o __asm__("x1") = &obits;                          \
    __asm__ volatile(                                                 \
        "ldr q18,[x0]\n\t"      /* v18 = vector lanes          */     \
        "ldr d19,[x0,#16]\n\t"  /* scalar operand              */     \
        "ldr d20,[x0,#24]\n\t"  /* accumulator for fmla/fmls   */     \
        "mov v2.16b, v19.16b\n\t"                                       \
        "mov v4.16b, v20.16b\n\t"                                       \
        ".word " #WORD "\n\t"                                           \
        "str d4,[x1]"                                                   \
        : : "r"(p), "r"(o) : "memory", "q18", "d19", "d20", "v2", "v4");\
    return obits;                                                     \
}
EL_FN(el_fmul_d0, 0x5FD29044)
EL_FN(el_fmul_d1, 0x5FD29844)
EL_FN(el_fmla_d1, 0x5FD21844)
EL_FN(el_fmls_d1, 0x5FD25844)
EL_FN(el_fmulx_d1, 0x7FD29844)
EL_FN(el_fmul_s0, 0x5F929044)
EL_FN(el_fmul_s3, 0x5FB29844)
EL_FN(el_fmla_s2, 0x5F921844)

static void test_elem(void) {
    double m[4];
    double lanes[2] = {3.0, 5.0}, scalar = 7.0, acc = 100.0;
    memcpy(m, lanes, 16); memcpy(m + 2, &scalar, 8); memcpy(m + 3, &acc, 8);
    double r;
    memcpy(&r, &(uint64_t){el_fmul_d0(m)}, 8);  check("fmul d,v.d[0]*7", r, 21.0);
    memcpy(&r, &(uint64_t){el_fmul_d1(m)}, 8);  check("fmul d,v.d[1]*7", r, 35.0);
    memcpy(&r, &(uint64_t){el_fmla_d1(m)}, 8);  check("fmla d acc+v.d[1]*7", r, 135.0);
    memcpy(&r, &(uint64_t){el_fmls_d1(m)}, 8);  check("fmls d acc-v.d[1]*7", r, 65.0);
    memcpy(&r, &(uint64_t){el_fmulx_d1(m)}, 8); check("fmulx d,v.d[1]*7", r, 35.0);

    float fl[4] = {2.0f, 3.0f, 4.0f, 6.0f};
    float sc = 10.0f, ac = 1000.0f;
    memcpy(m, fl, 16); memcpy(m + 2, &sc, 4); memcpy(m + 3, &ac, 4);
    float rf;
    memcpy(&rf, &(uint64_t){el_fmul_s0(m)}, 4); check("fmul s,v.s[0]*10", rf, 20.0);
    memcpy(&rf, &(uint64_t){el_fmul_s3(m)}, 4); check("fmul s,v.s[3]*10", rf, 60.0);
    memcpy(&rf, &(uint64_t){el_fmla_s2(m)}, 4); check("fmla s acc+v.s[2]*10", rf, 1040.0);
}

int main(void) {
    test_pairwise();
    test_elem();
    if (fails) { printf("FAILURES: %d\n", fails); return 1; }
    printf("ALL PASS (%d checks)\n", 18);
    return 0;
}
