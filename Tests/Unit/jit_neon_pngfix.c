// Regressions for SIMD paths exercised by libpng's RGB filters and RGBA stores.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(ok, name) do { \
    checks++; \
    if (ok) printf("OK:   %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static void test_ext_8b_crossing_boundary(void) {
    uint8_t a[16], b[16], out[8], expected[8];
    for (int i = 0; i < 8; i++) {
        a[i] = (uint8_t)(0x10 + i);
        b[i] = (uint8_t)(0x80 + i);
    }
    const uint8_t exp[8] = { 0x16, 0x17, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85 };
    memcpy(expected, exp, sizeof(exp));
    // Poison the architecturally unused high halves: Q=0 EXT must concatenate
    // A's low 8 bytes directly with B's low 8 bytes.
    memset(a + 8, 0xE1, 8);
    memset(b + 8, 0xE2, 8);
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "ext v2.8b, v0.8b, v1.8b, #6\n"
        "str d2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out)
        : "v0", "v1", "v2", "memory");
    if (memcmp(out, expected, sizeof(out)) != 0) {
        printf("ext8 got:"); for (int i = 0; i < 8; i++) printf(" %02x", out[i]);
        printf(" want:"); for (int i = 0; i < 8; i++) printf(" %02x", expected[i]); printf("\n");
    }
    CHECK(memcmp(out, expected, sizeof(out)) == 0, "ext_8b_crosses_into_vm_low");
}

static void test_ext_16b_crossing_boundary(void) {
    uint8_t a[16], b[16], out[16], expected[16];
    for (int i = 0; i < 16; i++) {
        a[i] = (uint8_t)(i + 1);
        b[i] = (uint8_t)(0xA0 + i);
    }
    const uint8_t exp[16] = {
        15, 16, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
        0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD
    };
    memcpy(expected, exp, sizeof(exp));
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "ext v2.16b, v0.16b, v1.16b, #14\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out)
        : "v0", "v1", "v2", "memory");
    if (memcmp(out, expected, sizeof(out)) != 0) {
        printf("ext16 got:"); for (int i = 0; i < 16; i++) printf(" %02x", out[i]);
        printf(" want:"); for (int i = 0; i < 16; i++) printf(" %02x", expected[i]); printf("\n");
    }
    CHECK(memcmp(out, expected, sizeof(out)) == 0, "ext_16b_crosses_into_vm");
}

static void test_ld_st4_lane(void) {
    uint8_t input[4] = { 0x31, 0x52, 0x73, 0x94 };
    uint8_t stored[4] = {0};
    uint8_t lane_src[4][16];
    uint8_t out[4][16];
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 16; i++)
            lane_src[r][i] = (uint8_t)(0x20 + r * 16 + i);
    memset(out, 0xCC, sizeof(out));
    __asm__ volatile (
        "ld1 {v0.16b}, [%[p0]]\n"
        "ld1 {v1.16b}, [%[p1]]\n"
        "ld1 {v2.16b}, [%[p2]]\n"
        "ld1 {v3.16b}, [%[p3]]\n"
        "st4 {v0.b-v3.b}[3], [%[stored]]\n"
        "ld1 {v4.16b}, [%[o0]]\n"
        "ld1 {v5.16b}, [%[o1]]\n"
        "ld1 {v6.16b}, [%[o2]]\n"
        "ld1 {v7.16b}, [%[o3]]\n"
        "ld4 {v4.b-v7.b}[3], [%[input]]\n"
        "st1 {v4.16b}, [%[o0]]\n"
        "st1 {v5.16b}, [%[o1]]\n"
        "st1 {v6.16b}, [%[o2]]\n"
        "st1 {v7.16b}, [%[o3]]\n"
        :: [p0]"r"(lane_src[0]), [p1]"r"(lane_src[1]),
           [p2]"r"(lane_src[2]), [p3]"r"(lane_src[3]),
           [stored]"r"(stored), [input]"r"(input),
           [o0]"r"(out[0]), [o1]"r"(out[1]),
           [o2]"r"(out[2]), [o3]"r"(out[3])
        : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
    CHECK(stored[0] == lane_src[0][3] && stored[1] == lane_src[1][3] &&
          stored[2] == lane_src[2][3] && stored[3] == lane_src[3][3],
          "st4_lane_writes_four_registers");
    if (!(stored[0] == lane_src[0][3] && stored[1] == lane_src[1][3] &&
          stored[2] == lane_src[2][3] && stored[3] == lane_src[3][3]))
        printf("st4 got %02x %02x %02x %02x expected %02x %02x %02x %02x\n",
               stored[0], stored[1], stored[2], stored[3], lane_src[0][3],
               lane_src[1][3], lane_src[2][3], lane_src[3][3]);
    int loads_ok = 1;
    for (int r = 0; r < 4; r++) {
        if (out[r][3] != input[r]) loads_ok = 0;
        for (int i = 0; i < 16; i++)
            if (i != 3 && out[r][i] != 0xCC) loads_ok = 0;
    }
    CHECK(loads_ok, "ld4_lane_updates_four_registers_only");
    if (!loads_ok)
        for (int r = 0; r < 4; r++) {
            printf("ld4 v%d:", r);
            for (int i = 0; i < 16; i++) printf(" %02x", out[r][i]);
            printf("\n");
        }
}

static void test_ld3r(void) {
    uint8_t source[3] = { 0x19, 0xA5, 0xE3 };
    uint8_t out[3][8];
    __asm__ volatile (
        "ld3r {v0.8b-v2.8b}, [%[src]]\n"
        "st1 {v0.8b}, [%[o0]]\n"
        "st1 {v1.8b}, [%[o1]]\n"
        "st1 {v2.8b}, [%[o2]]\n"
        :: [src]"r"(source), [o0]"r"(out[0]),
           [o1]"r"(out[1]), [o2]"r"(out[2])
        : "v0", "v1", "v2", "memory");
    int ok = 1;
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 8; i++)
            if (out[r][i] != source[r]) ok = 0;
    CHECK(ok, "ld3r_replicates_three_registers");
}

static void test_abdl_byte_lanes(void) {
    int8_t sa[16] = { -128, -1, 0, 1, 127, 64, -64, 7,
                       120, -120, 30, -30, 2, -2, 100, -100 };
    int8_t sb[16] = {  127,  1, 0, -1, -128, -64, 64, -8,
                      -120, 120, -30, 30, -3, 3, -100, 100 };
    uint8_t ua[16], ub[16];
    uint16_t sout[8], uout[8];
    uint8_t uabd_out[16];
    for (int i = 0; i < 16; i++) {
        ua[i] = (uint8_t)(i * 17 + 3);
        ub[i] = (uint8_t)(255 - i * 13);
    }
    __asm__ volatile (
        "ldr q0, [%[sa]]\n"
        "ldr q1, [%[sb]]\n"
        "sabd v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[sout]]\n"
        "uabd v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[uabd_out]]\n"
        "ldr q0, [%[ua]]\n"
        "ldr q1, [%[ub]]\n"
        "uabd v2.16b, v0.16b, v1.16b\n"
        "uabdl v3.8h, v0.8b, v1.8b\n"
        "str q3, [%[uout]]\n"
        :: [sa]"r"(sa), [sb]"r"(sb), [sout]"r"(sout),
           [ua]"r"(ua), [ub]"r"(ub), [uout]"r"(uout),
           [uabd_out]"r"(uabd_out)
        : "v0", "v1", "v2", "v3", "memory");
    int8_t expected_s[16];
    for (int i = 0; i < 16; i++) {
        int d = (int)sa[i] - (int)sb[i];
        expected_s[i] = (int8_t)(d < 0 ? -d : d);
    }
    uint16_t expected_u[8];
    uint8_t expected_uabd[16];
    for (int i = 0; i < 16; i++) {
        int d = (int)(uint8_t)sa[i] - (int)(uint8_t)sb[i];
        expected_uabd[i] = (uint8_t)(d < 0 ? -d : d);
    }
    for (int i = 0; i < 8; i++) {
        int d = (int)ua[i] - (int)ub[i];
        expected_u[i] = (uint16_t)(d < 0 ? -d : d);
    }
    if (memcmp(sout, expected_s, sizeof(expected_s)) != 0) {
        printf("sabd got:"); for (int i = 0; i < 16; i++) printf(" %02x", ((uint8_t *)sout)[i]);
        printf(" want:"); for (int i = 0; i < 16; i++) printf(" %02x", (uint8_t)expected_s[i]); printf("\n");
    }
    CHECK(memcmp(sout, expected_s, sizeof(expected_s)) == 0, "sabd_signed_byte_lanes");
    CHECK(memcmp(uabd_out, expected_uabd, sizeof(expected_uabd)) == 0,
          "uabd_unsigned_byte_lanes");
    CHECK(memcmp(uout, expected_u, sizeof(expected_u)) == 0, "uabdl_unsigned_byte_lanes");
}

int main(void) {
    test_ext_8b_crossing_boundary();
    test_ext_16b_crossing_boundary();
    test_ld_st4_lane();
    test_ld3r();
    test_abdl_byte_lanes();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
