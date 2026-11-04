/* jit_fp_pw_elem.c — scalar-pairwise FP (FADDP/FMAXP/FMINP) and
 * scalar-x-indexed-element (FMLA/FMLS/FMUL/FMULX) regression test.
 *
 * Both families were silently NOP'd by the interpreter's [FP-NOP]
 * fallback: compiler-rt's vectorized sin() emits `faddp d4,v4.2d`
 * and `fmul d4,d2,v3.d[1]`, corrupting sin() results for any guest
 * built with it (rudolf-cart audio was garbage).
 *
 * The first cases are emitted as .word because the assembler in some musl
 * cross toolchains rejected these mnemonics in inline asm; the sweep below
 * uses mnemonics, which the current toolchain accepts.
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
static void check_nan(const char *name, double got) {
    int ok = __builtin_isnan(got);
    printf("%-24s got %.17g want NaN %s\n", name, got,
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
    /* NaN rules: FMAXP/FMINP propagate like FMAX/FMIN (they are the
     * non-NM forms; FMAXNMP/FMINNMP are separate encodings). */
    dd[0] = 5.0; dd[1] = __builtin_nan("");
    memcpy(&r, &(uint64_t){pw_fmaxp_d(dd)}, 8); check_nan("fmaxp d(5,NaN)", r);
    memcpy(&r, &(uint64_t){pw_fminp_d(dd)}, 8); check_nan("fminp d(5,NaN)", r);
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

/* ---- every form and element against C references ----
 * The JIT runs these natively: FMUL/FMULX as a scalar multiply whose second
 * operand is Vm's element, FMLA/FMLS as a fused multiply-add. Vm is v17
 * (M=1); Vn and Vd carry garbage above their scalar, which the result must
 * clear. NaN results take the cold interpreter step and are checked as NaN. */
typedef struct { uint64_t lo, hi; } V128;
#define FORM(name, INSN) static V128 name(const V128 *m, const V128 *n, const V128 *d) { V128 r; \
    __asm__ volatile("ldr q17,[%1]\n\tldr q5,[%2]\n\tldr q6,[%3]\n\t" INSN "\n\tstr q6,[%0]" \
        : : "r"(&r), "r"(m), "r"(n), "r"(d) : "memory", "v5", "v6", "v17"); return r; }
#define FORMS_S(op) FORM(op##_s0, #op " s6, s5, v17.s[0]") FORM(op##_s1, #op " s6, s5, v17.s[1]") \
    FORM(op##_s2, #op " s6, s5, v17.s[2]") FORM(op##_s3, #op " s6, s5, v17.s[3]") \
    FORM(op##_d0, #op " d6, d5, v17.d[0]") FORM(op##_d1, #op " d6, d5, v17.d[1]")
FORMS_S(fmul) FORMS_S(fmulx) FORMS_S(fmla) FORMS_S(fmls)
/* Destination aliasing Vm: the element is read before Vd is written. */
static V128 alias_fmla_s3(const V128 *m, const V128 *n) { V128 r;
    __asm__ volatile("ldr q17,[%1]\n\tldr q5,[%2]\n\tfmla s17, s5, v17.s[3]\n\tstr q17,[%0]"
        : : "r"(&r), "r"(m), "r"(n) : "memory", "v5", "v17"); return r; }
typedef V128 (*FormFn)(const V128 *, const V128 *, const V128 *);
static const struct { FormFn fn; int kind, dbl, lane; } forms[] = {
#define ROW(op, k) {op##_s0, k, 0, 0}, {op##_s1, k, 0, 1}, {op##_s2, k, 0, 2}, {op##_s3, k, 0, 3}, \
    {op##_d0, k, 1, 0}, {op##_d1, k, 1, 1},
    ROW(fmul, 0) ROW(fmulx, 1) ROW(fmla, 2) ROW(fmls, 3)
};
static float ref_s(int kind, float a, float b, float c) {
    if (kind == 1 && ((a == 0 && __builtin_isinf(b)) || (__builtin_isinf(a) && b == 0)))
        return (__builtin_signbit(a) != 0) != (__builtin_signbit(b) != 0) ? -2.0f : 2.0f;
    return kind == 2 ? __builtin_fmaf(a, b, c) : kind == 3 ? __builtin_fmaf(-a, b, c) : a * b;
}
static double ref_d(int kind, double a, double b, double c) {
    if (kind == 1 && ((a == 0 && __builtin_isinf(b)) || (__builtin_isinf(a) && b == 0)))
        return (__builtin_signbit(a) != 0) != (__builtin_signbit(b) != 0) ? -2.0 : 2.0;
    return kind == 2 ? __builtin_fma(a, b, c) : kind == 3 ? __builtin_fma(-a, b, c) : a * b;
}
static int sweep_bad;
static void sweep_fail(const char *what, int f, int i, int j, int k, uint64_t got_lo, uint64_t got_hi) {
    if (++sweep_bad <= 8)
        printf("NG %s form %d (kind %d dbl %d lane %d) values %d %d %d: %016llx:%016llx\n", what, f,
               forms[f].kind, forms[f].dbl, forms[f].lane, i, j, k, (unsigned long long)got_hi,
               (unsigned long long)got_lo);
}
static void test_elem_sweep(void) {
    const double vals[] = {1.5, -2.25, 0.0, -0.0, __builtin_inf(), -__builtin_inf(), __builtin_nan(""),
                           1e-40, 3.0e38, 1.0000001, 7.0, -1e-310};
    const int nv = (int)(sizeof vals / sizeof vals[0]);
    for (int f = 0; f < (int)(sizeof forms / sizeof forms[0]); f++)
        for (int i = 0; i < nv; i++)
            for (int j = 0; j < nv; j++)
                for (int k = 0; k < nv; k++) {
                    const int dbl = forms[f].dbl, lane = forms[f].lane;
                    V128 m = {0x4141414142424242ull, 0x4343434344444444ull};  /* other lanes: finite */
                    V128 n = {0xdeadbeef00000000ull, 0x0123456789abcdefull};
                    V128 d = {0xfeedface00000000ull, 0x7ff8dead00000000ull};
                    if (dbl) {
                        double a = vals[i], b = vals[j], c = vals[k];
                        memcpy(&n.lo, &a, 8); memcpy(&d.lo, &c, 8);
                        memcpy(lane ? &m.hi : &m.lo, &b, 8);
                        const V128 r = forms[f].fn(&m, &n, &d);
                        double got, want = ref_d(forms[f].kind, a, b, c);
                        memcpy(&got, &r.lo, 8);
                        if (r.hi != 0) sweep_fail("upper", f, i, j, k, r.lo, r.hi);
                        else if (__builtin_isnan(want) ? !__builtin_isnan(got) : memcmp(&got, &want, 8))
                            sweep_fail("value", f, i, j, k, r.lo, r.hi);
                    } else {
                        float a = (float)vals[i], b = (float)vals[j], c = (float)vals[k];
                        uint32_t bb; memcpy(&bb, &b, 4);
                        memcpy(&n.lo, &a, 4); memcpy(&d.lo, &c, 4);
                        uint64_t *half = lane >= 2 ? &m.hi : &m.lo;
                        const int sh = (lane & 1) * 32;
                        *half = (*half & ~(0xffffffffull << sh)) | ((uint64_t)bb << sh);
                        const V128 r = forms[f].fn(&m, &n, &d);
                        float got, want = ref_s(forms[f].kind, a, b, c);
                        uint32_t gl = (uint32_t)r.lo;
                        memcpy(&got, &gl, 4);
                        if (r.hi != 0 || (r.lo >> 32) != 0) sweep_fail("upper", f, i, j, k, r.lo, r.hi);
                        else if (__builtin_isnan(want) ? !__builtin_isnan(got) : memcmp(&got, &want, 4))
                            sweep_fail("value", f, i, j, k, r.lo, r.hi);
                    }
                }
    V128 m = {0, 0}, n = {0, 0};
    float lanes[4] = {2.0f, 3.0f, 5.0f, 11.0f}, nn = 4.0f, want = __builtin_fmaf(4.0f, 11.0f, 2.0f);
    memcpy(&m, lanes, 16); memcpy(&n.lo, &nn, 4);
    const V128 r = alias_fmla_s3(&m, &n);
    float got; uint32_t gl = (uint32_t)r.lo; memcpy(&got, &gl, 4);
    if (got != want || r.hi || (r.lo >> 32)) sweep_fail("alias", 0, 0, 0, 0, r.lo, r.hi);
}
/* Loops: a block of scalar FP only (the low-half FP cache: upper elements
 * of v17 come from memory) and the same with a vector op (the full vector
 * cache: they come from the pinned XMM). v17 keeps all four elements; v19 is
 * rewritten as a scalar by FCSEL every iteration, so from the second
 * iteration on its element 2 must read as zero. */
typedef struct { float s6, s12; double d10; } LoopOut;
#define LOOP_BODY(VEC) \
    "ldr q17,[%[m]]\n\tldr q19,[%[m]]\n\tfmov s5, %w[n]\n\tfmov s6, %w[d]\n\tfmov s12, %w[d]\n\t" \
    "fmov d10, %[dd]\n\tfmov d11, %[nd]\n\tmovi d8, #0\n\tmovi v18.4s, #0\n\tmov x9, %[it]\n" \
    "1:\tfmla s6, s5, v17.s[3]\n\tfmul s7, s5, v17.s[1]\n\tfadd s6, s6, s7\n\t" \
    "fmla d10, d11, v17.d[1]\n\tfmla s12, s5, v19.s[2]\n\t" VEC \
    "cmp x9, #0\n\tfcsel s19, s8, s19, eq\n\tsubs x9, x9, #1\n\tb.ne 1b\n\t" \
    "str s6,[%[o]]\n\tstr s12,[%[o],#4]\n\tstr d10,[%[o],#8]"
#define LOOP_FN(name, VEC) static LoopOut name(const float *m4, float n, float d, double dd, double nd, int it) { \
    LoopOut o; __asm__ volatile(LOOP_BODY(VEC) : : [m] "r"(m4), [n] "r"(n), [d] "r"(d), [dd] "r"(dd), \
        [nd] "r"(nd), [it] "r"((long)it), [o] "r"(&o) \
        : "memory", "cc", "x9", "v5", "v6", "v7", "v8", "v10", "v11", "v12", "v17", "v18", "v19"); return o; }
LOOP_FN(loop_scalar, "")
LOOP_FN(loop_vector, "fadd v18.4s, v18.4s, v17.4s\n\t")
static LoopOut loop_ref(const float *m4, float n, float d, double dd, double nd, int it) {
    double hi; memcpy(&hi, m4 + 2, 8);
    float l2 = m4[2];  /* v19 element 2 */
    LoopOut o = {d, d, dd};
    for (long x9 = it; x9 > 0; x9--) {
        o.s6 = __builtin_fmaf(n, m4[3], o.s6);
        o.s6 = o.s6 + n * m4[1];
        o.d10 = __builtin_fma(nd, hi, o.d10);
        o.s12 = __builtin_fmaf(n, l2, o.s12);
        l2 = 0;   /* fcsel s19 (with s8 = 0 or s19) clears elements 1-3 */
    }
    return o;
}
static void test_elem_loops(void) {
    const float m4[4] = {1.25f, -0.75f, 9.0f, 0.5f};
    for (int it = 1; it <= 300; it += 37) {
        const LoopOut want = loop_ref(m4, 1.5f, 3.0f, 2.0, 0.25, it);
        const LoopOut a = loop_scalar(m4, 1.5f, 3.0f, 2.0, 0.25, it), b = loop_vector(m4, 1.5f, 3.0f, 2.0, 0.25, it);
        if (memcmp(&a, &want, sizeof want) || memcmp(&b, &want, sizeof want)) {
            if (++sweep_bad <= 8)
                printf("NG loop %d: scalar %.9g %.9g %.17g vector %.9g %.9g %.17g want %.9g %.9g %.17g\n", it,
                       a.s6, a.s12, a.d10, b.s6, b.s12, b.d10, want.s6, want.s12, want.d10);
        }
    }
}
int main(void) {
    test_pairwise();
    test_elem();
    test_elem_sweep();
    test_elem_loops();
    if (sweep_bad) { printf("NG by-element sweep: %d failures\n", sweep_bad); fails++; }
    if (fails) { printf("FAILURES: %d\n", fails); return 1; }
    printf("ALL PASS (%d checks)\n", 20);
    return 0;
}
