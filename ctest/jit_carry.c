/*
 * jit_carry.c — JIT carry-flag polarity tests.
 *
 * Background (alpha.4 bug):
 *   After ADDS/SUBS, the ARM64 C flag means *carry-out* for ADD
 *   (set when unsigned overflow occurs) and *borrow-inverted* for
 *   SUB (set when NO borrow occurs).  The x86 CF after sub is the
 *   opposite (set when borrow DOES occur), so the JIT must invert
 *   with cmc.
 *
 *   HI (C=1 AND Z=0) means unsigned greater-than.
 *   LS (C=0 OR Z=1) means unsigned less-or-equal.
 *
 *   A bug in carry polarity makes HI/LS report wrong results after
 *   SUBS, while GE/LT (which use N==V) are unaffected.
 *
 * Tests use pure C — the compiler emits the relevant ADDS/SUBS/CMP
 * instructions plus CSET/CSINC for the boolean result.  We then
 * verify the boolean.
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

/* Helpers that the compiler must emit conditional-branch or csel for. */
static int is_hi(uint64_t a, uint64_t b) {
    /* HI = unsigned greater-than: a > b */
    return a > b;
}
static int is_ls(uint64_t a, uint64_t b) {
    /* LS = unsigned less-or-equal: a <= b */
    return a <= b;
}
static int is_lo(uint64_t a, uint64_t b) {
    /* LO = unsigned less-than: a < b (uses C=0) */
    return a < b;
}
static int is_hs(uint64_t a, uint64_t b) {
    /* HS = unsigned greater-or-equal: a >= b (uses C=1) */
    return a >= b;
}
static int is_gt(int64_t a, int64_t b) { return a > b; }
static int is_lt(int64_t a, int64_t b) { return a < b; }
static int is_ge(int64_t a, int64_t b) { return a >= b; }
static int is_le(int64_t a, int64_t b) { return a <= b; }

int main(void) {
    /* 1. ADDS that carries (max + 1) → C=1, Z=0 → HI true */
    uint64_t a = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t r = a + 1;  /* compiler emits ADDS, sets C and Z */
    /* r == 0 means Z=1, but we just want the carry-check side */
    /* Use the (a < a+1) trick — that uses the C flag */
    int carry = (a < a + 1) ? 0 : 1;  /* if r==0 then a >= a+1 (wrap) → carry occurred */
    CHECK(carry == 1, "add_carry_hi");

    /* 2. ADDS that does NOT carry → C=0, Z=0 → HI false */
    a = 1; r = a + 2;
    carry = (a < r) ? 1 : 0;
    CHECK(carry == 1, "add_nocarry_hi");  /* 1 < 3 unsigned */

    /* 3. SUBS with no borrow (5 - 3 = 2) → C=1, Z=0 → HS true */
    uint64_t x = 5, y = 3;
    CHECK(is_hs(x, y), "sub_noborrow_hs");
    CHECK(is_hi(x, y), "sub_noborrow_hi");

    /* 4. SUBS with borrow (3 - 5) → C=0, Z=0 → LO true, HS false */
    x = 3; y = 5;
    CHECK(is_lo(x, y), "sub_borrow_lo");
    CHECK(is_ls(x, y), "sub_borrow_ls");
    CHECK(!is_hs(x, y), "sub_borrow_not_hs");

    /* 5. SUBS equal → Z=1, C=1 → LS true (Z=1) */
    x = 7; y = 7;
    CHECK(is_ls(x, y), "sub_eq_ls");
    CHECK(!is_hi(x, y), "sub_eq_not_hi");

    /* 6. SUBS 0 - 0 → Z=1, C=1 → HI false */
    x = 0; y = 0;
    CHECK(!is_hi(x, y), "sub_zero_zero_hi");

    /* 7. 32-bit ADDS carry (32-bit max + 1) */
    uint32_t a32 = 0xFFFFFFFFu;
    uint32_t r32 = a32 + 1;
    /* The compiler emits ADDS W-form.  Use the (a32 < r32) check which
     * uses the C flag (HS / CS).  After wraparound a32 > r32 unsigned. */
    CHECK(a32 >= r32, "add32_carry");

    /* 8. 32-bit SUBS borrow (1 - 2) */
    a32 = 1; uint32_t b32 = 2;
    uint32_t r32_2 = a32 - b32;
    /* After borrow, a32 < b32 unsigned → 1 < 2 → true */
    CHECK(a32 < b32, "sub32_borrow");

    /* 9. CSINC after CMP — used heavily by Clang for Integer.compare */
    int64_t sa = 100, sb = 200;
    CHECK(!is_gt(sa, sb), "cmp_signed_gt_inv");
    CHECK(is_lt(sa, sb), "cmp_signed_lt");
    CHECK(!is_ge(sa, sb), "cmp_signed_ge_inv");
    CHECK(is_le(sa, sb), "cmp_signed_le");

    /* 10. Signed comparison with negative numbers (uses N==V) */
    sa = -5; sb = 5;
    CHECK(is_lt(sa, sb), "cmp_signed_neg_lt");
    CHECK(!is_gt(sa, sb), "cmp_signed_neg_not_gt");
    sa = -5; sb = -5;
    CHECK(is_ge(sa, sb), "cmp_signed_eq_ge");
    CHECK(is_le(sa, sb), "cmp_signed_eq_le");

    /* 11. Signed comparison with INT64_MIN (edge case for N==V) */
    int64_t mn = INT64_MIN;
    int64_t mx = INT64_MAX;
    CHECK(is_lt(mn, mx), "cmp_min_max_lt");
    CHECK(is_gt(mx, mn), "cmp_max_min_gt");
    CHECK(is_lt(mn, 0), "cmp_min_zero_lt");
    CHECK(is_gt(mx, 0), "cmp_max_zero_gt");

    /* 12. 128-bit add via __uint128_t (uses ADDS + ADC) */
    /* (0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF) + (1, 1) = (1, 0) with carry-out. */
    __uint128_t big_a = ((__uint128_t)0xFFFFFFFFFFFFFFFFULL << 64) | 0xFFFFFFFFFFFFFFFFULL;
    __uint128_t big_b = ((__uint128_t)1 << 64) | 1;
    __uint128_t big_r = big_a + big_b;
    /* Carry from low half propagates into high half, then high half
     * overflows with carry-out (discarded).  Result: high=1, low=0. */
    CHECK((uint64_t)(big_r >> 64) == 1 && (uint64_t)big_r == 0, "adc_chain");

    /* 13. 128-bit sub via __uint128_t (uses SUBS + SBC) */
    /* (1, 0) - (0, 1) = (0, 0xFFFFFFFFFFFFFFFF) with borrow. */
    __uint128_t big_x = ((__uint128_t)1 << 64);  /* (1 : 0) */
    __uint128_t big_y = 1;
    __uint128_t big_diff = big_x - big_y;
    CHECK((uint64_t)(big_diff >> 64) == 0 &&
          (uint64_t)big_diff == 0xFFFFFFFFFFFFFFFFULL, "sbc_chain");

    printf("carry: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
