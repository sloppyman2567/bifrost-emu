/*
 * Vector shift-by-immediate regression (v1.5.1-alpha): USRA/SSRA/SLI/SRI
 * must match the reference implementation across every element size
 * (H/S/D), across both 64-bit halves (Q=1) and the 64-bit forms (Q=0),
 * including boundary shift amounts. Covered via NEON intrinsics so the
 * compiler emits real `usra`/`ssra`/`sli`/`sri` instructions.
 */
#include <stdio.h>
#include <stdint.h>
#include <arm_neon.h>

static int failures = 0;

static void check_h(const char* msg, const uint16_t* res, const uint16_t* exp, int n) {
    for (int i = 0; i < n; i++) {
        if (res[i] != exp[i]) {
            printf("FAIL: %s lane %d (0x%04x != 0x%04x)\n", msg, i, res[i], exp[i]);
            failures++;
            return;
        }
    }
    printf("ok:   %s\n", msg);
}
static void check_s(const char* msg, const uint32_t* res, const uint32_t* exp, int n) {
    for (int i = 0; i < n; i++) {
        if (res[i] != exp[i]) {
            printf("FAIL: %s lane %d (0x%08x != 0x%08x)\n", msg, i, res[i], exp[i]);
            failures++;
            return;
        }
    }
    printf("ok:   %s\n", msg);
}
static void check_d(const char* msg, const uint64_t* res, const uint64_t* exp, int n) {
    for (int i = 0; i < n; i++) {
        if (res[i] != exp[i]) {
            printf("FAIL: %s lane %d (0x%016llx != 0x%016llx)\n", msg, i,
                   (unsigned long long)res[i], (unsigned long long)exp[i]);
            failures++;
            return;
        }
    }
    printf("ok:   %s\n", msg);
}

static uint64_t usra_lane(uint64_t n, uint64_t d, int bits, int shift, uint64_t mask) {
    uint64_t v = (shift >= 64) ? 0 : (n >> shift);
    return (d + v) & mask;
}
static uint64_t ssra_lane(uint64_t n, uint64_t d, int bits, int shift, uint64_t mask) {
    int64_t v;
    if (bits == 8)       v = (int8_t)(uint8_t)n;
    else if (bits == 16) v = (int16_t)(uint16_t)n;
    else if (bits == 32) v = (int32_t)(uint32_t)n;
    else                 v = (int64_t)n;
    if (shift >= bits) v = (v < 0) ? -1 : 0;
    else v >>= shift;
    return (d + (uint64_t)v) & mask;
}
static uint64_t sli_lane(uint64_t n, uint64_t d, int bits, int shift, uint64_t mask) {
    uint64_t hi = (shift >= 64) ? 0 : ((n << shift) & mask);
    uint64_t lo = (bits - shift < bits) ? (d >> (bits - shift)) : 0;
    return hi | lo;
}
static uint64_t sri_lane(uint64_t n, uint64_t d, int bits, int shift, uint64_t mask) {
    uint64_t lo = (shift >= 64) ? 0 : (n >> shift);
    uint64_t hi = (bits - shift < bits) ? ((d << (bits - shift)) & mask) : 0;
    return hi | lo;
}

int main(void) {
    uint64_t m16 = 0xFFFF, m32 = 0xFFFFFFFFULL, m64 = ~0ULL;
    uint16_t exp[8], res[8];

    // ── 16-bit lanes, Q=1 (8 lanes) ────────────────────────────────────
    uint16_t nh[8] = {0x0001,0x8000,0xFFFF,0x1234,0x8000,0x0002,0x7FFF,0x0000};
    uint16_t dh[8] = {0x0002,0x0001,0x0001,0x8000,0xFFFF,0x0001,0x0001,0x0010};
    {
        uint16x8_t r = vsraq_n_u16(vld1q_u16(dh), vld1q_u16(nh), 3);
        vst1q_u16(res, r);
        for (int i = 0; i < 8; i++) exp[i] = usra_lane(nh[i], dh[i], 16, 3, m16);
        check_h("usra v.8h #3", res, exp, 8);
    }
    {
        uint16x8_t r = vsliq_n_u16(vld1q_u16(dh), vld1q_u16(nh), 5);
        vst1q_u16(res, r);
        for (int i = 0; i < 8; i++) exp[i] = sli_lane(nh[i], dh[i], 16, 5, m16);
        check_h("sli v.8h #5", res, exp, 8);
    }
    {
        uint16x8_t r = vsriq_n_u16(vld1q_u16(dh), vld1q_u16(nh), 5);
        vst1q_u16(res, r);
        for (int i = 0; i < 8; i++) exp[i] = sri_lane(nh[i], dh[i], 16, 5, m16);
        check_h("sri v.8h #5", res, exp, 8);
    }
    {
        uint16x8_t r = vsriq_n_u16(vld1q_u16(dh), vld1q_u16(nh), 16);  /* shift == esize*8 */
        vst1q_u16(res, r);
        for (int i = 0; i < 8; i++) exp[i] = sri_lane(nh[i], dh[i], 16, 16, m16);
        check_h("sri v.8h #16", res, exp, 8);
    }
    {
        uint16x8_t r = vsliq_n_u16(vld1q_u16(dh), vld1q_u16(nh), 0);   /* shift == 0 */
        vst1q_u16(res, r);
        for (int i = 0; i < 8; i++) exp[i] = sli_lane(nh[i], dh[i], 16, 0, m16);
        check_h("sli v.8h #0", res, exp, 8);
    }

    // ── 32-bit lanes, Q=1 (4 lanes) ────────────────────────────────────
    uint32_t ns[4] = {0x00000001,0x80000000,0xFFFFFFFF,0x12345678};
    uint32_t ds[4] = {0x00000002,0x00000001,0x7FFFFFFF,0x80000000};
    uint32_t res32[4], exp32[4];
    {
        uint32x4_t r = vsraq_n_u32(vld1q_u32(ds), vld1q_u32(ns), 7);
        vst1q_u32(res32, r);
        for (int i = 0; i < 4; i++) exp32[i] = usra_lane(ns[i], ds[i], 32, 7, m32);
        check_s("usra v.4s #7", res32, exp32, 4);
    }
    {
        int32x4_t r = vsraq_n_s32((int32x4_t)vld1q_u32(ds), (int32x4_t)vld1q_u32(ns), 7);
        vst1q_s32((int32_t*)res32, r);
        for (int i = 0; i < 4; i++) exp32[i] = ssra_lane(ns[i], ds[i], 32, 7, m32);
        check_s("ssra v.4s #7", res32, exp32, 4);
    }
    {
        uint32x4_t r = vsliq_n_u32(vld1q_u32(ds), vld1q_u32(ns), 13);
        vst1q_u32(res32, r);
        for (int i = 0; i < 4; i++) exp32[i] = sli_lane(ns[i], ds[i], 32, 13, m32);
        check_s("sli v.4s #13", res32, exp32, 4);
    }
    {
        uint32x4_t r = vsriq_n_u32(vld1q_u32(ds), vld1q_u32(ns), 13);
        vst1q_u32(res32, r);
        for (int i = 0; i < 4; i++) exp32[i] = sri_lane(ns[i], ds[i], 32, 13, m32);
        check_s("sri v.4s #13", res32, exp32, 4);
    }
    {
        int32x4_t r = vsraq_n_s32((int32x4_t)vld1q_u32(ds), (int32x4_t)vld1q_u32(ns), 32);
        vst1q_s32((int32_t*)res32, r);
        for (int i = 0; i < 4; i++) exp32[i] = ssra_lane(ns[i], ds[i], 32, 32, m32);
        check_s("ssra v.4s #32", res32, exp32, 4);
    }

    // ── 64-bit lanes, Q=1 (2 lanes) ────────────────────────────────────
    uint64_t nd[2] = {0x0000000000000001ULL, 0x8000000000000000ULL};
    uint64_t dd[2] = {0x0000000000000002ULL, 0x7FFFFFFFFFFFFFFFULL};
    uint64_t res64[2], exp64[2];
    {
        uint64x2_t r = vsraq_n_u64(vld1q_u64(dd), vld1q_u64(nd), 9);
        vst1q_u64(res64, r);
        for (int i = 0; i < 2; i++) exp64[i] = usra_lane(nd[i], dd[i], 64, 9, m64);
        check_d("usra v.2d #9", res64, exp64, 2);
    }
    {
        uint64x2_t r = vsliq_n_u64(vld1q_u64(dd), vld1q_u64(nd), 37);
        vst1q_u64(res64, r);
        for (int i = 0; i < 2; i++) exp64[i] = sli_lane(nd[i], dd[i], 64, 37, m64);
        check_d("sli v.2d #37", res64, exp64, 2);
    }
    {
        uint64x2_t r = vsriq_n_u64(vld1q_u64(dd), vld1q_u64(nd), 37);
        vst1q_u64(res64, r);
        for (int i = 0; i < 2; i++) exp64[i] = sri_lane(nd[i], dd[i], 64, 37, m64);
        check_d("sri v.2d #37", res64, exp64, 2);
    }
    {
        int64x2_t r = vsraq_n_s64((int64x2_t)vld1q_u64(dd), (int64x2_t)vld1q_u64(nd), 9);
        vst1q_s64((int64_t*)res64, r);
        for (int i = 0; i < 2; i++) exp64[i] = ssra_lane(nd[i], dd[i], 64, 9, m64);
        check_d("ssra v.2d #9 (interp)", res64, exp64, 2);
    }
    {
        uint64x2_t r = vsraq_n_u64(vld1q_u64(dd), vld1q_u64(nd), 64);  /* USRA shift == esize*8 */
        vst1q_u64(res64, r);
        for (int i = 0; i < 2; i++) exp64[i] = usra_lane(nd[i], dd[i], 64, 64, m64);
        check_d("usra v.2d #64", res64, exp64, 2);
    }

    // ── 64-bit forms, Q=0 (low half only) ──────────────────────────────
    uint16_t nq0[4] = {0x0001,0x8000,0xFFFF,0x1234};
    uint16_t dq0[4] = {0x0002,0x0001,0x0001,0x8000};
    {
        uint16x4_t r = vsra_n_u16(vld1_u16(dq0), vld1_u16(nq0), 3);
        vst1_u16(res, r);
        for (int i = 0; i < 4; i++) exp[i] = usra_lane(nq0[i], dq0[i], 16, 3, m16);
        check_h("usra v.4h #3 (Q=0)", res, exp, 4);
    }
    {
        int16x4_t r = vsra_n_s16((int16x4_t)vld1_u16(dq0), (int16x4_t)vld1_u16(nq0), 3);
        vst1_s16((int16_t*)res, r);
        for (int i = 0; i < 4; i++) exp[i] = ssra_lane(nq0[i], dq0[i], 16, 3, m16);
        check_h("ssra v.4h #3 (Q=0)", res, exp, 4);
    }
    {
        uint16x4_t r = vsli_n_u16(vld1_u16(dq0), vld1_u16(nq0), 5);
        vst1_u16(res, r);
        for (int i = 0; i < 4; i++) exp[i] = sli_lane(nq0[i], dq0[i], 16, 5, m16);
        check_h("sli v.4h #5 (Q=0)", res, exp, 4);
    }
    {
        uint16x4_t r = vsri_n_u16(vld1_u16(dq0), vld1_u16(nq0), 5);
        vst1_u16(res, r);
        for (int i = 0; i < 4; i++) exp[i] = sri_lane(nq0[i], dq0[i], 16, 5, m16);
        check_h("sri v.4h #5 (Q=0)", res, exp, 4);
    }
    {
        uint16x4_t r = vsri_n_u16(vld1_u16(dq0), vld1_u16(nq0), 16);  /* shift == esize*8, Q=0 */
        vst1_u16(res, r);
        for (int i = 0; i < 4; i++) exp[i] = sri_lane(nq0[i], dq0[i], 16, 16, m16);
        check_h("sri v.4h #16 (Q=0)", res, exp, 4);
    }
    {
        uint32x2_t sn = vdup_n_u32(0xFFFFFFFF), sd = vdup_n_u32(0x00000001);
        uint32x2_t r = vsra_n_u32(sd, sn, 31);
        uint32_t resx = vget_lane_u32(r, 0);
        if (resx != usra_lane(0xFFFFFFFF, 0x1, 32, 31, m32)) {
            printf("FAIL: usra v.2s #31 (Q=0)\n"); failures++;
        } else printf("ok:   usra v.2s #31 (Q=0)\n");
        r = vsra_n_u32(sd, sn, 32);
        resx = vget_lane_u32(r, 0);
        if (resx != usra_lane(0xFFFFFFFF, 0x1, 32, 32, m32)) {
            printf("FAIL: usra v.2s #32 (Q=0)\n"); failures++;
        } else printf("ok:   usra v.2s #32 (Q=0)\n");
    }

    printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
