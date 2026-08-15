/*
 * jit_fcsel.c — JIT native FP conditional-select (FCSEL) tests.
 *
 * Background (1.5.2-alpha):
 *   FCSEL Sd/Dd, Sn, Sm, cond previously lowered to a 4-op chain
 *   (FMOV_F2G; FMOV_F2G; CSEL; FMOV_G2F) ≈ 20+ host instructions
 *   including a pushfq/popfq and a full vreg flush. It is now a single
 *   native IR op (IROp::FP_CSEL) compiled to x86 VBLENDVPS/VPD with a
 *   flags-derived sign mask in XMM0.
 *
 *   The mask semantics: DEST = (mask lane-sign-bit SET) ? SRC1 : SRC2.
 *   ARM n is the cond-TRUE operand → VEX.vvvv (SRC1), ARM m is the
 *   cond-FALSE operand → ModRM.rm (SRC2). Every test selects between two
 *   clearly distinct values (1.0f vs 2.0f, or +0.0 vs -0.0 bits) so a
 *   reversed vvvv/rm silently fails.
 *
 *   These tests are designed to be discriminating:
 *     - all 16 ARM conditions, single + double precision
 *     - the HI/LS cmc path (flags from ADDS, carry direct)
 *     - the pstate-load path (flag-setter and fcsel in different blocks,
 *       and a volatile integer op clobbering RFLAGS between them)
 *     - NaN selection (bitwise — a NaN operand is still selected exactly)
 *     - AL/NV (the interpreter treats both as always-true, decoder.cpp)
 *     - bit-exact double selection (+0.0 vs -0.0 via memcpy)
 *     - real C ternaries (GCC -O2 lowers float/double ternaries to fcsel)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("OK: %s\n", msg); } \
    else      { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* ── Per-condition single-precision helpers ────────────────────────────
 * Each sets the guest flags with FLAGASM (operand %w1 = w), then does
 * `fcsel s0, s1, s2, CONDMN`. vn/vm are pinned to s1/s2 so the operand
 * order is exact. The w value picks TRUE (expect n) or FALSE (expect m);
 * AL/NV are always-true, so both pick n. */
#define DEF_FCSEL_S(NAME, CONDMN, FLAGASM) \
    __attribute__((noinline)) static float NAME(float n, float m, int w) { \
        register float vn asm("s1") = n; \
        register float vm asm("s2") = m; \
        register float rout asm("s0") = 0.0f; \
        asm volatile(FLAGASM "\n" \
                     "fcsel s0, s1, s2, " CONDMN "\n" \
                     : "=w"(rout) : "r"(w), "w"(vn), "w"(vm) : "cc"); \
        return rout; \
    }

DEF_FCSEL_S(fcsel_eq, "eq", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_ne, "ne", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_cs, "cs", "cmp %w1, #1")   /* w=1 → C=1; w=0 → C=0 */
DEF_FCSEL_S(fcsel_cc, "cc", "cmp %w1, #1")
DEF_FCSEL_S(fcsel_mi, "mi", "cmp %w1, #0")   /* w=-1 → N=1; w=1 → N=0 */
DEF_FCSEL_S(fcsel_pl, "pl", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_hi, "hi", "cmp %w1, #1")   /* w=2 → C=1,Z=0; w=1 → Z=1 */
DEF_FCSEL_S(fcsel_ls, "ls", "cmp %w1, #1")
DEF_FCSEL_S(fcsel_ge, "ge", "cmp %w1, #0")   /* w=0 → N=V=0; w=-1 → N=1 */
DEF_FCSEL_S(fcsel_lt, "lt", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_gt, "gt", "cmp %w1, #0")   /* w=1 → N=V=0,Z=0; w=0 → Z=1 */
DEF_FCSEL_S(fcsel_le, "le", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_al, "al", "cmp %w1, #0")
DEF_FCSEL_S(fcsel_nv, "nv", "cmp %w1, #0")

/* VS/VC: V is only set by arithmetic. adds w,w,w overflows (V=1) for
 * w = 0x40000000. The +r operand is modified, so use a named operand. */
#define DEF_FCSEL_S_ADD(NAME, CONDMN) \
    __attribute__((noinline)) static float NAME(float n, float m, int w) { \
        register float vn asm("s1") = n; \
        register float vm asm("s2") = m; \
        register float rout asm("s0") = 0.0f; \
        register int wv asm("w3") = w; \
        asm volatile("adds %w[w], %w[w], %w[w]\n" \
                     "fcsel s0, s1, s2, " CONDMN "\n" \
                     : "=w"(rout), [w] "+r"(wv) : "w"(vn), "w"(vm) : "cc"); \
        return rout; \
    }

DEF_FCSEL_S_ADD(fcsel_vs, "vs")   /* w=0x40000000 → V=1; w=0 → V=0 */
DEF_FCSEL_S_ADD(fcsel_vc, "vc")

/* ── Per-condition double-precision helpers ──────────────────────────── */
#define DEF_FCSEL_D(NAME, CONDMN, FLAGASM) \
    __attribute__((noinline)) static double NAME(double n, double m, int w) { \
        register double vn asm("d1") = n; \
        register double vm asm("d2") = m; \
        register double rout asm("d0") = 0.0; \
        asm volatile(FLAGASM "\n" \
                     "fcsel d0, d1, d2, " CONDMN "\n" \
                     : "=w"(rout) : "r"(w), "w"(vn), "w"(vm) : "cc"); \
        return rout; \
    }

DEF_FCSEL_D(fcsel_d_eq, "eq", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_ne, "ne", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_cs, "cs", "cmp %w1, #1")
DEF_FCSEL_D(fcsel_d_cc, "cc", "cmp %w1, #1")
DEF_FCSEL_D(fcsel_d_mi, "mi", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_pl, "pl", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_hi, "hi", "cmp %w1, #1")
DEF_FCSEL_D(fcsel_d_ls, "ls", "cmp %w1, #1")
DEF_FCSEL_D(fcsel_d_ge, "ge", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_lt, "lt", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_gt, "gt", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_le, "le", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_al, "al", "cmp %w1, #0")
DEF_FCSEL_D(fcsel_d_nv, "nv", "cmp %w1, #0")

#define DEF_FCSEL_D_ADD(NAME, CONDMN) \
    __attribute__((noinline)) static double NAME(double n, double m, int w) { \
        register double vn asm("d1") = n; \
        register double vm asm("d2") = m; \
        register double rout asm("d0") = 0.0; \
        register int wv asm("w3") = w; \
        asm volatile("adds %w[w], %w[w], %w[w]\n" \
                     "fcsel d0, d1, d2, " CONDMN "\n" \
                     : "=w"(rout), [w] "+r"(wv) : "w"(vn), "w"(vm) : "cc"); \
        return rout; \
    }

DEF_FCSEL_D_ADD(fcsel_d_vs, "vs")
DEF_FCSEL_D_ADD(fcsel_d_vc, "vc")

/* ── pstate-load path: flag-setter and fcsel in separate blocks ─────── */
__attribute__((noinline)) static void set_flags_cmp0(int w) {
    asm volatile("cmp %w0, #0\n" :: "r"(w) : "cc");
}
__attribute__((noinline)) static float fcsel_eq_pstate(float n, float m) {
    register float vn asm("s1") = n;
    register float vm asm("s2") = m;
    register float rout asm("s0") = 0.0f;
    asm volatile("fcsel s0, s1, s2, eq\n"
                 : "=w"(rout) : "w"(vn), "w"(vm) : "cc");
    return rout;
}
__attribute__((noinline)) static double fcsel_d_lt_pstate(double n, double m) {
    register double vn asm("d1") = n;
    register double vm asm("d2") = m;
    register double rout asm("d0") = 0.0;
    asm volatile("fcsel d0, d1, d2, lt\n"
                 : "=w"(rout) : "w"(vn), "w"(vm) : "cc");
    return rout;
}
/* pstate-load with a volatile int op clobbering RFLAGS between the
 * flag-setter and the fcsel (forces the JIT's ADD to materialize flags). */
__attribute__((noinline)) static float fcsel_eq_volatile(float n, float m, int w) {
    register float vn asm("s1") = n;
    register float vm asm("s2") = m;
    register float rout asm("s0") = 0.0f;
    asm volatile("cmp %w0, #0\n" :: "r"(w) : "cc");
    volatile int dummy = w + 1;
    (void)dummy;
    asm volatile("fcsel s0, s1, s2, eq\n"
                 : "=w"(rout) : "w"(vn), "w"(vm) : "cc");
    return rout;
}

/* ── HI/LS cmc path: flags from ADDS (carry direct, needs cmc) ──────── */
__attribute__((noinline)) static float fcsel_hi_adds(float n, float m, int w) {
    register float vn asm("s1") = n;
    register float vm asm("s2") = m;
    register float rout asm("s0") = 0.0f;
    register int wv asm("w3") = w;
    asm volatile("adds %w[w], %w[w], #2\n"
                 "fcsel s0, s1, s2, hi\n"
                 : "=w"(rout), [w] "+r"(wv) : "w"(vn), "w"(vm) : "cc");
    return rout;
}
__attribute__((noinline)) static float fcsel_ls_adds(float n, float m, int w) {
    register float vn asm("s1") = n;
    register float vm asm("s2") = m;
    register float rout asm("s0") = 0.0f;
    register int wv asm("w3") = w;
    asm volatile("adds %w[w], %w[w], #2\n"
                 "fcsel s0, s1, s2, ls\n"
                 : "=w"(rout), [w] "+r"(wv) : "w"(vn), "w"(vm) : "cc");
    return rout;
}

/* ── C ternaries (GCC -O2 lowers these to fcsel) ────────────────────── */
__attribute__((noinline)) static float f_tern(float a, float b, float x, float y) {
    return (a > b) ? x : y;
}
__attribute__((noinline)) static double d_tern(double a, double b, double x, double y) {
    return (a > b) ? x : y;
}

static void test_all_conds_s(void) {
    /* n=1.0f selected on TRUE, m=2.0f on FALSE; AL/NV always TRUE. */
    CHECK(fcsel_eq(1.0f, 2.0f, 0) == 1.0f,  "s_eq_true");
    CHECK(fcsel_eq(1.0f, 2.0f, 1) == 2.0f,  "s_eq_false");
    CHECK(fcsel_ne(1.0f, 2.0f, 1) == 1.0f,  "s_ne_true");
    CHECK(fcsel_ne(1.0f, 2.0f, 0) == 2.0f,  "s_ne_false");
    CHECK(fcsel_cs(1.0f, 2.0f, 1) == 1.0f,  "s_cs_true");
    CHECK(fcsel_cs(1.0f, 2.0f, 0) == 2.0f,  "s_cs_false");
    CHECK(fcsel_cc(1.0f, 2.0f, 0) == 1.0f,  "s_cc_true");
    CHECK(fcsel_cc(1.0f, 2.0f, 1) == 2.0f,  "s_cc_false");
    CHECK(fcsel_mi(1.0f, 2.0f, -1) == 1.0f, "s_mi_true");
    CHECK(fcsel_mi(1.0f, 2.0f, 1) == 2.0f,  "s_mi_false");
    CHECK(fcsel_pl(1.0f, 2.0f, 1) == 1.0f,  "s_pl_true");
    CHECK(fcsel_pl(1.0f, 2.0f, -1) == 2.0f, "s_pl_false");
    CHECK(fcsel_vs(1.0f, 2.0f, 0x40000000) == 1.0f, "s_vs_true");
    CHECK(fcsel_vs(1.0f, 2.0f, 0) == 2.0f,  "s_vs_false");
    CHECK(fcsel_vc(1.0f, 2.0f, 0) == 1.0f,  "s_vc_true");
    CHECK(fcsel_vc(1.0f, 2.0f, 0x40000000) == 2.0f, "s_vc_false");
    CHECK(fcsel_hi(1.0f, 2.0f, 2) == 1.0f,  "s_hi_true");
    CHECK(fcsel_hi(1.0f, 2.0f, 1) == 2.0f,  "s_hi_false");
    CHECK(fcsel_ls(1.0f, 2.0f, 1) == 1.0f,  "s_ls_true");
    CHECK(fcsel_ls(1.0f, 2.0f, 2) == 2.0f,  "s_ls_false");
    CHECK(fcsel_ge(1.0f, 2.0f, 0) == 1.0f,  "s_ge_true");
    CHECK(fcsel_ge(1.0f, 2.0f, -1) == 2.0f, "s_ge_false");
    CHECK(fcsel_lt(1.0f, 2.0f, -1) == 1.0f, "s_lt_true");
    CHECK(fcsel_lt(1.0f, 2.0f, 0) == 2.0f,  "s_lt_false");
    CHECK(fcsel_gt(1.0f, 2.0f, 1) == 1.0f,  "s_gt_true");
    CHECK(fcsel_gt(1.0f, 2.0f, 0) == 2.0f,  "s_gt_false");
    CHECK(fcsel_le(1.0f, 2.0f, 0) == 1.0f,  "s_le_true");
    CHECK(fcsel_le(1.0f, 2.0f, 1) == 2.0f,  "s_le_false");
    /* AL/NV: always select src1 (interp cond_true → true for both). */
    CHECK(fcsel_al(1.0f, 2.0f, 0) == 1.0f,  "s_al_true");
    CHECK(fcsel_al(1.0f, 2.0f, 1) == 1.0f,  "s_al_false");
    CHECK(fcsel_nv(1.0f, 2.0f, 0) == 1.0f,  "s_nv_true");
    CHECK(fcsel_nv(1.0f, 2.0f, 1) == 1.0f,  "s_nv_false");
}

static void test_all_conds_d(void) {
    CHECK(fcsel_d_eq(1.0, 2.0, 0) == 1.0,  "d_eq_true");
    CHECK(fcsel_d_eq(1.0, 2.0, 1) == 2.0,  "d_eq_false");
    CHECK(fcsel_d_ne(1.0, 2.0, 1) == 1.0,  "d_ne_true");
    CHECK(fcsel_d_ne(1.0, 2.0, 0) == 2.0,  "d_ne_false");
    CHECK(fcsel_d_cs(1.0, 2.0, 1) == 1.0,  "d_cs_true");
    CHECK(fcsel_d_cs(1.0, 2.0, 0) == 2.0,  "d_cs_false");
    CHECK(fcsel_d_cc(1.0, 2.0, 0) == 1.0,  "d_cc_true");
    CHECK(fcsel_d_cc(1.0, 2.0, 1) == 2.0,  "d_cc_false");
    CHECK(fcsel_d_mi(1.0, 2.0, -1) == 1.0, "d_mi_true");
    CHECK(fcsel_d_mi(1.0, 2.0, 1) == 2.0,  "d_mi_false");
    CHECK(fcsel_d_pl(1.0, 2.0, 1) == 1.0,  "d_pl_true");
    CHECK(fcsel_d_pl(1.0, 2.0, -1) == 2.0, "d_pl_false");
    CHECK(fcsel_d_vs(1.0, 2.0, 0x40000000) == 1.0, "d_vs_true");
    CHECK(fcsel_d_vs(1.0, 2.0, 0) == 2.0,  "d_vs_false");
    CHECK(fcsel_d_vc(1.0, 2.0, 0) == 1.0,  "d_vc_true");
    CHECK(fcsel_d_vc(1.0, 2.0, 0x40000000) == 2.0, "d_vc_false");
    CHECK(fcsel_d_hi(1.0, 2.0, 2) == 1.0,  "d_hi_true");
    CHECK(fcsel_d_hi(1.0, 2.0, 1) == 2.0,  "d_hi_false");
    CHECK(fcsel_d_ls(1.0, 2.0, 1) == 1.0,  "d_ls_true");
    CHECK(fcsel_d_ls(1.0, 2.0, 2) == 2.0,  "d_ls_false");
    CHECK(fcsel_d_ge(1.0, 2.0, 0) == 1.0,  "d_ge_true");
    CHECK(fcsel_d_ge(1.0, 2.0, -1) == 2.0, "d_ge_false");
    CHECK(fcsel_d_lt(1.0, 2.0, -1) == 1.0, "d_lt_true");
    CHECK(fcsel_d_lt(1.0, 2.0, 0) == 2.0,  "d_lt_false");
    CHECK(fcsel_d_gt(1.0, 2.0, 1) == 1.0,  "d_gt_true");
    CHECK(fcsel_d_gt(1.0, 2.0, 0) == 2.0,  "d_gt_false");
    CHECK(fcsel_d_le(1.0, 2.0, 0) == 1.0,  "d_le_true");
    CHECK(fcsel_d_le(1.0, 2.0, 1) == 2.0,  "d_le_false");
    CHECK(fcsel_d_al(1.0, 2.0, 0) == 1.0,  "d_al_true");
    CHECK(fcsel_d_al(1.0, 2.0, 1) == 1.0,  "d_al_false");
    CHECK(fcsel_d_nv(1.0, 2.0, 0) == 1.0,  "d_nv_true");
    CHECK(fcsel_d_nv(1.0, 2.0, 1) == 1.0,  "d_nv_false");
}

int main(void) {
    test_all_conds_s();
    test_all_conds_d();

    /* ── pstate-load path (flag-setter in a separate function) ──────── */
    set_flags_cmp0(0);                       /* Z=1 → EQ true */
    CHECK(fcsel_eq_pstate(1.0f, 2.0f) == 1.0f, "pstate_eq_true");
    set_flags_cmp0(1);                       /* Z=0 → EQ false */
    CHECK(fcsel_eq_pstate(1.0f, 2.0f) == 2.0f, "pstate_eq_false");
    set_flags_cmp0(-1);                      /* N=1,V=0 → LT true */
    CHECK(fcsel_d_lt_pstate(1.0, 2.0) == 1.0,  "pstate_d_lt_true");
    set_flags_cmp0(0);                       /* N=0,V=0 → LT false */
    CHECK(fcsel_d_lt_pstate(1.0, 2.0) == 2.0,  "pstate_d_lt_false");

    /* ── pstate-load with a volatile int op between setter and fcsel ── */
    CHECK(fcsel_eq_volatile(1.0f, 2.0f, 0) == 1.0f, "volatile_eq_true");
    CHECK(fcsel_eq_volatile(1.0f, 2.0f, 1) == 2.0f, "volatile_eq_false");

    /* ── HI/LS cmc path (flags from ADDS → carry direct → cmc) ─────── */
    CHECK(fcsel_hi_adds(1.0f, 2.0f, 0xFFFFFFFF) == 1.0f, "cmc_hi_true");
    CHECK(fcsel_hi_adds(1.0f, 2.0f, 0xFFFFFFFE) == 2.0f, "cmc_hi_false");
    CHECK(fcsel_ls_adds(1.0f, 2.0f, 0xFFFFFFFE) == 1.0f, "cmc_ls_true");
    CHECK(fcsel_ls_adds(1.0f, 2.0f, 0xFFFFFFFF) == 2.0f, "cmc_ls_false");

    /* ── NaN handling: the select is bitwise; a NaN operand is still
     *    selected exactly, never flushed/zeroed. ───────────────────── */
    {
        uint32_t nan_bits = 0x7FC00000u;
        float nan;
        memcpy(&nan, &nan_bits, 4);
        float r = fcsel_eq(nan, 3.0f, 0);   /* EQ true → select NaN */
        uint32_t rb; memcpy(&rb, &r, 4);
        CHECK(rb == nan_bits, "nan_selected_as_n");
        r = fcsel_eq(3.0f, nan, 1);         /* EQ false → select NaN (m) */
        memcpy(&rb, &r, 4);
        CHECK(rb == nan_bits, "nan_selected_as_m");
        r = fcsel_ne(nan, 3.0f, 1);         /* NE true → select NaN */
        memcpy(&rb, &r, 4);
        CHECK(rb == nan_bits, "nan_ne_selected_as_n");
    }

    /* ── Bit-exact double selection: +0.0 vs -0.0 (equal in FP compare,
     *    distinct in bits — a bit-twiddle or operand swap fails). ───── */
    {
        double zpos = 0.0, zneg = -0.0;
        uint64_t zb;
        double r;
        r = fcsel_d_eq(zpos, zneg, 0);      /* EQ true → +0.0 */
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x0000000000000000ULL, "d_zero_plus_bits");
        r = fcsel_d_ne(zpos, zneg, 1);      /* NE true → +0.0 */
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x0000000000000000ULL, "d_ne_zero_plus_bits");
        r = fcsel_d_eq(zneg, zpos, 1);      /* EQ false → +0.0 (m) */
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x0000000000000000ULL, "d_eq_false_zero_plus_bits");
        r = fcsel_d_ne(zneg, zpos, 0);      /* NE false → +0.0 (m=zpos) */
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x0000000000000000ULL, "d_ne_false_zero_plus_bits");
        /* 1.0 vs 2.0 exact bit patterns (0x3FF0... vs 0x4000...) */
        r = fcsel_d_ge(1.0, 2.0, 0);
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x3FF0000000000000ULL, "d_ge_one_bits");
        r = fcsel_d_ge(1.0, 2.0, -1);
        memcpy(&zb, &r, 8);
        CHECK(zb == 0x4000000000000000ULL, "d_ge_two_bits");
    }

    /* ── C ternaries (real fcsel streams, not just inline asm) ─────── */
    CHECK(f_tern(3.0f, 1.0f, 10.0f, 20.0f) == 10.0f, "c_tern_f_gt");
    CHECK(f_tern(1.0f, 3.0f, 10.0f, 20.0f) == 20.0f, "c_tern_f_le");
    CHECK(d_tern(3.0, 1.0, 10.0, 20.0) == 10.0, "c_tern_d_gt");
    CHECK(d_tern(1.0, 3.0, 10.0, 20.0) == 20.0, "c_tern_d_le");
    {
        /* Ternary in a loop — exercises repeated fcsel with loop-carried
         * flags/accumulator (the fp-cache / self-loop interplay). */
        float acc = 0.0f;
        int data[8] = { 3, 1, 4, 1, 5, 9, 2, 6 };
        for (int i = 0; i < 8; i++)
            acc += f_tern((float)data[i], 2.0f, 1.0f, 0.0f);
        /* data[i] > 2 → 1.0 for 3,4,5,9,6 (5 times); else 0.0 */
        CHECK(acc == 5.0f, "c_tern_loop");
    }

    printf("fcsel: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}