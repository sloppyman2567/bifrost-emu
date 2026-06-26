// test_simd_arith.c — Test native SIMD arithmetic (ADD/SUB/MUL).
//
// Uses NEON intrinsics to generate vector add/sub/mul instructions
// that the JIT should now compile natively via SIMD_ARITH.
#include <stdio.h>
#include <arm_neon.h>
#include <string.h>

int main(void) {
    printf("Test 1: 8-bit lane add (8 elements)\n");
    uint8x8_t a = vdup_n_u8(10);
    uint8x8_t b = vcreate_u8(0x0504030201000706ULL);
    uint8x8_t r = vadd_u8(a, b);
    // b as LE bytes: {06, 07, 00, 01, 02, 03, 04, 05}
    // a + b =        {16, 17, 10, 11, 12, 13, 14, 15}
    uint8_t expected[8] = {16, 17, 10, 11, 12, 13, 14, 15};
    uint8_t got[8];
    vst1_u8(got, r);
    if (memcmp(got, expected, 8) != 0) {
        printf("FAIL: got");
        for (int i = 0; i < 8; i++) printf(" %d", got[i]);
        printf("\n");
        return 1;
    }
    printf("PASS\n");

    printf("Test 2: 16-bit lane sub (4 elements)\n");
    uint16x4_t a2 = vcreate_u16(0x0010002000300040ULL);
    uint16x4_t b2 = vcreate_u16(0x0001000200030004ULL);
    uint16x4_t r2 = vsub_u16(a2, b2);
    uint16_t got2[4];
    vst1_u16(got2, r2);
    uint16_t exp2[4] = {0x003C, 0x002D, 0x001E, 0x000F};
    if (memcmp(got2, exp2, 8) != 0) {
        printf("FAIL: got");
        for (int i = 0; i < 4; i++) printf(" 0x%04x", got2[i]);
        printf("\n");
        return 1;
    }
    printf("PASS\n");

    printf("Test 3: 32-bit lane mul (2 elements)\n");
    uint32x2_t a3 = vcreate_u32(0x0000000300000007ULL);
    uint32x2_t b3 = vcreate_u32(0x0000000600000005ULL);
    uint32x2_t r3 = vmul_u32(a3, b3);
    uint32_t got3[2];
    vst1_u32(got3, r3);
    uint32_t exp3[2] = {35, 18};
    if (memcmp(got3, exp3, 8) != 0) {
        printf("FAIL: got %u %u\n", got3[0], got3[1]);
        return 1;
    }
    printf("PASS\n");

    printf("Test 4: 128-bit add (16 bytes)\n");
    uint8x16_t a4 = vdupq_n_u8(1);
    uint8x16_t b4 = vdupq_n_u8(2);
    uint8x16_t r4 = vaddq_u8(a4, b4);
    uint8_t got4[16];
    vst1q_u8(got4, r4);
    for (int i = 0; i < 16; i++) {
        if (got4[i] != 3) {
            printf("FAIL: got4[%d]=%d expected 3\n", i, got4[i]);
            return 1;
        }
    }
    printf("PASS\n");

    printf("Test 5: 32-bit lane add (4 elements, Q=1)\n");
    uint32x4_t a5 = vdupq_n_u32(100);
    uint32x4_t b5 = {1, 2, 3, 4};
    uint32x4_t r5 = vaddq_u32(a5, b5);
    uint32_t got5[4];
    vst1q_u32(got5, r5);
    uint32_t exp5[4] = {101, 102, 103, 104};
    if (memcmp(got5, exp5, 16) != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");

    printf("Test 6: 16-bit lane mul (4 elements)\n");
    uint16x4_t a6 = vcreate_u16(0x000A001400030005ULL);
    uint16x4_t b6 = vcreate_u16(0x0002000100070003ULL);
    uint16x4_t r6 = vmul_u16(a6, b6);
    uint16_t got6[4];
    vst1_u16(got6, r6);
    uint16_t exp6[4] = {15, 21, 20, 20};
    if (memcmp(got6, exp6, 8) != 0) {
        printf("FAIL: got");
        for (int i = 0; i < 4; i++) printf(" %u", got6[i]);
        printf("\n");
        return 1;
    }
    printf("PASS\n");

    printf("\ntest_simd_arith: ALL PASS\n");
    return 0;
}
