// jit_neon_fixups.c — regression coverage for v1.5.0 SIMD fixes:
//   TBL1/TBL2/TBX1/TBX2, INS (element, vector), UHSUB unsigned wrap,
//   RBIT/NOT/CNT disambiguation, vector FCVTZS/FCVTZU, real EXT.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

static void test_tbl2(void) {
    // tbl v0.16b, {v0.16b, v1.16b}, v2.16b — 2-source table concat.
    // v0 = {0..15}, v1 = {16..31}, index = {2, 18, 5, 31, 4, 4, 4, 4,
    //                                       9, 9, 9, 9, 12, 12, 12, 12}
    uint8_t a[16], b[16], idx[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(16 + i); }
    uint8_t inds[16] = {2, 18, 5, 31, 4, 4, 4, 4, 9, 9, 9, 9, 12, 12, 12, 12};
    for (int i = 0; i < 16; i++) {
        int j = inds[i];
        exp[i] = (j < 16) ? a[j] : b[j - 16];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "ldr q2, [%[idx]]\n"
        "tbl v3.16b, {v0.16b, v1.16b}, v2.16b\n"
        "str q3, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [idx]"r"(inds), [out]"r"(out)
        : "v0","v1","v2","v3","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "tbl v.16b {v0,v1} 2-src");
}

static void test_tbl1_tbx(void) {
    // TBL 8B: table is 8 bytes ({v0.8b}); indices ≥8 → 0.
    uint8_t a[16], idx[8], out[8], exp[8];
    for (int i = 0; i < 16; i++) a[i] = (uint8_t)i;
    uint8_t inds[8] = {3, 9, 15, 0, 7, 8, 1, 2};
    for (int i = 0; i < 8; i++) exp[i] = (inds[i] < 8) ? a[inds[i]] : 0;
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr d2, [%[idx]]\n"
        "tbl v3.8b, {v0.16b}, v2.8b\n"
        "str d3, [%[out]]\n"
        :: [a]"r"(a), [idx]"r"(inds), [out]"r"(out)
        : "v0","v2","v3","memory"
    );
    CHECK(memcmp(out, exp, 8) == 0, "tbl v.8b {v0} 1-src");

    // TBX keeps dest unchanged for out-of-range index (255).
    uint8_t d[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    uint8_t out2[8], exp2[8];
    uint8_t inds2[8] = {0, 255, 2, 255, 4, 5, 6, 7};
    memcpy(exp2, d, 8);
    for (int i = 0; i < 8; i++) {
        if (inds2[i] < 8) exp2[i] = a[inds2[i]];  // 8B table = 8 bytes
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr d2, [%[idx]]\n"
        "ldr d3, [%[d]]\n"
        "tbx v3.8b, {v0.16b}, v2.8b\n"
        "str d3, [%[out]]\n"
        :: [a]"r"(a), [idx]"r"(inds2), [d]"r"(d), [out]"r"(out2)
        : "v0","v2","v3","memory"
    );
    CHECK(memcmp(out2, exp2, 8) == 0, "tbx v.8b {v0} out-of-range");
}

static void test_ins_vector_elem(void) {
    // mov v0.b[i], v2.b[j] — element-to-element INS.
    uint8_t a[16], s[16], out[16];
    for (int i = 0; i < 16; i++) { a[i] = 0xFF; s[i] = (uint8_t)(10 * (i + 1)); }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q2, [%[s]]\n"
        "ins v0.b[1], v2.b[3]\n"     /* out[1] = s[3] = 40 */
        "ins v0.b[5], v2.b[7]\n"     /* out[5] = s[7] = 80 */
        "ins v0.b[14], v2.b[0]\n"    /* out[14] = s[0] = 10 */
        "str q0, [%[out]]\n"
        :: [a]"r"(a), [s]"r"(s), [out]"r"(out)
        : "v0","v2","memory"
    );
    CHECK(out[0] == 0xFF && out[1] == 40 && out[5] == 80
          && out[14] == 10 && out[15] == 0xFF, "ins v.b[i], v.b[j]");

    // 16-bit and 32-bit element forms.
    uint16_t a16[8], s16[8], out16[8];
    for (int i = 0; i < 8; i++) { a16[i] = 0xFFFF; s16[i] = (uint16_t)(0x100 + i); }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q2, [%[s]]\n"
        "ins v0.h[2], v2.h[5]\n"
        "ins v0.s[3], v2.s[1]\n"
        "str q0, [%[out]]\n"
        :: [a]"r"(a16), [s]"r"(s16), [out]"r"(out16)
        : "v0","v2","memory"
    );
    CHECK(out16[2] == 0x105 && out16[6] == 0x102, "ins v.h[i]/v.s[i]");
}

static void test_uhsub_wrap(void) {
    // vhsubq_u8: (0 - 3) as unsigned halving = ((0 - 3) & 0xFF) >> 1 = 126.
    uint8_t a[16], b[16], out[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = 3; }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "uhsub v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out)
        : "v0","v1","v2","memory"
    );
    CHECK(out[0] == 126 && out[15] == 6, "uhsub v.16b unsigned wrap");
}

static void test_rbit_not_cnt(void) {
    uint8_t a[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t out[16];

    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "rbit v1.16b, v0.16b\n"
        "str q1, [%[out]]\n"
        :: [a]"r"(a), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(out[0] == 0 && out[1] == 128 && out[2] == 64 && out[15] == 240,
          "rbit v.16b");

    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "mvn v1.16b, v0.16b\n"
        "str q1, [%[out]]\n"
        :: [a]"r"(a), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(out[0] == 0xFF && out[15] == 0xF0, "not v.16b (mvn)");

    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "cnt v1.16b, v0.16b\n"
        "str q1, [%[out]]\n"
        :: [a]"r"(a), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 2 && out[15] == 4,
          "cnt v.16b");
}

static void test_fcvtzs_vector(void) {
    float src[4] = {-3.9f, 3.9f, -1.5f, 0.0f};
    int32_t out[4] = {0, 0, 0, 0};
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzs v1.4s, v0.4s\n"
        "str q1, [%[out]]\n"
        :: [s]"r"(src), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(out[0] == -3 && out[1] == 3 && out[2] == -1 && out[3] == 0,
          "fcvtzs v.4s (toward zero)");

    uint32_t uout[4] = {0, 0, 0, 0};
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzu v1.4s, v0.4s\n"
        "str q1, [%[out]]\n"
        :: [s]"r"(src), [out]"r"(uout) : "v0","v1","memory"
    );
    CHECK(uout[0] == 0 && uout[1] == 3 && uout[2] == 0 && uout[3] == 0,
          "fcvtzu v.4s (toward zero)");
}

static void test_ext_real(void) {
    uint8_t a[16], b[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(0x80 + i); }
    for (int i = 0; i < 16; i++) {
        // ext v2.16b, v0, v1, #4 = {v1[4..15], v0[0..3]}
        exp[i] = (i < 12) ? b[i + 4] : a[i - 12];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "ext v2.16b, v0.16b, v1.16b, #4\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ext v.16b #4");
}

static void test_tbl34(void) {
    // 4-register table: {v0..v3} concatenated (64 bytes).
    uint8_t a[64], idx[16], out[16], exp[16];
    for (int i = 0; i < 64; i++) a[i] = (uint8_t)i;
    uint8_t inds[16] = {0, 1, 31, 30, 16, 17, 63, 32, 5, 6, 7, 8, 9, 10, 11, 12};
    for (int i = 0; i < 16; i++) exp[i] = a[inds[i]];
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[a], #16]\n"
        "ldr q2, [%[a], #32]\n"
        "ldr q3, [%[a], #48]\n"
        "ldr q4, [%[idx]]\n"
        "tbl v5.16b, {v0.16b, v1.16b, v2.16b, v3.16b}, v4.16b\n"
        "str q5, [%[out]]\n"
        :: [a]"r"(a), [idx]"r"(inds), [out]"r"(out)
        : "v0","v1","v2","v3","v4","v5","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "tbl v.16b 4-src");

    // 3-register TBX: table = 48 bytes; indices 40-47 in range,
    // 48-55 out of range → keep Vd.
    uint8_t d[16] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0x00};
    uint8_t inds2[16] = {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55};
    uint8_t out2[16], exp2[16];
    memcpy(exp2, d, 16);
    for (int i = 0; i < 16; i++)
        if (inds2[i] < 48) exp2[i] = a[inds2[i]];
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[a], #16]\n"
        "ldr q2, [%[a], #32]\n"
        "ldr q4, [%[idx]]\n"
        "ldr q5, [%[d]]\n"
        "tbx v5.16b, {v0.16b, v1.16b, v2.16b}, v4.16b\n"
        "str q5, [%[out]]\n"
        :: [a]"r"(a), [idx]"r"(inds2), [d]"r"(d), [out]"r"(out2)
        : "v0","v1","v2","v4","v5","memory"
    );
    CHECK(memcmp(out2, exp2, 16) == 0, "tbx v.16b 3-src out-of-range");
}

int main(void) {
    printf("=== jit_neon_fixups ===\n");
    test_tbl2();
    test_tbl1_tbx();
    test_tbl34();
    test_ins_vector_elem();
    test_uhsub_wrap();
    test_rbit_not_cnt();
    test_fcvtzs_vector();
    test_ext_real();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
