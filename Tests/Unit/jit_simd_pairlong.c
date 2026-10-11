#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAIR(word, dest) do { \
    __asm__ volatile("ldr q0, [%1]\n\tldr q1, [%2]\n\t.inst " #word \
                     "\n\tstr q" #dest ", [%0]" \
                     : : "r"(out), "r"(source), "r"(accumulator) \
                     : "v0", "v1", "memory"); \
} while (0)
#define FORM(base) do { \
    if (alias) { PAIR(base, 0); } else { PAIR(base + 1, 1); } \
} while (0)

static void pairlong(const uint8_t* source, const uint8_t* accumulator,
                     uint8_t* out, int size, int q, int uns, int acc, int alias) {
    switch ((size << 3) | (q << 2) | (uns << 1) | acc) {
    case 0: FORM(0x0e202800); break;
    case 1: FORM(0x0e206800); break;
    case 2: FORM(0x2e202800); break;
    case 3: FORM(0x2e206800); break;
    case 4: FORM(0x4e202800); break;
    case 5: FORM(0x4e206800); break;
    case 6: FORM(0x6e202800); break;
    case 7: FORM(0x6e206800); break;
    case 8: FORM(0x0e602800); break;
    case 9: FORM(0x0e606800); break;
    case 10: FORM(0x2e602800); break;
    case 11: FORM(0x2e606800); break;
    case 12: FORM(0x4e602800); break;
    case 13: FORM(0x4e606800); break;
    case 14: FORM(0x6e602800); break;
    case 15: FORM(0x6e606800); break;
    case 16: FORM(0x0ea02800); break;
    case 17: FORM(0x0ea06800); break;
    case 18: FORM(0x2ea02800); break;
    case 19: FORM(0x2ea06800); break;
    case 20: FORM(0x4ea02800); break;
    case 21: FORM(0x4ea06800); break;
    case 22: FORM(0x6ea02800); break;
    case 23: FORM(0x6ea06800); break;
    }
}

static int64_t signed_lane(const uint8_t* data, int bytes) {
    if (bytes == 1) { int8_t v; memcpy(&v, data, 1); return v; }
    if (bytes == 2) { int16_t v; memcpy(&v, data, 2); return v; }
    int32_t v; memcpy(&v, data, 4); return v;
}

int main(void) {
    unsigned checks = 0;
    for (int size = 0; size < 3; ++size)
    for (int q = 0; q < 2; ++q)
    for (int uns = 0; uns < 2; ++uns)
    for (int acc = 0; acc < 2; ++acc)
    for (int alias = 0; alias < 2; ++alias)
    for (unsigned pattern = 0; pattern < 64; ++pattern) {
        const int bytes = 1 << size, wide = 2 * bytes;
        const int pairs = (q ? 16 : 8) / wide;
        uint8_t source[16], accumulator[16], expected[16] = {}, out[16];
        for (int lane = 0; lane < 16 / bytes; ++lane) {
            uint64_t value = pattern == 0 ? 0 : pattern == 1 ? ~0ULL :
                pattern == 2 ? 1ULL << (8 * bytes - 1) :
                pattern == 3 ? (1ULL << (8 * bytes - 1)) - 1 :
                pattern * 0x918374659e3779b9ULL + lane * 0x1234567ULL;
            memcpy(source + lane * bytes, &value, bytes);
        }
        for (int lane = 0; lane < 16 / wide; ++lane) {
            const uint64_t value = ~0ULL - pattern - lane;
            memcpy(accumulator + lane * wide, &value, wide);
        }
        for (int lane = 0; lane < pairs; ++lane) {
            uint64_t value = 0;
            if (acc) memcpy(&value, (alias ? source : accumulator) + lane * wide, wide);
            for (int element = 0; element < 2; ++element) {
                const uint8_t* p = source + (lane * 2 + element) * bytes;
                if (uns) {
                    uint64_t input = 0; memcpy(&input, p, bytes); value += input;
                } else value += (uint64_t)signed_lane(p, bytes);
            }
            memcpy(expected + lane * wide, &value, wide);
        }
        pairlong(source, accumulator, out, size, q, uns, acc, alias);
        if (memcmp(out, expected, 16)) {
            printf("pairlong FAIL size=%d q=%d unsigned=%d acc=%d alias=%d pattern=%u\n",
                   size, q, uns, acc, alias, pattern);
            return 1;
        }
        ++checks;
    }
    printf("jit_simd_pairlong: %u checks passed\n", checks);
    return 0;
}
