/*
 * jit_cls.c — JIT CLS (count leading sign bits) tests.
 *
 * Background:
 *   CLS was previously routed through CALL_INTERP, which invalidated
 *   the entire vreg cache and forced a block split. Commit a0e545c
 *   decomposed CLS into the identity CLZ(v ^ SAR(v, W-1)) - 1 using
 *   primitive IR ops (SAR, XOR, CLZ, SUB).
 *
 *   This test verifies the decomposition is correct for both 32-bit
 *   and 64-bit CLS, covering all edge cases:
 *     - v == 0      → CLS = width-1
 *     - v == ~0     → CLS = width-1
 *     - v == 1      → CLS = width-2
 *     - v == -1     → CLS = width-1 (all sign bits set)
 *     - v == min_int → CLS = 0 (no leading sign bits)
 *     - v == max_int → CLS = 0
 *
 * Uses __builtin_clrsb / __builtin_clrsbll which the compiler emits
 * as the CLS instruction.
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

int main(void) {
    /* ── 64-bit CLS ─────────────────────────────────────────────── */
    /* CLS(0) = 63 (all bits are sign bits, all zero) */
    CHECK(__builtin_clrsbll(0LL) == 63, "cls_zero_64");
    /* CLS(~0) = 63 (all bits are sign bits, all one) */
    CHECK(__builtin_clrsbll(~0LL) == 63, "cls_ones_64");
    /* CLS(1) = 62 (one leading zero, then sign bit at position 0) */
    CHECK(__builtin_clrsbll(1LL) == 62, "cls_one_64");
    /* CLS(-1) = 63 (all ones — same as ~0) */
    CHECK(__builtin_clrsbll(-1LL) == 63, "cls_neg1_64");
    /* CLS(2) = 61 (two leading zeros, sign bit at position 1) */
    CHECK(__builtin_clrsbll(2LL) == 61, "cls_two_64");
    /* CLS(0x4000000000000000) = 0 (bit 62 set, no leading sign bits) */
    CHECK(__builtin_clrsbll(0x4000000000000000LL) == 0, "cls_bit62_64");
    /* CLS(0x8000000000000000) = 0 (sign bit only, no leading) */
    CHECK(__builtin_clrsbll(0x8000000000000000LL) == 0, "cls_minint_64");
    /* CLS(0x7FFFFFFFFFFFFFFF) = 0 (all-positive, no leading sign) */
    CHECK(__builtin_clrsbll(0x7FFFFFFFFFFFFFFFLL) == 0, "cls_maxint_64");
    /* CLS(0x2000000000000000) = 1 (bit 61 set, one leading zero) */
    CHECK(__builtin_clrsbll(0x2000000000000000LL) == 1, "cls_bit61_64");

    /* ── 32-bit CLS ─────────────────────────────────────────────── */
    /* CLS(0) = 31 */
    CHECK(__builtin_clrsb(0) == 31, "cls_zero_32");
    /* CLS(~0) = 31 */
    CHECK(__builtin_clrsb(~0) == 31, "cls_ones_32");
    /* CLS(1) = 30 */
    CHECK(__builtin_clrsb(1) == 30, "cls_one_32");
    /* CLS(-1) = 31 */
    CHECK(__builtin_clrsb(-1) == 31, "cls_neg1_32");
    /* CLS(0x40000000) = 0 */
    CHECK(__builtin_clrsb(0x40000000) == 0, "cls_bit30_32");
    /* CLS(0x80000000) = 0 */
    CHECK(__builtin_clrsb(0x80000000) == 0, "cls_minint_32");
    /* CLS(0x7FFFFFFF) = 0 */
    CHECK(__builtin_clrsb(0x7FFFFFFF) == 0, "cls_maxint_32");

    /* ── CLS in a loop (realistic: bit-clustering) ──────────────── */
    static int64_t values[] = {
        0, -1, 1, 2, -2, 0x4000000000000000LL, -0x4000000000000000LL,
    };
    int sum = 0;
    for (int i = 0; i < 7; i++) {
        sum += __builtin_clrsbll(values[i]);
    }
    /* 63 + 63 + 62 + 61 + 63 + 0 + 0 = 312 */
    CHECK(sum == 312, "cls_loop_sum");

    printf("cls: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
