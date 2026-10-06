#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MUL(word, dest) do { \
    __asm__ volatile("ldr q0, [%1]\n\tldr q1, [%2]\n\tmovi v2.16b, #255\n\t.inst " #word \
                     "\n\tstr q" #dest ", [%0]" \
                     : : "r"(out), "r"(a), "r"(b) : "v0", "v1", "v2", "memory"); \
} while (0)
#define FORM(base) do { \
    if (alias == 0) { MUL(base, 0); } \
    else if (alias == 1) { MUL(base + 1, 1); } \
    else { MUL(base + 2, 2); } \
} while (0)

static void multiply(const uint8_t* a, const uint8_t* b, uint8_t* out,
                     int wide, int high, int alias) {
    switch ((wide << 1) | high) {
    case 0: FORM(0x0e21e000); break;
    case 1: FORM(0x4e21e000); break;
    case 2: FORM(0x0ee1e000); break;
    case 3: FORM(0x4ee1e000); break;
    }
}

int main(void) {
    unsigned checks = 0;
    for (int wide = 0; wide < 2; ++wide)
    for (int high = 0; high < 2; ++high)
    for (int alias = 0; alias < 3; ++alias)
    for (unsigned pattern = 0; pattern < 512; ++pattern) {
        const int bytes = wide ? 8 : 1;
        uint8_t a[16], b[16], out[16], expected[16] = {};
        for (int lane = 0; lane < 16 / bytes; ++lane) {
            uint64_t x = pattern == 0 ? 0 : pattern == 1 ? ~0ULL :
                pattern == 2 ? 1ULL << (bytes * 8 - 1) :
                pattern * 0x123456789e3779b9ULL + lane * 0xa0b1c2d3ULL;
            uint64_t y = pattern < 3 ? ~0ULL :
                pattern * 0x918374652dc3b4a5ULL + lane * 0x43215678ULL;
            memcpy(a + lane * bytes, &x, bytes);
            memcpy(b + lane * bytes, &y, bytes);
        }
        for (int lane = 0; lane < 8 / bytes; ++lane) {
            uint64_t x = 0, y = 0;
            memcpy(&x, a + high * 8 + lane * bytes, bytes);
            memcpy(&y, b + high * 8 + lane * bytes, bytes);
            __uint128_t product = 0;
            for (int bit = 0; bit < bytes * 8; ++bit)
                if ((x >> bit) & 1) product ^= (__uint128_t)y << bit;
            memcpy(expected + lane * bytes * 2, &product, bytes * 2);
        }
        multiply(a, b, out, wide, high, alias);
        if (memcmp(expected, out, 16)) {
            printf("pmull FAIL wide=%d high=%d alias=%d pattern=%u\n",
                   wide, high, alias, pattern);
            return 1;
        }
        ++checks;
    }
    printf("jit_simd_pmull: %u checks passed\n", checks);
    return 0;
}
