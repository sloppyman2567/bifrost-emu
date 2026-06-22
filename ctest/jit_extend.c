/*
 * jit_extend.c — JIT sign/zero extension tests.
 *
 * Background (alpha.4 bug):
 *   LDRSB/LDRSH/LDRSW must sign-extend to 64 bits (or 32 for the W form).
 *   The decoder exposes the sign bit via opc_ls bit 1, NOT via d.cls.
 *   Using d.cls (which collapses LDRSB/LDRB into one class) produced
 *   zero-extension where sign extension was required, corrupting any
 *   computation that relied on the high bits.
 *
 * Also tests:
 *   - SXTB/SXTH/SXTW (standalone sign-extend)
 *   - UXTB/UXTH (zero-extend, must NOT pick up sign)
 *   - LDRSW followed by arithmetic that uses the sign
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

int main(void) {
    /* Set up a small buffer with negative bytes */
    static unsigned char buf[16] = {
        0xFF, 0x80, 0x7F, 0x00,
        0xFF, 0xFF, 0xFF, 0x80,
        0x7F, 0xFF, 0xFF, 0xFF,
        0x80, 0x00, 0x00, 0x00,
    };

    /* 1. LDRSB (64-bit dest) — buf[0]=0xFF → -1 */
    int64_t sb;
    __asm__ volatile ("ldrsb %0, [%1]" : "=r"(sb) : "r"(buf));
    CHECK(sb == -1, "ldrsb_neg1");

    /* 2. LDRSH (64-bit dest) — buf[0..1]=0x80FF (LE) → 0x80FF as int16 = -32641 */
    int64_t sh;
    __asm__ volatile ("ldrsh %0, [%1]" : "=r"(sh) : "r"(buf));
    CHECK(sh == -0x7F01, "ldrsh_neg");

    /* 3. LDRSW (64-bit dest) — buf[4..7]=0x80FFFFFF (LE) → 0x80FFFFFF as int32 = -2130706433 */
    int64_t sw;
    __asm__ volatile ("ldrsw %0, [%1]" : "=r"(sw) : "r"((char*)buf + 4));
    CHECK(sw == (int64_t)(int32_t)0x80FFFFFFu, "ldrsw_neg");

    /* 4. LDRSB W form — must zero upper 32 bits of dest */
    int32_t sb_w;
    __asm__ volatile ("ldrsb %w0, [%1]" : "=r"(sb_w) : "r"(buf));
    CHECK(sb_w == -1, "ldrsb_w_neg1");

    /* 5. LDRSH W form */
    int32_t sh_w;
    __asm__ volatile ("ldrsh %w0, [%1]" : "=r"(sh_w) : "r"(buf));
    CHECK(sh_w == -0x7F01, "ldrsh_w_neg");

    /* 6. SXTB — sign-extend byte (W source, X dest) */
    uint32_t v = 0xDEADBE80u;  /* bit 7 set */
    uint64_t r;
    __asm__ volatile ("sxtb %0, %w1" : "=r"(r) : "r"(v));
    CHECK((int64_t)r == -128, "sxtb");

    /* 7. SXTH */
    __asm__ volatile ("sxth %0, %w1" : "=r"(r) : "r"(v));
    CHECK((int64_t)r == (int64_t)(int16_t)0xBE80u, "sxth");

    /* 8. SXTW */
    __asm__ volatile ("sxtw %0, %w1" : "=r"(r) : "r"(v));
    CHECK((int64_t)r == (int64_t)(int32_t)0xDEADBE80u, "sxtw");

    /* 9. SXTB of a positive byte */
    v = 0x7F;
    __asm__ volatile ("sxtb %0, %w1" : "=r"(r) : "r"(v));
    CHECK(r == 0x7F, "sxtb_pos");

    /* 10. UXTB — must zero-extend, NOT sign-extend */
    v = 0xDEADBE80u;
    __asm__ volatile ("uxtb %0, %w1" : "=r"(r) : "r"(v));
    CHECK(r == 0x80, "uxtb");

    /* 11. UXTH */
    __asm__ volatile ("uxth %0, %w1" : "=r"(r) : "r"(v));
    CHECK(r == 0xBE80, "uxth");

    /* 12. Sign-extended LDRSB used in arithmetic (regression) */
    int64_t arr[4] = { -100, -1, 0, 50 };
    /* Loading byte at arr[1] (low byte of -1 = 0xFF) */
    int64_t loaded;
    __asm__ volatile ("ldrsb %0, [%1]" : "=r"(loaded) : "r"(&arr[1]));
    int64_t sum = loaded + 200;  /* -1 + 200 = 199 */
    CHECK(sum == 199, "ldrsb_arith");

    /* 13. Sign-extend then multiply (catches missed sign-extension in JIT) */
    int8_t b = -3;
    int64_t wide;
    __asm__ volatile ("ldrsb %0, [%1]" : "=r"(wide) : "r"(&b));
    int64_t prod = wide * 100;  /* -300 */
    CHECK(prod == -300, "ldrsb_mul");

    /* 14. Negative LDRSW used in array index */
    static int32_t idx_buf[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    int32_t neg_idx_storage = -2;  /* treat as unsigned offset into a region */
    /* Compute &idx_buf[2] + (neg_idx) — sign-extended add */
    int64_t base = (int64_t)&idx_buf[2];
    int64_t off;
    __asm__ volatile ("ldrsw %0, [%1]" : "=r"(off) : "r"(&neg_idx_storage));
    int32_t *p = (int32_t*)(base + off * 4);
    CHECK(*p == 10, "ldrsw_index");  /* idx_buf[2] + (-2)*4 = idx_buf[0] = 10 */

    printf("extend: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
