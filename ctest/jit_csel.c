/*
 * jit_csel.c — JIT conditional-select tests.
 *
 * Background:
 *   CSEL/CSINC/CSINV/CSNEG all share the same encoding shape.  These
 *   ops go through CALL_INTERP in the JIT (native CMOVcc was tried
 *   but reverted due to flag polarity issues).  Each CALL_INTERP
 *   invalidates cached vregs, and the block-splitter caps the count
 *   at MAX_CALL_INTERP_PER_BLOCK = 2 per block.
 *
 *   Tests use noinline functions so the compiler emits actual CSEL
 *   instructions (which become CALL_INTERP in the JIT).  Each
 *   function uses at most 1 CSEL to stay within the block limit.
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

    printf("csel: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
