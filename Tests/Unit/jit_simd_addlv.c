#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REDUCE(word, dest) do { \
    __asm__ volatile("ldr q0, [%1]\n\tmovi v1.16b, #255\n\t.inst " #word \
                     "\n\tstr q" #dest ", [%0]" \
                     : : "r"(out), "r"(in) : "v0", "v1", "memory"); \
} while (0)
#define FORM(base) do { \
    if (alias) { REDUCE(base, 0); } else { REDUCE(base + 1, 1); } \
} while (0)
static void reduce(const uint8_t* in, uint64_t* out, int size, int q, int uns, int alias) {
    switch ((size << 2) | (q << 1) | uns) {
    case 0: FORM(0x0e303800); break;
    case 1: FORM(0x2e303800); break;
    case 2: FORM(0x4e303800); break;
    case 3: FORM(0x6e303800); break;
    case 4: FORM(0x0e703800); break;
    case 5: FORM(0x2e703800); break;
    case 6: FORM(0x4e703800); break;
    case 7: FORM(0x6e703800); break;
    case 10: FORM(0x4eb03800); break;
    case 11: FORM(0x6eb03800); break;
    }
}
#define PAIR(word, dest) do { \
    __asm__ volatile("ldr q0, [%1]\n\tldr q1, [%2]\n\tmovi v2.16b, #255\n\t.inst " #word \
                     "\n\tstr q" #dest ", [%0]" \
                     : : "r"(out), "r"(a), "r"(b) : "v0", "v1", "v2", "memory"); \
} while (0)
#define PAIR_FORM(base) do { \
    if (alias == 0) { PAIR(base, 0); } \
    else if (alias == 1) { PAIR(base + 1, 1); } \
    else { PAIR(base + 2, 2); } \
} while (0)
static void pair(const uint8_t* a, const uint8_t* b, uint8_t* out, int size, int q, int alias) {
    switch ((size << 1) | q) {
    case 0: PAIR_FORM(0x0e21bc00); break;
    case 1: PAIR_FORM(0x4e21bc00); break;
    case 2: PAIR_FORM(0x0e61bc00); break;
    case 3: PAIR_FORM(0x4e61bc00); break;
    case 4: PAIR_FORM(0x0ea1bc00); break;
    case 5: PAIR_FORM(0x4ea1bc00); break;
    case 7: PAIR_FORM(0x4ee1bc00); break;
    }
}
int main(void) {
    unsigned checks = 0;
    for (int size = 0; size < 3; ++size)
    for (int q = 0; q < 2; ++q) {
        if (size == 2 && !q) continue;
        const int bits = 8 << size;
        const int bytes = 1 << size;
        const int lanes = (q ? 16 : 8) / bytes;
        for (int uns = 0; uns < 2; ++uns)
        for (int alias = 0; alias < 2; ++alias)
        for (int pattern = 0; pattern < 64; ++pattern) {
            uint8_t in[16];
            memset(in, 0xff, sizeof(in)); // high half must be ignored for Q=0
            int64_t sum = 0;
            for (int lane = 0; lane < lanes; ++lane) {
                uint64_t value = pattern == 0 ? 0 : pattern == 1 ? 8 :
                    pattern == 2 ? (1ULL << bits) - 1 : pattern == 3 ? 1ULL << (bits - 1) :
                    ((uint64_t)pattern * 0x9e3779b9u + lane * 0x1234567u) & ((1ULL << bits) - 1);
                memcpy(in + lane * bytes, &value, bytes);
                sum += !uns && (value & (1ULL << (bits - 1))) ?
                    (int64_t)value - (1LL << bits) : (int64_t)value;
            }
            uint64_t out[2] = {0, 0};
            reduce(in, out, size, q, uns, alias);
            uint64_t want = (uint64_t)sum & (bits == 32 ? ~0ULL : (1ULL << (bits * 2)) - 1);
            if (out[0] != want || out[1]) {
                printf("addlv fail size=%d q=%d unsigned=%d alias=%d pattern=%d got=%llx:%llx want=%llx\n",
                    size,q,uns,alias,pattern,(unsigned long long)out[1],
                    (unsigned long long)out[0],(unsigned long long)want);
                return 1;
            }
            ++checks;
        }
    }
    for (int size = 0; size < 4; ++size)
    for (int q = 0; q < 2; ++q) {
        if (size == 3 && !q) continue;
        const int bytes = 1 << size;
        const int lanes = (q ? 16 : 8) / bytes;
        for (int alias = 0; alias < 3; ++alias)
        for (int pattern = 0; pattern < 64; ++pattern) {
            uint8_t a[16], b[16], out[16], expected[16] = {0};
            memset(a, 0xff, sizeof(a));
            memset(b, 0xff, sizeof(b));
            for (int source = 0; source < 2; ++source) {
                uint8_t* input = source ? b : a;
                for (int lane = 0; lane < lanes; ++lane) {
                    uint64_t value = pattern < 4 ? ~0ULL - pattern :
                        (uint64_t)pattern * 0xa0b1c2d312345678ULL + source * 17 + lane * 0x91837465ULL;
                    memcpy(input + lane * bytes, &value, bytes);
                }
                for (int i = 0; i < lanes / 2; ++i) {
                    uint64_t first = 0, second = 0;
                    memcpy(&first, input + 2 * i * bytes, bytes);
                    memcpy(&second, input + (2 * i + 1) * bytes, bytes);
                    uint64_t value = first + second;
                    memcpy(expected + (source * lanes / 2 + i) * bytes, &value, bytes);
                }
            }
            pair(a,b,out,size,q,alias);
            if (memcmp(out, expected, sizeof(out))) {
                printf("addp fail size=%d q=%d alias=%d pattern=%d\n",size,q,alias,pattern);
                return 1;
            }
            ++checks;
        }
    }
    printf("jit_simd_addlv: %u checks passed\n", checks);
    return 0;
}
