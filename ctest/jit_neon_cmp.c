/*
 * jit_neon_cmp.c — NEON integer compare correctness tests.
 *
 * Exercises the SIMD 3-register compare family — CMGT (signed >),
 * CMGE (signed >=), CMEQ (==), CMHI (unsigned >), CMHS (unsigned >=) —
 * which the JIT lowers natively to SSE2 PCMPGT/PCMPEQ (SSE4.1 pcmpeqq /
 * SSE4.2 pcmpgtq for 64-bit elements).
 *
 * Each sub-test feeds boundary-heavy lane data (INT_MIN/INT_MAX, 0,
 * sign-bit 0x80.. values that expose the unsigned sign-flip trick) and
 * validates every result lane against a scalar C reference. Both the
 * 128-bit (Q=1) and 64-bit (Q=0) forms are covered — the Q=0 forms
 * also verify the interpreter's "v_hi zeroed" semantics.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

static int failures = 0;

static int64_t ld_s(const uint8_t* p, int esize) {
    switch (esize) {
        case 1: return (int8_t)p[0];
        case 2: return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
        case 4: return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        default: { int64_t v; memcpy(&v, p, 8); return v; }
    }
}
static uint64_t ld_u(const uint8_t* p, int esize) {
    switch (esize) {
        case 1: return p[0];
        case 2: return p[0] | ((uint16_t)p[1] << 8);
        case 4: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        default: { uint64_t v; memcpy(&v, p, 8); return v; }
    }
}
static int pred_gt_s(const uint8_t* a, const uint8_t* b, int es) { return ld_s(a, es) > ld_s(b, es); }
static int pred_ge_s(const uint8_t* a, const uint8_t* b, int es) { return ld_s(a, es) >= ld_s(b, es); }
static int pred_gt_u(const uint8_t* a, const uint8_t* b, int es) { return ld_u(a, es) > ld_u(b, es); }
static int pred_ge_u(const uint8_t* a, const uint8_t* b, int es) { return ld_u(a, es) >= ld_u(b, es); }
static int pred_eq(const uint8_t* a, const uint8_t* b, int es) { return ld_u(a, es) == ld_u(b, es); }

static void check_lanes(const char* name, const uint8_t* out, const uint8_t* a,
                        const uint8_t* b, int esize, int lanes,
                        int (*pred)(const uint8_t*, const uint8_t*, int)) {
    int ok = 1;
    for (int i = 0; i < lanes && ok; i++) {
        uint8_t exp[8];
        memset(exp, pred(a + i * esize, b + i * esize, esize) ? 0xFF : 0x00, esize);
        if (memcmp(out + i * esize, exp, esize) != 0) ok = 0;
    }
    if (ok) printf("ok %s\n", name);
    else    { printf("NG %s\n", name); failures++; }
}

int main(void) {
    /* ── Signed inputs (boundaries: INT_MIN/INT_MAX, 0, equal lanes) ── */
    int8_t  s8a[16]  = {127, -128, 0, -1, 100, -100, 5, 5, -1, 1, 0, -128, 127, -1, 0, 0};
    int8_t  s8b[16]  = {5, -5, 0, 1, 100, -100, -5, 5, 0, 0, 1, 127, -128, 0, -1, 0};
    int16_t s16a[8]  = {32767, -32768, 0, -1, 1000, -1000, 5, 5};
    int16_t s16b[8]  = {5, -5, 0, 1, 1000, -1000, -5, 5};
    int32_t s32a[4]  = {INT32_MAX, INT32_MIN, 0, -1};
    int32_t s32b[4]  = {5, -5, 0, 1};
    int64_t s64a[2]  = {INT64_MAX, INT64_MIN};
    int64_t s64b[2]  = {5, -5};
    uint8_t o8[16] = {0}, o16[16] = {0}, o32[16] = {0}, o64[16] = {0};

    /* ── Unsigned inputs (expose the sign-flip trick: 0x80.. high bits) ── */
    uint8_t  u8a[16] = {255, 0, 128, 127, 200, 100, 5, 5, 254, 1, 128, 129, 0, 255, 7, 8};
    uint8_t  u8b[16] = {5, 0, 127, 128, 100, 200, 5, 6, 1, 254, 129, 128, 0, 0, 8, 7};
    uint16_t u16a[8] = {65535, 0, 32768, 32767, 50000, 1000, 5, 5};
    uint16_t u16b[8] = {5, 0, 32767, 32768, 1000, 50000, 5, 6};
    uint32_t u32a[4] = {UINT32_MAX, 0, 0x80000000u, 0x7FFFFFFFu};
    uint32_t u32b[4] = {5, 0, 0x7FFFFFFFu, 0x80000000u};
    uint64_t u64a[2] = {UINT64_MAX, 0};
    uint64_t u64b[2] = {5, 0};

    /* ── Signed compares, Q=1 (128-bit) ── */
    vst1q_u8((int8_t*)o8,  vcgtq_s8(vld1q_s8(s8a), vld1q_s8(s8b)));
    check_lanes("cmgt_s8_q", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 16, pred_gt_s);
    vst1q_u8((int8_t*)o8,  vcgeq_s8(vld1q_s8(s8a), vld1q_s8(s8b)));
    check_lanes("cmge_s8_q", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 16, pred_ge_s);
    vst1q_u8((int8_t*)o8,  vceqq_s8(vld1q_s8(s8a), vld1q_s8(s8b)));
    check_lanes("cmeq_s8_q", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 16, pred_eq);

    vst1q_u16((int16_t*)o16, vcgtq_s16(vld1q_s16(s16a), vld1q_s16(s16b)));
    check_lanes("cmgt_s16_q", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 8, pred_gt_s);
    vst1q_u16((int16_t*)o16, vcgeq_s16(vld1q_s16(s16a), vld1q_s16(s16b)));
    check_lanes("cmge_s16_q", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 8, pred_ge_s);
    vst1q_u16((int16_t*)o16, vceqq_s16(vld1q_s16(s16a), vld1q_s16(s16b)));
    check_lanes("cmeq_s16_q", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 8, pred_eq);

    vst1q_u32((int32_t*)o32, vcgtq_s32(vld1q_s32(s32a), vld1q_s32(s32b)));
    check_lanes("cmgt_s32_q", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 4, pred_gt_s);
    vst1q_u32((int32_t*)o32, vcgeq_s32(vld1q_s32(s32a), vld1q_s32(s32b)));
    check_lanes("cmge_s32_q", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 4, pred_ge_s);
    vst1q_u32((int32_t*)o32, vceqq_s32(vld1q_s32(s32a), vld1q_s32(s32b)));
    check_lanes("cmeq_s32_q", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 4, pred_eq);

    vst1q_u64((int64_t*)o64, vcgtq_s64(vld1q_s64(s64a), vld1q_s64(s64b)));
    check_lanes("cmgt_s64_q", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 2, pred_gt_s);
    vst1q_u64((int64_t*)o64, vcgeq_s64(vld1q_s64(s64a), vld1q_s64(s64b)));
    check_lanes("cmge_s64_q", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 2, pred_ge_s);
    vst1q_u64((int64_t*)o64, vceqq_s64(vld1q_s64(s64a), vld1q_s64(s64b)));
    check_lanes("cmeq_s64_q", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 2, pred_eq);

    /* ── Unsigned compares, Q=1 (sign-flip trick paths) ── */
    vst1q_u8((uint8_t*)o8,  vcgtq_u8(vld1q_u8(u8a), vld1q_u8(u8b)));
    check_lanes("cmhi_u8_q", o8, (const uint8_t*)u8a, (const uint8_t*)u8b, 1, 16, pred_gt_u);
    vst1q_u8((uint8_t*)o8,  vcgeq_u8(vld1q_u8(u8a), vld1q_u8(u8b)));
    check_lanes("cmhs_u8_q", o8, (const uint8_t*)u8a, (const uint8_t*)u8b, 1, 16, pred_ge_u);

    vst1q_u16((uint16_t*)o16, vcgtq_u16(vld1q_u16(u16a), vld1q_u16(u16b)));
    check_lanes("cmhi_u16_q", o16, (const uint8_t*)u16a, (const uint8_t*)u16b, 2, 8, pred_gt_u);
    vst1q_u16((uint16_t*)o16, vcgeq_u16(vld1q_u16(u16a), vld1q_u16(u16b)));
    check_lanes("cmhs_u16_q", o16, (const uint8_t*)u16a, (const uint8_t*)u16b, 2, 8, pred_ge_u);

    vst1q_u32((uint32_t*)o32, vcgtq_u32(vld1q_u32(u32a), vld1q_u32(u32b)));
    check_lanes("cmhi_u32_q", o32, (const uint8_t*)u32a, (const uint8_t*)u32b, 4, 4, pred_gt_u);
    vst1q_u32((uint32_t*)o32, vcgeq_u32(vld1q_u32(u32a), vld1q_u32(u32b)));
    check_lanes("cmhs_u32_q", o32, (const uint8_t*)u32a, (const uint8_t*)u32b, 4, 4, pred_ge_u);

    vst1q_u64((uint64_t*)o64, vcgtq_u64(vld1q_u64(u64a), vld1q_u64(u64b)));
    check_lanes("cmhi_u64_q", o64, (const uint8_t*)u64a, (const uint8_t*)u64b, 8, 2, pred_gt_u);
    vst1q_u64((uint64_t*)o64, vcgeq_u64(vld1q_u64(u64a), vld1q_u64(u64b)));
    check_lanes("cmhs_u64_q", o64, (const uint8_t*)u64a, (const uint8_t*)u64b, 8, 2, pred_ge_u);

    /* ── Q=0 forms (64-bit results; also verify v_hi is zeroed) ── */
    vst1_u8((int8_t*)o8,  vcgt_s8(vld1_s8(s8a), vld1_s8(s8b)));
    check_lanes("cmgt_s8", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 8, pred_gt_s);
    vst1_u8((int8_t*)o8,  vcge_s8(vld1_s8(s8a), vld1_s8(s8b)));
    check_lanes("cmge_s8", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 8, pred_ge_s);
    vst1_u8((int8_t*)o8,  vceq_s8(vld1_s8(s8a), vld1_s8(s8b)));
    check_lanes("cmeq_s8", o8, (const uint8_t*)s8a, (const uint8_t*)s8b, 1, 8, pred_eq);

    vst1_u16((int16_t*)o16, vcgt_s16(vld1_s16(s16a), vld1_s16(s16b)));
    check_lanes("cmgt_s16", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 4, pred_gt_s);
    vst1_u16((int16_t*)o16, vcge_s16(vld1_s16(s16a), vld1_s16(s16b)));
    check_lanes("cmge_s16", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 4, pred_ge_s);
    vst1_u16((int16_t*)o16, vceq_s16(vld1_s16(s16a), vld1_s16(s16b)));
    check_lanes("cmeq_s16", o16, (const uint8_t*)s16a, (const uint8_t*)s16b, 2, 4, pred_eq);

    vst1_u32((int32_t*)o32, vcgt_s32(vld1_s32(s32a), vld1_s32(s32b)));
    check_lanes("cmgt_s32", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 2, pred_gt_s);
    vst1_u32((int32_t*)o32, vcge_s32(vld1_s32(s32a), vld1_s32(s32b)));
    check_lanes("cmge_s32", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 2, pred_ge_s);
    vst1_u32((int32_t*)o32, vceq_s32(vld1_s32(s32a), vld1_s32(s32b)));
    check_lanes("cmeq_s32", o32, (const uint8_t*)s32a, (const uint8_t*)s32b, 4, 2, pred_eq);

    vst1_u64((int64_t*)o64, vcgt_s64(vld1_s64(s64a), vld1_s64(s64b)));
    check_lanes("cmgt_s64", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 1, pred_gt_s);
    vst1_u64((int64_t*)o64, vcge_s64(vld1_s64(s64a), vld1_s64(s64b)));
    check_lanes("cmge_s64", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 1, pred_ge_s);
    vst1_u64((int64_t*)o64, vceq_s64(vld1_s64(s64a), vld1_s64(s64b)));
    check_lanes("cmeq_s64", o64, (const uint8_t*)s64a, (const uint8_t*)s64b, 8, 1, pred_eq);

    vst1_u8((uint8_t*)o8,  vcgt_u8(vld1_u8(u8a), vld1_u8(u8b)));
    check_lanes("cmhi_u8", o8, (const uint8_t*)u8a, (const uint8_t*)u8b, 1, 8, pred_gt_u);
    vst1_u8((uint8_t*)o8,  vcge_u8(vld1_u8(u8a), vld1_u8(u8b)));
    check_lanes("cmhs_u8", o8, (const uint8_t*)u8a, (const uint8_t*)u8b, 1, 8, pred_ge_u);

    vst1_u16((uint16_t*)o16, vcgt_u16(vld1_u16(u16a), vld1_u16(u16b)));
    check_lanes("cmhi_u16", o16, (const uint8_t*)u16a, (const uint8_t*)u16b, 2, 4, pred_gt_u);
    vst1_u16((uint16_t*)o16, vcge_u16(vld1_u16(u16a), vld1_u16(u16b)));
    check_lanes("cmhs_u16", o16, (const uint8_t*)u16a, (const uint8_t*)u16b, 2, 4, pred_ge_u);

    vst1_u32((uint32_t*)o32, vcgt_u32(vld1_u32(u32a), vld1_u32(u32b)));
    check_lanes("cmhi_u32", o32, (const uint8_t*)u32a, (const uint8_t*)u32b, 4, 2, pred_gt_u);
    vst1_u32((uint32_t*)o32, vcge_u32(vld1_u32(u32a), vld1_u32(u32b)));
    check_lanes("cmhs_u32", o32, (const uint8_t*)u32a, (const uint8_t*)u32b, 4, 2, pred_ge_u);

    vst1_u64((uint64_t*)o64, vcgt_u64(vld1_u64(u64a), vld1_u64(u64b)));
    check_lanes("cmhi_u64", o64, (const uint8_t*)u64a, (const uint8_t*)u64b, 8, 1, pred_gt_u);
    vst1_u64((uint64_t*)o64, vcge_u64(vld1_u64(u64a), vld1_u64(u64b)));
    check_lanes("cmhs_u64", o64, (const uint8_t*)u64a, (const uint8_t*)u64b, 8, 1, pred_ge_u);

    /* ── Q=0 64-bit forms, forced via inline asm ──
     * GCC folds the vcgt_s64 / vcgeq_u64 1-lane intrinsics into scalar
     * cmp (or constant-folds the static inputs) at -O2 and never emits the
     * SIMD_DP 64-bit vector compare, so force the exact `cmgt v0.1d` /
     * `cmhi v0.1d` instructions. Note: the ISA has NO `.1d` vector compare
     * (64-bit elements only exist as `.2d`, Q=1) — but the emulator's
     * decoder accepts Q=0 size=3 SIMD_DP and its interpreter has explicit
     * cases for them, so raw encodings still exercise the Q=0 esize=8 path.
     * The old binutils rejects the `.1d` qualifier, so emit the raw words
     * (low word = opcode<<10 | rn<<5 | rd; Rn=1 (a), Rm=2 (b)):
     *   cmgt v0.1d,v1.1d,v2.1d = 0x0EE23420   cmge = 0x0EE23C20
     *   cmeq v0.1d,v1.1d,v2.1d = 0x2EE28C20
     *   cmhi v0.1d,v1.1d,v2.1d = 0x2EE23420   cmhs = 0x2EE23C20
     * (each & 0xBF20FC00 lands on the matching INT_CMP table row.) */
    {
        const uint64_t* a = (const uint64_t*)s64a;
        const uint64_t* b = (const uint64_t*)s64b;
        const uint64_t* ua = (const uint64_t*)u64a;
        const uint64_t* ub = (const uint64_t*)u64b;
#define FORCE_1D_CMP(NAME, PA, PB, INST, REF_A, REF_B, PRED) \
        do { \
            __asm__ volatile( \
                "ldr d1, [%0]\n\t" \
                "ldr d2, [%1]\n\t" \
                ".inst " INST "\n\t" \
                "str d0, [%2]" \
                :: "r"(PA), "r"(PB), "r"(o64) \
                : "v0", "v1", "v2", "memory"); \
            check_lanes(NAME, o64, (const uint8_t*)&REF_A, \
                        (const uint8_t*)&REF_B, 8, 1, PRED); \
        } while (0)
        FORCE_1D_CMP("cmgt_s64d", a, b,    "0x0ee23420", s64a[0], s64b[0], pred_gt_s);
        FORCE_1D_CMP("cmge_s64d", a, b,    "0x0ee23c20", s64a[0], s64b[0], pred_ge_s);
        FORCE_1D_CMP("cmeq_s64d", a, b,    "0x2ee28c20", s64a[0], s64b[0], pred_eq);
        FORCE_1D_CMP("cmhi_u64d", ua, ub,  "0x2ee23420", u64a[0], u64b[0], pred_gt_u);
        FORCE_1D_CMP("cmhs_u64d", ua, ub,  "0x2ee23c20", u64a[0], u64b[0], pred_ge_u);
#undef FORCE_1D_CMP
    }

    printf("neon_cmp: %d/%d checks passed, %d failures\n", 45 - failures, 45, failures);
    return failures ? 1 : 0;
}
