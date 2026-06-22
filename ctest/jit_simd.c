/*
 * jit_simd.c — JIT native NEON SIMD tests.
 *
 * Background (alpha.4):
 *   The JIT gained native SSE2 codegen for a small set of NEON
 *   instructions: SIMD_LOGICAL (AND/BIC/ORR/ORN/EOR/BIF/BIT/BSL),
 *   SIMD_DUP, SIMD_MOVI, SIMD_LDST (LD1/ST1).  All other NEON ops
 *   still go through CALL_INTERP, but these four cover the patterns
 *   most C programs hit (vectorized memcmp, vectorized memset, etc.).
 *
 * Uses pure C with vectorized loops.  The compiler at -O2 emits
 * NEON SIMD ops for these patterns.  We verify the result.
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
    /* ── Vectorized memset (uses MOVI + ST1) ──────────────────────── */
    /* The compiler should vectorize this loop into NEON stores. */
    static uint8_t buf[64];
    memset(buf, 0xAA, 64);
    int all_aa = 1;
    for (int i = 0; i < 64; i++) if (buf[i] != 0xAA) all_aa = 0;
    CHECK(all_aa, "vector_memset_64");

    /* Vectorized memset with another value */
    memset(buf, 0x55, 64);
    int all_55 = 1;
    for (int i = 0; i < 64; i++) if (buf[i] != 0x55) all_55 = 0;
    CHECK(all_55, "vector_memset_55");

    /* Vectorized memcpy (uses LD1 + ST1) */
    static const uint8_t src[64] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
        32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
        48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    };
    static uint8_t dst[64];
    memcpy(dst, src, 64);
    CHECK(memcmp(dst, src, 64) == 0, "vector_memcpy_64");

    /* ── Vectorized memcmp (uses LD1 + EOR + UMAXP) ───────────────── */
    /* The compiler should vectorize this comparison. */
    static uint8_t a_buf[64], b_buf[64];
    memcpy(a_buf, src, 64);
    memcpy(b_buf, src, 64);
    /* Same → equal */
    CHECK(memcmp(a_buf, b_buf, 64) == 0, "vector_memcmp_eq");

    /* One byte different → not equal */
    b_buf[15] = 99;
    CHECK(memcmp(a_buf, b_buf, 64) != 0, "vector_memcmp_neq");

    /* Restore and check again */
    b_buf[15] = src[15];
    CHECK(memcmp(a_buf, b_buf, 64) == 0, "vector_memcmp_eq_restored");

    /* ── Vectorized XOR (EOR) — used in crypto-style code ─────────── */
    static uint8_t x_buf[64], y_buf[64], z_buf[64];
    for (int i = 0; i < 64; i++) {
        x_buf[i] = i;
        y_buf[i] = i ^ 0xFF;
    }
    /* Compute XOR */
    for (int i = 0; i < 64; i++) {
        z_buf[i] = x_buf[i] ^ y_buf[i];
    }
    /* x XOR (x XOR 0xFF) = 0xFF for all bytes */
    int all_ff = 1;
    for (int i = 0; i < 64; i++) if (z_buf[i] != 0xFF) all_ff = 0;
    CHECK(all_ff, "vector_xor_all_ff");

    /* ── Vectorized AND ───────────────────────────────────────────── */
    for (int i = 0; i < 64; i++) {
        x_buf[i] = i;
        y_buf[i] = 0x0F;
        z_buf[i] = x_buf[i] & y_buf[i];
    }
    /* z should be i & 0x0F for each byte */
    int and_ok = 1;
    for (int i = 0; i < 64; i++) if (z_buf[i] != (i & 0x0F)) and_ok = 0;
    CHECK(and_ok, "vector_and");

    /* ── Vectorized OR ────────────────────────────────────────────── */
    for (int i = 0; i < 64; i++) {
        x_buf[i] = i & 0x0F;
        y_buf[i] = i & 0xF0;
        z_buf[i] = x_buf[i] | y_buf[i];
    }
    int or_ok = 1;
    for (int i = 0; i < 64; i++) if (z_buf[i] != i) or_ok = 0;
    CHECK(or_ok, "vector_or");

    /* ── Vectorized 32-bit fill (DUP + ST1) ───────────────────────── */
    static uint32_t wbuf[16];
    for (int i = 0; i < 16; i++) wbuf[i] = 0xDEADBEEF;
    int all_deadbeef = 1;
    for (int i = 0; i < 16; i++) if (wbuf[i] != 0xDEADBEEF) all_deadbeef = 0;
    CHECK(all_deadbeef, "vector_fill_32");

    /* ── Vectorized 64-bit fill (DUP + ST1) ───────────────────────── */
    static uint64_t dbuf[8];
    for (int i = 0; i < 8; i++) dbuf[i] = 0x1122334455667788ULL;
    int all_same = 1;
    for (int i = 0; i < 8; i++) if (dbuf[i] != 0x1122334455667788ULL) all_same = 0;
    CHECK(all_same, "vector_fill_64");

    /* ── Vectorized sum (uses LD1 + ADDV) ─────────────────────────── */
    static uint8_t sumbuf[256];
    for (int i = 0; i < 256; i++) sumbuf[i] = i;
    /* Sum of 0..255 = 32640 */
    uint64_t sum = 0;
    for (int i = 0; i < 256; i++) sum += sumbuf[i];
    CHECK(sum == 32640, "vector_sum_256");

    /* ── Vectorized find-first-diff (the glibc memcmp pattern) ────── */
    /* The compiler emits LD1 + EOR + UMAXP to find the first non-zero
     * byte in the XOR.  We test it via memcmp on different-length
     * common prefixes. */
    static uint8_t p_buf[64], q_buf[64];
    memset(p_buf, 0xCC, 64);
    memset(q_buf, 0xCC, 64);
    /* First 32 bytes equal, last 32 different */
    memset(q_buf + 32, 0xDD, 32);
    CHECK(memcmp(p_buf, q_buf, 64) != 0, "memcmp_half_diff");
    CHECK(memcmp(p_buf, q_buf, 32) == 0, "memcmp_first_half_eq");

    /* ── Vectorized byte swap (uses REV16/REV32) ──────────────────── */
    static uint32_t le_buf[16];
    static uint32_t be_buf[16];
    for (int i = 0; i < 16; i++) le_buf[i] = i * 0x11111111u;
    /* Convert to BE (which on LE host = byte-swap each u32) */
    for (int i = 0; i < 16; i++) {
        uint32_t v = le_buf[i];
        be_buf[i] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                    ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
    }
    int bswap_ok = 1;
    for (int i = 0; i < 16; i++) {
        uint32_t expected = i * 0x11111111u;
        uint32_t got = be_buf[i];
        /* Expected bytes reversed */
        uint32_t reversed =
            ((expected & 0xFF) << 24) |
            ((expected & 0xFF00) << 8) |
            ((expected & 0xFF0000) >> 8) |
            ((expected & 0xFF000000) >> 24);
        if (got != reversed) bswap_ok = 0;
    }
    CHECK(bswap_ok, "vector_bswap_32");

    printf("simd: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
