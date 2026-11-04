/*
 * jit_csel.c — JIT conditional-select tests.
 *
 * Background:
 *   CSEL/CSINC/CSINV/CSNEG all share the same encoding shape.  The
 *   JIT translates them to a transform plus native CSEL (CMOVcc, or
 *   setcc for CSET).
 *
 *   Tests use noinline functions so the compiler emits actual CSEL
 *   instructions; cset_forms_ok() fixes the exact forms in inline asm.
 *
 *   Note: main() is compiled at -O1 via __attribute__((optimize("O1")))
 *   because the -O2 codegen for repeated CHECK macros triggers a JIT
 *   bug (unmapped read at 0x8000001000).  This is a known JIT issue,
 *   not a test bug. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

/* Each function = one CSEL → one CALL_INTERP, well within the
 * MAX_CALL_INTERP_PER_BLOCK = 2 limit. */

__attribute__((noinline)) static int64_t csel_max(int64_t a, int64_t b) { return a > b ? a : b; }
__attribute__((noinline)) static int64_t csel_min(int64_t a, int64_t b) { return a < b ? a : b; }
__attribute__((noinline)) static int64_t csel_abs(int64_t a) { return a < 0 ? -a : a; }
__attribute__((noinline)) static uint64_t csel_umax(uint64_t a, uint64_t b) { return a > b ? a : b; }
__attribute__((noinline)) static uint64_t csel_umin(uint64_t a, uint64_t b) { return a < b ? a : b; }
__attribute__((noinline)) static int csel_sign(int v) { return (v > 0) ? 1 : (v < 0 ? -1 : 0); }
__attribute__((noinline)) static int csinc_test(int cond, int v) { return cond ? v : v + 1; }

/* CSET/CSETM/CINC and a W-form CSEL for every condition: the JIT emits
 * CSET as setcc+movzx (its constants never reach a register), a W-form
 * CSEL as a 32-bit CMOV that must clear bits 63:32 of dirty sources, and
 * CSETM's -1 as a sign-extended imm32. Flags come from MSR (cpu.pstate),
 * CMP and CMN (host flags, carry inverted for HI/LS after CMN). */
static int cond_true(int cc, unsigned nzcv) {
    const int n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
    switch (cc) {
        case 0: return z;            case 1: return !z;
        case 2: return c;            case 3: return !c;
        case 4: return n;            case 5: return !n;
        case 6: return v;            case 7: return !v;
        case 8: return c && !z;      case 9: return !(c && !z);
        case 10: return n == v;      case 11: return n != v;
        case 12: return !z && n == v; default: return z || n != v;
    }
}
#define FORMS(cc, setup) __asm__ volatile(setup "\n" \
    "cset %[r0], " #cc "\n cset %w[r1], " #cc "\n csetm %w[r2], " #cc "\n" \
    "csel %w[r3], %w[a], %w[b], " #cc "\n cinc %[r4], %[a], " #cc \
    : [r0] "=&r"(o[0]), [r1] "=&r"(o[1]), [r2] "=&r"(o[2]), [r3] "=&r"(o[3]), [r4] "=&r"(o[4]) \
    : [x] "r"(x), [y] "r"(y), [a] "r"(a), [b] "r"(b) : "cc")
#define FORMS_FN(cc) __attribute__((noinline)) static void forms_##cc(int mode, uint64_t x, \
    uint64_t y, uint64_t a, uint64_t b, uint64_t *o) { \
    if (mode == 0) FORMS(cc, "msr nzcv, %[x]"); \
    else if (mode == 1) FORMS(cc, "cmp %[x], %[y]"); \
    else FORMS(cc, "cmn %[x], %[y]"); }
FORMS_FN(eq) FORMS_FN(ne) FORMS_FN(cs) FORMS_FN(cc) FORMS_FN(mi) FORMS_FN(pl) FORMS_FN(vs)
FORMS_FN(vc) FORMS_FN(hi) FORMS_FN(ls) FORMS_FN(ge) FORMS_FN(lt) FORMS_FN(gt) FORMS_FN(le)
static void (*const forms[14])(int, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *) = {
    forms_eq, forms_ne, forms_cs, forms_cc, forms_mi, forms_pl, forms_vs,
    forms_vc, forms_hi, forms_ls, forms_ge, forms_lt, forms_gt, forms_le};
static unsigned nzcv_of(int mode, uint64_t x, uint64_t y) {
    if (mode == 0) return (unsigned)(x >> 28) & 0xf;
    const uint64_t r = mode == 1 ? x - y : x + y;
    const unsigned n = (int64_t)r < 0, z = r == 0;
    const unsigned c = mode == 1 ? x >= y : r < x;
    const unsigned v = mode == 1 ? (int64_t)((x ^ y) & (x ^ r)) < 0 : (int64_t)(~(x ^ y) & (x ^ r)) < 0;
    return n << 3 | z << 2 | c << 1 | v;
}
static int cset_forms_ok(void) {
    static const uint64_t vals[] = {0, 1, 2, (uint64_t)-1, (uint64_t)-2, 0x7fffffffffffffffULL,
                                    0x8000000000000000ULL, 0x80000000ULL, 0xffffffffULL};
    const uint64_t a = 0xdead0000beef1234ULL, b = 0xfeed00004321cafeULL;  /* dirty bits 63:32 */
    const int nv = (int)(sizeof vals / sizeof vals[0]);
    int bad = 0;
    for (int mode = 0; mode < 3; mode++)
        for (int i = 0; i < (mode ? nv : 16); i++)
            for (int j = 0; j < (mode ? nv : 1); j++) {
                const uint64_t x = mode ? vals[i] : (uint64_t)i << 28, y = mode ? vals[j] : 0;
                const unsigned nzcv = nzcv_of(mode, x, y);
                for (int cc = 0; cc < 14; cc++) {
                    uint64_t o[5];
                    forms[cc](mode, x, y, a, b, o);
                    const int t = cond_true(cc, nzcv);
                    const uint64_t want[5] = {(uint64_t)t, (uint64_t)t, t ? 0xffffffffULL : 0,
                                              (t ? a : b) & 0xffffffffULL, t ? a + 1 : a};
                    for (int k = 0; k < 5; k++)
                        if (o[k] != want[k] && ++bad <= 8)
                            printf("NG cset form %d mode %d cc %d nzcv %x: %llx want %llx\n", k, mode,
                                   cc, nzcv, (unsigned long long)o[k], (unsigned long long)want[k]);
                }
            }
    return bad == 0;
}

__attribute__((optimize("O1")))
int main(void) {
    /* ── Basic CSEL: max/min ──────────────────────────────────────── */
    CHECK(csel_max(100, 200) == 200, "csel_max_gt");
    CHECK(csel_max(200, 100) == 200, "csel_max_lt");
    CHECK(csel_max(50, 50) == 50, "csel_max_eq");
    CHECK(csel_min(100, 200) == 100, "csel_min_lt");
    CHECK(csel_min(200, 100) == 100, "csel_min_gt");
    CHECK(csel_min(50, 50) == 50, "csel_min_eq");

    /* ── Signed/unsigned distinction ──────────────────────────────── */
    CHECK(csel_max(-1, 1) == 1, "csel_signed_neg_vs_pos");
    CHECK(csel_umax((uint64_t)-1, 1) == (uint64_t)-1, "csel_unsigned_max");

    /* ── CSNEG: abs() ─────────────────────────────────────────────── */
    CHECK(csel_abs(5) == 5, "csneg_pos");
    CHECK(csel_abs(-5) == 5, "csneg_neg");
    CHECK(csel_abs(0) == 0, "csneg_zero");

    /* ── CSINC: ternary with increment ────────────────────────────── */
    CHECK(csinc_test(1, 100) == 100, "csinc_true");
    CHECK(csinc_test(0, 100) == 101, "csinc_false");

    /* ── CSET: boolean from comparison ────────────────────────────── */
    CHECK(csinc_test(100 > 50, 100) == 100, "cset_gt_true");
    CHECK(csinc_test(50 > 100, 100) == 101, "cset_gt_false");
    CHECK(csinc_test(100 == 100, 100) == 100, "cset_eq_true");

    /* ── CSETM: all-ones mask from comparison ─────────────────────── */
    int64_t mask = (100 > 50) ? -1 : 0;
    CHECK(mask == -1, "csetm_gt_true");
    mask = (50 > 100) ? -1 : 0;
    CHECK(mask == 0, "csetm_lt_false");

    /* ── Sign-of (uses two CSELs in a separate function) ──────────── */
    CHECK(csel_sign(5) == 1, "sign_pos");
    CHECK(csel_sign(-5) == -1, "sign_neg");
    CHECK(csel_sign(0) == 0, "sign_zero");

    /* ── CSEL inside a loop (max-of-array pattern) ────────────────── */
    uint64_t arr[8] = { 3, 1, 4, 1, 5, 9, 2, 6 };
    uint64_t mx = 0;
    for (int i = 0; i < 8; i++) {
        mx = csel_umax(mx, arr[i]);
    }
    CHECK(mx == 9, "csel_max_loop");

    /* Min-of-array */
    uint64_t mn = ~0ULL;
    for (int i = 0; i < 8; i++) {
        mn = csel_umin(mn, arr[i]);
    }
    CHECK(mn == 1, "csel_min_loop");

    /* ── CSEL inside abs(int) sum ─────────────────────────────────── */
    int64_t vals[5] = { -10, 5, -3, 0, 7 };
    int64_t abs_sum = 0;
    for (int i = 0; i < 5; i++) {
        abs_sum += csel_abs(vals[i]);
    }
    /* 10 + 5 + 3 + 0 + 7 = 25 */
    CHECK(abs_sum == 25, "csneg_abs_sum");

    /* ── Conditional increment in a loop ──────────────────────────── */
    int data[10] = { 1, 2, 3, 2, 1, 2, 4, 5, 2, 1 };
    int target = 2;
    int count = 0;
    for (int i = 0; i < 10; i++) {
        count += (data[i] == target) ? 1 : 0;
    }
    CHECK(count == 4, "csinc_count_loop");

    /* ── Branchless ternary ───────────────────────────────────────── */
    int x = 10, y = 20;
    int larger = (x > y) ? x : y;
    int smaller = (x > y) ? y : x;
    int diff = larger - smaller;
    CHECK(diff == 10, "ternary_pair");

    /* ── Bitwise conditional (CSINV) ──────────────────────────────── */
    uint32_t inv_v = 0xCAFEBABE;
    uint32_t inv_r1 = (1 == 1) ? inv_v : ~inv_v;
    uint32_t inv_r2 = (1 == 2) ? inv_v : ~inv_v;
    CHECK(inv_r1 == 0xCAFEBABEu, "csinv_true");
    CHECK(inv_r2 == 0x35014541u, "csinv_false");

    /* ── FCMP-producer selects (2026-08-21 R8-scratch regression) ──
     * FCMP materializes NZCV to pstate (flags_in_host_=false), so every
     * following conditional select runs the load-flags-from-pstate path.
     * emit_load_flags_from_pstate once used R8 as scratch while the CSEL
     * emitter flushed only RAX/RCX/RDX — a CMOVcc else-value staged in
     * R8 was silently destroyed and every cset after an fcmp returned 0
     * (minecraft's sign() → step=(0,0,0) → raycast assert / ground
     * clipping). The carry-condition cases additionally catch a wrong
     * C extraction in the loader (x86 CF must be ARM C ^ from_sub, not
     * C ^ N — a right-shift instead of left-shift in the bit trick). */
    {
        volatile float fs[] = { 1.0f, -1.0f, 0.0f, 0.5f, -2.5f, 3.0f, 0.0f/0.0f };
        volatile double fd[] = { 1.0, -1.0, 0.0, 0.5, -2.5, 3.0, 0.0/0.0 };
        int sign_ok = 1, cs_ok = 1, cc_ok = 1, hi_ok = 1, tr_ok = 1;
        for (int i = 0; i < 7; i++) {
            float f = fs[i];
            __typeof__(f) xx = f;
            int s = (int)(((0 < xx) - (xx < 0)));       /* fcmp+cset,cset+sub */
            int w = (f > 0) - (f < 0);
            if (isnan(f)) w = 0;                         /* NaN: both false */
            if (s != w) sign_ok = 0;
            /* csel cs/cc/hi directly after fcmp (C-flag polarity) */
            uint64_t r_cs, r_cc, r_hi;
            asm("fcmp %s[fa], %s[fb]\n csel %0, %[x], %[y], cs"
                : "=r"(r_cs) : [x]"r"(0x1111ULL+i), [y]"r"(0x2222ULL),
                  [fa]"w"(f), [fb]"w"(0.0f) : "cc");
            asm("fcmp %s[fa], %s[fb]\n csel %0, %[x], %[y], cc"
                : "=r"(r_cc) : [x]"r"(0x1111ULL+i), [y]"r"(0x2222ULL),
                  [fa]"w"(f), [fb]"w"(0.0f) : "cc");
            asm("fcmp %s[fa], %s[fb]\n csel %0, %[x], %[y], hi"
                : "=r"(r_hi) : [x]"r"(0x1111ULL+i), [y]"r"(0x2222ULL),
                  [fa]"w"(f), [fb]"w"(0.0f) : "cc");
            /* ARM FP flags: C=1 iff f>=0 or unordered; Z=1 iff f==0 */
            int fc = !(f < 0.0f), fz = (f == 0.0f);
            if (r_cs != (fc ? 0x1111ULL+i : 0x2222ULL)) cs_ok = 0;
            if (r_cc != (!fc ? 0x1111ULL+i : 0x2222ULL)) cc_ok = 0;
            if (r_hi != ((fc && !fz) ? 0x1111ULL+i : 0x2222ULL)) hi_ok = 0;
            /* double producer + transform variants */
            double d = fd[i];
            uint64_t r_inc, r_inv, r_neg;
            asm("fcmp %d[da], %d[db]\n csinc %0, %[x], %[y], mi"
                : "=r"(r_inc) : [x]"r"(0x81ULL+i), [y]"r"(0x82ULL),
                  [da]"w"(d), [db]"w"(0.0) : "cc");
            asm("fcmp %d[da], %d[db]\n csinv %0, %[x], %[y], ge"
                : "=r"(r_inv) : [x]"r"(0x81ULL+i), [y]"r"(0x82ULL),
                  [da]"w"(d), [db]"w"(0.0) : "cc");
            asm("fcmp %d[da], %d[db]\n csneg %0, %[x], %[y], eq"
                : "=r"(r_neg) : [x]"r"(0x81ULL+i), [y]"r"(0x82ULL),
                  [da]"w"(d), [db]"w"(0.0) : "cc");
            int fmi = (d < 0.0), fge = !(d < 0.0) && !isnan(d);
            int feq = (d == 0.0) && !isnan(d);
            if (r_inc != (fmi ? 0x81ULL+i : 0x83ULL) ||
                r_inv != (fge ? 0x81ULL+i : ~0x82ULL) ||
                r_neg != (feq ? 0x81ULL+i : (uint64_t)-(int64_t)0x82ULL))
                tr_ok = 0;
        }
        CHECK(sign_ok, "fcmp_cset_sign");
        CHECK(cs_ok, "fcmp_csel_cs");
        CHECK(cc_ok, "fcmp_csel_cc");
        CHECK(hi_ok, "fcmp_csel_hi");
        CHECK(tr_ok, "fcmp_csinc_csinv_csneg_d");
    }

    CHECK(cset_forms_ok(), "cset_csetm_cinc_csel_w_all_conditions");

    printf("csel: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
