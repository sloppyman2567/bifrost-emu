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

    /* Tests 7-9: widening multiply by indexed lane (neverball regression).
     * `smull v.4s, v.4h, v.h[lane]` JIT-broadcast the lane with PSHUFD
     * (dword shuffle) instead of PSHUFLW/PSHUFHW (word shuffles), so every
     * nonzero lane picked the wrong coefficient (libjpeg's IDCT produced
     * zeros/garbage). All table lanes use DISTINCT values so any lane
     * mixup fails. Lanes 0-3 cover pshuflw, 4-7 cover pshufhw.
     * (Inputs are volatile-loaded so GCC cannot constant-fold the
     * intrinsics away — a folded test would pass vacuously.) */
    printf("Test 7: smull by lane (lanes 0-7)\n");
    {
        static volatile int16_t va_mem[4] = {1, 2, 3, 4};
        static volatile int16_t vt_mem[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        int16_t la[4], lt[8];
        for (int i = 0; i < 4; i++) la[i] = va_mem[i];
        for (int i = 0; i < 8; i++) lt[i] = vt_mem[i];
        int16x4_t va = vld1_s16(la);
        int16x8_t vt = vld1q_s16(lt);
        int32x4_t r;
        int32_t got[4];
        const int32_t tab[8] = {10, 20, 30, 40, 50, 60, 70, 80};
#define CHECK_LANE(n) do { \
            r = vmull_laneq_s16(va, vt, n); \
            vst1q_s32(got, r); \
            for (int i = 0; i < 4; i++) { \
                if (got[i] != (la[i]) * tab[n]) { \
                    printf("FAIL: lane %d got[%d]=%d expected %d\n", \
                           n, i, got[i], (la[i]) * tab[n]); \
                    return 1; \
                } \
            } \
        } while (0)
        CHECK_LANE(0); CHECK_LANE(1); CHECK_LANE(2); CHECK_LANE(3);
        CHECK_LANE(4); CHECK_LANE(5); CHECK_LANE(6); CHECK_LANE(7);
#undef CHECK_LANE
    }
    printf("PASS\n");

    printf("Test 8: smlal/smlsl by lane (accumulate variants)\n");
    {
        static volatile int16_t va_mem[4] = {1, 2, 3, 4};
        static volatile int16_t vt_mem[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        int16_t la[4], lt[8];
        for (int i = 0; i < 4; i++) la[i] = va_mem[i];
        for (int i = 0; i < 8; i++) lt[i] = vt_mem[i];
        int16x4_t va = vld1_s16(la);
        int16x8_t vt = vld1q_s16(lt);
        int32x4_t acc = {1000, 1000, 1000, 1000};
        int32x4_t r;
        int32_t got[4];
        r = vmlal_laneq_s16(acc, va, vt, 3);  /* acc + va*40 */
        vst1q_s32(got, r);
        int32_t exp8[4] = {1040, 1080, 1120, 1160};
        if (memcmp(got, exp8, 16) != 0) {
            printf("FAIL: smlal lane3 got %d %d %d %d\n",
                   got[0], got[1], got[2], got[3]);
            return 1;
        }
        r = vmlsl_laneq_s16(acc, va, vt, 5);  /* acc - va*60 */
        vst1q_s32(got, r);
        int32_t exp8b[4] = {940, 880, 820, 760};
        if (memcmp(got, exp8b, 16) != 0) {
            printf("FAIL: smlsl lane5 got %d %d %d %d\n",
                   got[0], got[1], got[2], got[3]);
            return 1;
        }
        /* non-widening MLS: acc16 - va*30 (lane 2) */
        {
            static volatile int16_t ma_mem[4] = {100, 100, 100, 100};
            int16_t ma[4];
            for (int i = 0; i < 4; i++) ma[i] = ma_mem[i];
            int16x4_t acc16 = vld1_s16(ma);
            int16x4_t rm = vmls_laneq_s16(acc16, va, vt, 2);
            int16_t gotm[4];
            vst1_s16(gotm, rm);
            int16_t expm[4] = {70, 40, 10, -20};
            if (memcmp(gotm, expm, 8) != 0) {
                printf("FAIL: mls lane2 got %d %d %d %d\n",
                       gotm[0], gotm[1], gotm[2], gotm[3]);
                return 1;
            }
        }
    }
    printf("PASS\n");

    printf("Test 9: smull by .s lane (32-bit, regression guard)\n");
    {
        static volatile int32_t sa_mem[2] = {3, 5};
        static volatile int32_t st_mem[4] = {7, 11, 13, 17};
        int32_t sa[2], st[4];
        for (int i = 0; i < 2; i++) sa[i] = sa_mem[i];
        for (int i = 0; i < 4; i++) st[i] = st_mem[i];
        int32x2_t va = vld1_s32(sa);
        int32x4_t vt = vld1q_s32(st);
        int64x2_t r = vmull_laneq_s32(va, vt, 2);  /* va*13 */
        int64_t got[2];
        vst1q_s64(got, r);
        if (got[0] != (int64_t)sa[0] * st[2] || got[1] != (int64_t)sa[1] * st[2]) {
            printf("FAIL: smull_lane_s32 got %ld %ld\n", (long)got[0], (long)got[1]);
            return 1;
        }
    }
    printf("PASS\n");

    printf("\ntest_simd_arith: ALL PASS\n");
    return 0;
}
