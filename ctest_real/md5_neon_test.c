// md5_neon_test.c — exercises the NEON ops that toybox's MD5 uses.
//
// MD5's NEON code in toybox uses:
//   - vshrq_n_u32 / vshlq_n_u32 (vector shift right/left)
//   - vsliq_n_u32 (shift left insert) for ROTL
//   - vrev64q_u32 (reverse elements in 64-bit container) for byte-swap
//   - veorq_u32, vorrq_u32, vandq_u32 (logical)
//   - vaddq_u32 (vector add)
//   - vld1q_u32 / vst1q_u32 (load/store)
//   - vdupq_n_u32 (duplicate scalar)
//
// We test each of these and print the results so we can compare
// emulator output against native execution.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

static void print_u32x4(const char *label, uint32x4_t v) {
    uint32_t buf[4];
    vst1q_u32(buf, v);
    printf("%-20s %08x %08x %08x %08x\n", label, buf[0], buf[1], buf[2], buf[3]);
}

int main(void) {
    uint32_t a = 0x12345678, b = 0x9abcdef0, c = 0x0fedcba9, d = 0x13579bdf;
    uint32x4_t v = {a, b, c, d};
    uint32x4_t v2 = {0x04050607, 0x08090a0b, 0x0c0d0e0f, 0x10111213};
    print_u32x4("input", v);

    // 1. SHL by 4
    print_u32x4("shl 4", vshlq_n_u32(v, 4));
    // 2. USHR by 4
    print_u32x4("ushr 4", vshrq_n_u32(v, 4));
    // 3. ROTL via vsliq (rotate left by 7)
    print_u32x4("rotl 7 (sli)", vsliq_n_u32(v, v, 7));
    // 4. ROTL via shl|ushr (rotate left by 7)
    uint32x4_t rotl_or = vorrq_u32(vshlq_n_u32(v, 7), vshrq_n_u32(v, 25));
    print_u32x4("rotl 7 (or)", rotl_or);
    // 5. vrev64q_u32 (byte swap each 32-bit word)
    print_u32x4("rev64 u32", vrev64q_u32(v));
    // 6. vrev64q_u8 (full byte reverse in 64-bit)
    print_u32x4("rev64 u8", vreinterpretq_u32_u8(vrev64q_u8(vreinterpretq_u8_u32(v))));
    // 7. XOR
    print_u32x4("xor", veorq_u32(v, v2));
    // 8. ADD
    print_u32x4("add", vaddq_u32(v, v2));
    // 9. vdupq_n_u32
    print_u32x4("dup 0xdeadbeef", vdupq_n_u32(0xdeadbeef));

    // 10. MD5 round function test: F(x,y,z) = (x & y) | (~x & z)
    uint32x4_t x = v, y = v2, z = vdupq_n_u32(0xAAAAAAAA);
    uint32x4_t f = vorrq_u32(vandq_u32(x, y), vandq_u32(vmvnq_u32(x), z));
    print_u32x4("F(x,y,z)", f);

    return 0;
}
