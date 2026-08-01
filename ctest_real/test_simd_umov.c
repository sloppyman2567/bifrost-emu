/*
 * UMOV regression (v1.5.1-alpha): UMOV must extract a vector element into a
 * GPR across every element size (B/H/S/D) and across both 64-bit halves
 * (Q=1). Covered via vgetq_lane_* intrinsics so the compiler emits real
 * `umov` instructions into W/X registers correctly. Expected values computed
 * from the loaded vectors.
 */
#include <stdio.h>
#include <stdint.h>
#include <arm_neon.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    uint8_t b[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                     0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    uint16_t h[8] = {0x1111,0x2222,0x3333,0x4444,0x5555,0x6666,0x7777,0x8888};
    uint32_t s[4] = {0x11112222, 0x33334444, 0x55556666, 0x77778888};
    uint64_t d[2] = {0x1111222233334444ULL, 0x5555666677778888ULL};

    uint8x16_t vb = vld1q_u8(b);
    uint16x8_t vh = vld1q_u16(h);
    uint32x4_t vs = vld1q_u32(s);
    uint64x2_t vd = vld1q_u64(d);

    CK(vgetq_lane_u8(vb, 0) == 0x00, "umov v.b[0]");
    CK(vgetq_lane_u8(vb, 7) == 0x07, "umov v.b[7]");
    CK(vgetq_lane_u8(vb, 8) == 0x08, "umov v.b[8] (hi)");
    CK(vgetq_lane_u8(vb, 15) == 0x0F, "umov v.b[15] (hi)");

    CK(vgetq_lane_u16(vh, 0) == 0x1111, "umov v.h[0]");
    CK(vgetq_lane_u16(vh, 3) == 0x4444, "umov v.h[3]");
    CK(vgetq_lane_u16(vh, 4) == 0x5555, "umov v.h[4] (hi)");
    CK(vgetq_lane_u16(vh, 7) == 0x8888, "umov v.h[7] (hi)");

    CK(vgetq_lane_u32(vs, 0) == 0x11112222, "umov v.s[0]");
    CK(vgetq_lane_u32(vs, 1) == 0x33334444, "umov v.s[1]");
    CK(vgetq_lane_u32(vs, 2) == 0x55556666, "umov v.s[2] (hi)");
    CK(vgetq_lane_u32(vs, 3) == 0x77778888, "umov v.s[3] (hi)");

    CK(vgetq_lane_u64(vd, 0) == 0x1111222233334444ULL, "umov v.d[0]");
    CK(vgetq_lane_u64(vd, 1) == 0x5555666677778888ULL, "umov v.d[1]");

    uint64x2_t v2 = vdupq_n_u64(0xFFFFFFFFULL);
    CK(vgetq_lane_u64(v2, 0) == 0xFFFFFFFFULL, "movi/umov v.2d d[0]");
    CK(vgetq_lane_u64(v2, 1) == 0xFFFFFFFFULL, "movi/umov v.2d d[1]");

    printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
