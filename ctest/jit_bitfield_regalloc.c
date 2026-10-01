// Bitfield results must survive register pressure, memory helpers, and
// tier-2 side exits without an eager write to their scratch stack slots.
#include <stdint.h>
#include <stdio.h>

static uint64_t input[256], output[256];

static uint64_t reference(unsigned count) {
    uint64_t sum = 0;
    while (count) {
        const uint64_t value = input[count & 255];
        const uint64_t u = (value >> 9) & 0x1ffff;
        uint32_t s = (uint32_t)(value >> 5) & 0x7ff;
        if (s & 0x400) s |= 0xfffff800u;
        uint64_t inserted = value & 0x1ff;
        if (inserted & 0x100) inserted |= ~UINT64_C(0x1ff);
        uint32_t shifted = (uint32_t)value >> 12;
        if ((uint32_t)value & 0x80000000u) shifted |= 0xfff00000u;
        sum ^= u;
        sum += s;
        sum ^= inserted << 3;
        sum += value >> 7;
        sum += shifted;
        sum += (uint32_t)value << 3;
        sum ^= value; // source is live across several clobbering bitfields
        sum += u;
        --count;
    }
    return sum;
}

static uint64_t run(unsigned iterations) {
    register uint64_t count asm("x0") = iterations;
    register const uint64_t *src asm("x1") = input;
    register uint64_t sum asm("x2") = 0;
    register uint64_t *dst asm("x12") = output;
    __asm__ volatile(
        "1:\n"
        "cbz x0, 2f\n" // side exit plus unconditional back-edge: tier-2 region
        "and x14, x0, #255\n"
        "add x3, x1, x14, lsl #3\n"
        "ldr x4, [x3]\n"
        "ubfx x5, x4, #9, #17\n"
        "sbfx w6, w4, #5, #11\n"
        "sbfiz x7, x4, #3, #9\n"
        "lsr x8, x4, #7\n"
        "asr w9, w4, #12\n"
        "lsl w10, w4, #3\n"
        "eor x2, x2, x5\n"
        "add x2, x2, x6\n"
        "eor x2, x2, x7\n"
        "add x2, x2, x8\n"
        "add x2, x2, x9\n"
        "add x2, x2, x10\n"
        "eor x2, x2, x4\n"
        "add x13, x12, x14, lsl #3\n"
        "str x5, [x13]\n"
        "ldr x11, [x13]\n"
        "add x2, x2, x11\n"
        "sub x0, x0, #1\n"
        "b 1b\n"
        "2:\n"
        : "+r"(count), "+r"(sum)
        : "r"(src), "r"(dst)
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
          "x11", "x13", "x14", "cc", "memory");
    return sum;
}

int main(void) {
    uint64_t seed = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 256; ++i) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        input[i] = seed;
    }
    const unsigned counts[] = {1, 63, 8192, 8193};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        uint64_t want = reference(counts[i]);
        uint64_t got = run(counts[i]);
        if (got != want) {
            printf("FAIL: bitfield count=%u got=%llx want=%llx\n", counts[i],
                   (unsigned long long)got, (unsigned long long)want);
            return 1;
        }
    }
    puts("ALL PASS: bitfield regalloc, live sources, spills and region exits");
    return 0;
}
