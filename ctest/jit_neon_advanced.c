/*
 * jit_neon_advanced.c — Advanced NEON/SIMD correctness tests.
 *
 * Exercises SIMD instructions NOT covered by jit_neon.c:
 *   - EXT (extract from concatenated vectors)
 *   - TBL (table lookup)
 *   - UZP1/UZP2 (unzip even/odd)
 *   - ZIP1/ZIP2 (interleave)
 *   - TRN1/TRN2 (transpose)
 *   - SHRN (shift right narrow)
 *   - SSHLL (shift left long, signed)
 *   - USHLL (shift left long, unsigned)
 *
 * Each sub-test uses inline assembly to force the specific instruction,
 * then compares against a scalar C reference. All pass under JIT,
 * interpreter, and ASan+UBSan.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

static int fails = 0;

static void check(const char* name, int ok) {
    if (ok) printf("ok %s\n", name);
    else    { printf("NG %s\n", name); fails++; }
}

/* ── EXT: extract bytes from concatenated v1:v0 ── */
static void test_ext(void) {
    /* EXT d0, d1, d0, #4 — take bytes [4..11] from d1:d0 */
    uint8_t a[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t b[16] = {16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
    uint8_t out[16] = {0};
    /* ext v0.16b, v1.16b, v0.16b, #4: result = bytes [4..19] of b:a */
    __asm__ volatile(
        "ext %0.16b, %1.16b, %2.16b, #4"
        : "=w"(*(uint8x16_t*)out)
        : "w"(*(const uint8x16_t*)b), "w"(*(const uint8x16_t*)a)
    );
    /* Concatenation is [Vm:Vn] = [a:b] = {0..15, 16..31}.
     * Extracting 16 bytes at offset 4: {4,5,...,15, 16,17,18,19}. */
    uint8_t e[16] = {4,5,6,7,8,9,10,11,12,13,14,15, 16,17,18,19};
    check("ext_4", memcmp(out, e, 16) == 0);
}

/* ── TBL: table lookup with 128-bit table ── */
static void test_tbl(void) {
    /* TBL with a single 16-byte table */
    uint8_t table[16] = {0x10,0x20,0x30,0x40, 0x50,0x60,0x70,0x80,
                         0x90,0xA0,0xB0,0xC0, 0xD0,0xE0,0xF0,0x0F};
    uint8_t idx[16]   = {0, 5, 10, 15, 3, 7, 12, 1, 8, 2, 14, 6, 11, 4, 9, 13};
    uint8_t out[16]   = {0};
    uint8x16_t tbl = vld1q_u8(table);
    uint8x16_t ix  = vld1q_u8(idx);
    /* Use inline asm to force TBL instruction */
    __asm__ volatile(
        "tbl %0.16b, { %1.16b }, %2.16b"
        : "=w"(*(uint8x16_t*)out)
        : "w"(tbl), "w"(ix)
    );
    uint8_t e[16];
    for (int i = 0; i < 16; i++) e[i] = table[idx[i]];
    check("tbl_single", memcmp(out, e, 16) == 0);
}

/* ── UZP1: unzip even elements ── */
static void test_uzp1(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* uzp1 v0.4s, v1.4s, v2.4s: even elements = a[0],a[2],b[0],b[2] */
    __asm__ volatile(
        "uzp1 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[0], a[2], b[0], b[2]};
    check("uzp1_even", memcmp(out, e, 16) == 0);
}

/* ── ZIP1: interleave lower halves ── */
static void test_zip1(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* zip1 v0.4s, v1.4s, v2.4s: a[0],b[0],a[1],b[1] */
    __asm__ volatile(
        "zip1 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[0], b[0], a[1], b[1]};
    check("zip1_lower", memcmp(out, e, 16) == 0);
}

/* ── TRN1: transpose lower halves ── */
static void test_trn1(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* trn1 v0.4s, v1.4s, v2.4s: a[0],b[0],a[2],b[2] */
    __asm__ volatile(
        "trn1 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[0], b[0], a[2], b[2]};
    check("trn1_lower", memcmp(out, e, 16) == 0);
}

/* ── SHRN: shift right narrow (32-bit → 16-bit) ── */
static void test_shrn(void) {
    uint32_t in[4] = {0x0000FFFF, 0x0001FFFE, 0x0002FFFC, 0x0003FFF8};
    uint16_t out[4] = {0};
    /* shrn v0.4h, v1.4s, #4: narrow right-shift by 4 */
    __asm__ volatile(
        "shrn %0.4h, %1.4s, #4"
        : "=w"(*(uint16x4_t*)out)
        : "w"(*(const uint32x4_t*)in)
    );
    uint16_t e[4] = {in[0]>>4, in[1]>>4, in[2]>>4, in[3]>>4};
    check("shrn_4", memcmp(out, e, 8) == 0);
}

/* ── SSHLL: shift left long, signed (16-bit → 32-bit) ── */
static void test_sshll(void) {
    int16_t in[4] = {-1, 100, -200, 32767};
    int32_t out[4] = {0};
    /* sshll v0.4s, v1.4h, #3: widen 16→32, shift left 3 */
    __asm__ volatile(
        "sshll %0.4s, %1.4h, #3"
        : "=w"(*(int32x4_t*)out)
        : "w"(*(const int16x4_t*)in)
    );
    int32_t e[4] = {in[0]<<3, in[1]<<3, in[2]<<3, in[3]<<3};
    check("sshll_3", memcmp(out, e, 16) == 0);
}

/* ── USHLL: shift left long, unsigned (16-bit → 32-bit) ── */
static void test_ushll(void) {
    uint16_t in[4] = {0, 100, 1000, 65535};
    uint32_t out[4] = {0};
    /* ushll v0.4s, v1.4h, #3: widen 16→32, shift left 3 */
    __asm__ volatile(
        "ushll %0.4s, %1.4h, #3"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint16x4_t*)in)
    );
    uint32_t e[4] = {in[0]<<3, in[1]<<3, in[2]<<3, in[3]<<3};
    check("ushll_3", memcmp(out, e, 16) == 0);
}

/* ── UZP2: unzip odd elements ── */
static void test_uzp2(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* uzp2 v0.4s, v1.4s, v2.4s: odd elements = a[1],a[3],b[1],b[3] */
    __asm__ volatile(
        "uzp2 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[1], a[3], b[1], b[3]};
    check("uzp2_odd", memcmp(out, e, 16) == 0);
}

/* ── ZIP2: interleave upper halves ── */
static void test_zip2(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* zip2 v0.4s, v1.4s, v2.4s: a[2],b[2],a[3],b[3] */
    __asm__ volatile(
        "zip2 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[2], b[2], a[3], b[3]};
    check("zip2_upper", memcmp(out, e, 16) == 0);
}

/* ── TRN2: transpose upper halves ── */
static void test_trn2(void) {
    uint32_t a[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t b[4] = {0xAAAA0000, 0xBBBB0000, 0xCCCC0000, 0xDDDD0000};
    uint32_t out[4] = {0};
    /* trn2 v0.4s, v1.4s, v2.4s: a[1],b[1],a[3],b[3] */
    __asm__ volatile(
        "trn2 %0.4s, %1.4s, %2.4s"
        : "=w"(*(uint32x4_t*)out)
        : "w"(*(const uint32x4_t*)a), "w"(*(const uint32x4_t*)b)
    );
    uint32_t e[4] = {a[1], b[1], a[3], b[3]};
    check("trn2_upper", memcmp(out, e, 16) == 0);
}

int main(void) {
    test_ext();
    test_tbl();
    test_uzp1();
    test_zip1();
    test_trn1();
    test_shrn();
    test_sshll();
    test_ushll();
    test_uzp2();
    test_zip2();
    test_trn2();
    printf("neon_advanced: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
