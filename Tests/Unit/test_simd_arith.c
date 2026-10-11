// test_simd_arith.c — Test native SIMD arithmetic (ADD/SUB/MUL/ABD/ABDL).
//
// Uses NEON intrinsics to exercise native JIT vector arithmetic paths.
#include <stdio.h>
#include <arm_neon.h>
#include <string.h>
#include <stdint.h>

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

    /* Test 10: absolute differences whose mathematical result exceeds the
     * signed source-lane range. The JIT must choose max/min before subtracting;
     * subtract-then-abs wraps and returns 1 for these endpoint pairs. Inputs
     * are volatile-loaded so the compiler cannot fold the NEON operations. */
    printf("Test 10: ABD/ABDL 16/32-bit wraparound endpoints\n");
    {
        static volatile int16_t sabd16_a_mem[4] = {-32768, 32767, -100, 100};
        static volatile int16_t sabd16_b_mem[4] = { 32767,-32768,  100,-100};
        int16_t sabd16_a[4], sabd16_b[4];
        for (int i = 0; i < 4; i++) {
            sabd16_a[i] = sabd16_a_mem[i];
            sabd16_b[i] = sabd16_b_mem[i];
        }
        uint16_t got_sabd16[4];
        const uint16_t exp_sabd16[4] = {65535, 65535, 200, 200};
        vst1_u16(got_sabd16, vreinterpret_u16_s16(
            vabd_s16(vld1_s16(sabd16_a), vld1_s16(sabd16_b))));
        if (memcmp(got_sabd16, exp_sabd16, sizeof(exp_sabd16)) != 0) {
            printf("FAIL: SABD.S got %u %u %u %u\n", got_sabd16[0],
                   got_sabd16[1], got_sabd16[2], got_sabd16[3]);
            return 1;
        }

        static volatile uint16_t uabd16_a_mem[4] = {65535, 0, 50000, 100};
        static volatile uint16_t uabd16_b_mem[4] = {    0,65535,     1, 200};
        uint16_t uabd16_a[4], uabd16_b[4], got_uabd16[4];
        for (int i = 0; i < 4; i++) {
            uabd16_a[i] = uabd16_a_mem[i];
            uabd16_b[i] = uabd16_b_mem[i];
        }
        const uint16_t exp_uabd16[4] = {65535, 65535, 49999, 100};
        vst1_u16(got_uabd16, vabd_u16(vld1_u16(uabd16_a), vld1_u16(uabd16_b)));
        if (memcmp(got_uabd16, exp_uabd16, sizeof(exp_uabd16)) != 0) {
            printf("FAIL: UABD.S got %u %u %u %u\n", got_uabd16[0],
                   got_uabd16[1], got_uabd16[2], got_uabd16[3]);
            return 1;
        }

        static volatile int32_t sabd32_a_mem[2] = {-2147483647 - 1, 2147483647};
        static volatile int32_t sabd32_b_mem[2] = { 2147483647,-2147483647 - 1};
        int32_t sabd32_a[2], sabd32_b[2];
        for (int i = 0; i < 2; i++) {
            sabd32_a[i] = sabd32_a_mem[i];
            sabd32_b[i] = sabd32_b_mem[i];
        }
        uint32_t got_sabd32[2];
        const uint32_t exp_sabd32[2] = {UINT32_MAX, UINT32_MAX};
        vst1_u32(got_sabd32, vreinterpret_u32_s32(
            vabd_s32(vld1_s32(sabd32_a), vld1_s32(sabd32_b))));
        if (memcmp(got_sabd32, exp_sabd32, sizeof(exp_sabd32)) != 0) {
            printf("FAIL: SABD.D got %u %u\n", got_sabd32[0], got_sabd32[1]);
            return 1;
        }

        static volatile uint32_t uabd32_a_mem[2] = {UINT32_MAX, 0};
        static volatile uint32_t uabd32_b_mem[2] = {0, UINT32_MAX};
        uint32_t uabd32_a[2], uabd32_b[2], got_uabd32[2];
        for (int i = 0; i < 2; i++) {
            uabd32_a[i] = uabd32_a_mem[i];
            uabd32_b[i] = uabd32_b_mem[i];
        }
        const uint32_t exp_uabd32[2] = {UINT32_MAX, UINT32_MAX};
        vst1_u32(got_uabd32, vabd_u32(vld1_u32(uabd32_a), vld1_u32(uabd32_b)));
        if (memcmp(got_uabd32, exp_uabd32, sizeof(exp_uabd32)) != 0) {
            printf("FAIL: UABD.D got %u %u\n", got_uabd32[0], got_uabd32[1]);
            return 1;
        }

        static volatile int16_t sabdl16_a_mem[4] = {-32768, 32767, -100, 100};
        static volatile int16_t sabdl16_b_mem[4] = { 32767,-32768,  100,-100};
        int16_t sabdl16_a[4], sabdl16_b[4];
        for (int i = 0; i < 4; i++) {
            sabdl16_a[i] = sabdl16_a_mem[i];
            sabdl16_b[i] = sabdl16_b_mem[i];
        }
        int32_t got_sabdl16[4];
        const int32_t exp_sabdl16[4] = {65535, 65535, 200, 200};
        vst1q_s32(got_sabdl16, vabdl_s16(vld1_s16(sabdl16_a), vld1_s16(sabdl16_b)));
        if (memcmp(got_sabdl16, exp_sabdl16, sizeof(exp_sabdl16)) != 0) {
            printf("FAIL: SABDL.H got %d %d %d %d\n", got_sabdl16[0],
                   got_sabdl16[1], got_sabdl16[2], got_sabdl16[3]);
            return 1;
        }

        static volatile uint16_t uabdl16_a_mem[4] = {65535, 0, 50000, 100};
        static volatile uint16_t uabdl16_b_mem[4] = {    0,65535,     1, 200};
        uint16_t uabdl16_a[4], uabdl16_b[4];
        for (int i = 0; i < 4; i++) {
            uabdl16_a[i] = uabdl16_a_mem[i];
            uabdl16_b[i] = uabdl16_b_mem[i];
        }
        uint32_t got_uabdl16[4];
        const uint32_t exp_uabdl16[4] = {65535, 65535, 49999, 100};
        vst1q_u32(got_uabdl16, vabdl_u16(vld1_u16(uabdl16_a), vld1_u16(uabdl16_b)));
        if (memcmp(got_uabdl16, exp_uabdl16, sizeof(exp_uabdl16)) != 0) {
            printf("FAIL: UABDL.H got %u %u %u %u\n", got_uabdl16[0],
                   got_uabdl16[1], got_uabdl16[2], got_uabdl16[3]);
            return 1;
        }

        static volatile int32_t sabdl32_a_mem[2] = {-2147483647 - 1, 2147483647};
        static volatile int32_t sabdl32_b_mem[2] = { 2147483647,-2147483647 - 1};
        int32_t sabdl32_a[2], sabdl32_b[2];
        for (int i = 0; i < 2; i++) {
            sabdl32_a[i] = sabdl32_a_mem[i];
            sabdl32_b[i] = sabdl32_b_mem[i];
        }
        int64_t got_sabdl32[2];
        const int64_t exp_sabdl32[2] = {4294967295LL, 4294967295LL};
        vst1q_s64(got_sabdl32, vabdl_s32(vld1_s32(sabdl32_a), vld1_s32(sabdl32_b)));
        if (memcmp(got_sabdl32, exp_sabdl32, sizeof(exp_sabdl32)) != 0) {
            printf("FAIL: SABDL.S got %lld %lld\n", (long long)got_sabdl32[0],
                   (long long)got_sabdl32[1]);
            return 1;
        }

        static volatile uint32_t uabdl32_a_mem[2] = {UINT32_MAX, 0};
        static volatile uint32_t uabdl32_b_mem[2] = {0, UINT32_MAX};
        uint32_t uabdl32_a[2], uabdl32_b[2];
        for (int i = 0; i < 2; i++) {
            uabdl32_a[i] = uabdl32_a_mem[i];
            uabdl32_b[i] = uabdl32_b_mem[i];
        }
        uint64_t got_uabdl32[2];
        const uint64_t exp_uabdl32[2] = {UINT32_MAX, UINT32_MAX};
        vst1q_u64(got_uabdl32, vabdl_u32(vld1_u32(uabdl32_a), vld1_u32(uabdl32_b)));
        if (memcmp(got_uabdl32, exp_uabdl32, sizeof(exp_uabdl32)) != 0) {
            printf("FAIL: UABDL.S got %llu %llu\n",
                   (unsigned long long)got_uabdl32[0],
                   (unsigned long long)got_uabdl32[1]);
            return 1;
        }
    }
    printf("PASS\n");

    /* Test 11: ABDL2 high-half selection and SABAL/UABAL accumulation,
     * including the combined Q=1 accumulating forms. The low and high source
     * halves deliberately have different answers so using the wrong half is
     * visible. */
    printf("Test 11: ABDL2/ABAL high-half and accumulation\n");
#define CHECK_ABDL(label, got, expected) do { \
        if (memcmp((got), (expected), sizeof(expected)) != 0) { \
            printf("FAIL: %s\n", (label)); \
            return 1; \
        } \
    } while (0)
    {
        static volatile int16_t s16a_mem[8] = {
            -32768, 32767, -100, 100, -30000, 30000, -50, 50};
        static volatile int16_t s16b_mem[8] = {
             32767,-32768,  100,-100,  30000,-30000,  50,-50};
        int16_t s16a[8], s16b[8];
        for (int i = 0; i < 8; i++) {
            s16a[i] = s16a_mem[i]; s16b[i] = s16b_mem[i];
        }
        int16x8_t vs16a = vld1q_s16(s16a), vs16b = vld1q_s16(s16b);
        const int32_t exp_sabdl2_16[4] = {60000, 60000, 100, 100};
        int32_t got_sabdl2_16[4];
        vst1q_s32(got_sabdl2_16, vabdl_high_s16(vs16a, vs16b));
        CHECK_ABDL("SABDL2.H", got_sabdl2_16, exp_sabdl2_16);
        const int32_t exp_sabal_16[4] = {65536, 65537, 203, 204};
        const int32_t exp_sabal2_16[4] = {60010, 60020, 130, 140};
        int32_t got_sabal_16[4], got_sabal2_16[4];
        int32x4_t acc_s16 = {1, 2, 3, 4};
        vst1q_s32(got_sabal_16,
                  vabal_s16(acc_s16, vget_low_s16(vs16a), vget_low_s16(vs16b)));
        CHECK_ABDL("SABAL.H", got_sabal_16, exp_sabal_16);
        acc_s16 = (int32x4_t){10, 20, 30, 40};
        vst1q_s32(got_sabal2_16, vabal_high_s16(acc_s16, vs16a, vs16b));
        CHECK_ABDL("SABAL2.H", got_sabal2_16, exp_sabal2_16);

        static volatile uint16_t u16a_mem[8] = {
            65535, 0, 50000, 100, 60000, 1, 45000, 600};
        static volatile uint16_t u16b_mem[8] = {
                0,65535,     1, 200,     0,60000,  1000, 600};
        uint16_t u16a[8], u16b[8];
        for (int i = 0; i < 8; i++) {
            u16a[i] = u16a_mem[i]; u16b[i] = u16b_mem[i];
        }
        uint16x8_t vu16a = vld1q_u16(u16a), vu16b = vld1q_u16(u16b);
        const uint32_t exp_uabdl2_16[4] = {60000, 59999, 44000, 0};
        uint32_t got_uabdl2_16[4];
        vst1q_u32(got_uabdl2_16, vabdl_high_u16(vu16a, vu16b));
        CHECK_ABDL("UABDL2.H", got_uabdl2_16, exp_uabdl2_16);
        const uint32_t exp_uabal_16[4] = {65536, 65537, 50002, 104};
        const uint32_t exp_uabal2_16[4] = {60010, 60019, 44030, 40};
        uint32_t got_uabal_16[4], got_uabal2_16[4];
        uint32x4_t acc_u16 = {1, 2, 3, 4};
        vst1q_u32(got_uabal_16,
                  vabal_u16(acc_u16, vget_low_u16(vu16a), vget_low_u16(vu16b)));
        CHECK_ABDL("UABAL.H", got_uabal_16, exp_uabal_16);
        acc_u16 = (uint32x4_t){10, 20, 30, 40};
        vst1q_u32(got_uabal2_16, vabal_high_u16(acc_u16, vu16a, vu16b));
        CHECK_ABDL("UABAL2.H", got_uabal2_16, exp_uabal2_16);

        static volatile int32_t s32a_mem[4] = {
            -2147483647 - 1, 2147483647, -1, 100};
        static volatile int32_t s32b_mem[4] = {
             2147483647,-2147483647 - 1,  1,-200};
        int32_t s32a[4], s32b[4];
        for (int i = 0; i < 4; i++) {
            s32a[i] = s32a_mem[i]; s32b[i] = s32b_mem[i];
        }
        int32x4_t vs32a = vld1q_s32(s32a), vs32b = vld1q_s32(s32b);
        const int64_t exp_sabdl2_32[2] = {2, 300};
        int64_t got_sabdl2_32[2];
        vst1q_s64(got_sabdl2_32, vabdl_high_s32(vs32a, vs32b));
        CHECK_ABDL("SABDL2.S", got_sabdl2_32, exp_sabdl2_32);
        const int64_t exp_sabal_32[2] = {4294967296LL, 4294967297LL};
        const int64_t exp_sabal2_32[2] = {12, 320};
        int64_t got_sabal_32[2], got_sabal2_32[2];
        int64x2_t acc_s32 = {1, 2};
        vst1q_s64(got_sabal_32,
                  vabal_s32(acc_s32, vget_low_s32(vs32a), vget_low_s32(vs32b)));
        CHECK_ABDL("SABAL.S", got_sabal_32, exp_sabal_32);
        acc_s32 = (int64x2_t){10, 20};
        vst1q_s64(got_sabal2_32, vabal_high_s32(acc_s32, vs32a, vs32b));
        CHECK_ABDL("SABAL2.S", got_sabal2_32, exp_sabal2_32);

        static volatile uint32_t u32a_mem[4] = {UINT32_MAX, 0, 100, UINT32_MAX};
        static volatile uint32_t u32b_mem[4] = {0, UINT32_MAX, 200, 0};
        uint32_t u32a[4], u32b[4];
        for (int i = 0; i < 4; i++) {
            u32a[i] = u32a_mem[i]; u32b[i] = u32b_mem[i];
        }
        uint32x4_t vu32a = vld1q_u32(u32a), vu32b = vld1q_u32(u32b);
        const uint64_t exp_uabdl2_32[2] = {100, UINT32_MAX};
        uint64_t got_uabdl2_32[2];
        vst1q_u64(got_uabdl2_32, vabdl_high_u32(vu32a, vu32b));
        CHECK_ABDL("UABDL2.S", got_uabdl2_32, exp_uabdl2_32);
        const uint64_t exp_uabal_32[2] = {4294967296ULL, 4294967297ULL};
        const uint64_t exp_uabal2_32[2] = {110, 4294967315ULL};
        uint64_t got_uabal_32[2], got_uabal2_32[2];
        uint64x2_t acc_u32 = {1, 2};
        vst1q_u64(got_uabal_32,
                  vabal_u32(acc_u32, vget_low_u32(vu32a), vget_low_u32(vu32b)));
        CHECK_ABDL("UABAL.S", got_uabal_32, exp_uabal_32);
        acc_u32 = (uint64x2_t){10, 20};
        vst1q_u64(got_uabal2_32, vabal_high_u32(acc_u32, vu32a, vu32b));
        CHECK_ABDL("UABAL2.S", got_uabal2_32, exp_uabal2_32);
    }
#undef CHECK_ABDL
    printf("PASS\n");

    printf("\ntest_simd_arith: ALL PASS\n");
    return 0;
}
