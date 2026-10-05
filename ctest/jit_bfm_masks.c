#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    const uint64_t values[] = {0, ~0ULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
                              0x10000, 0x8000000000000001ULL};
    uint32_t* code = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    unsigned slot = 0;
    for (unsigned width = 32; width <= 64; width += 32)
        for (unsigned r = 0; r < width; ++r)
            for (unsigned s = 0; s < width; ++s) {
                code[slot++] = (width == 64 ? 0xb3400020u : 0x33000020u) | (r << 16) | (s << 10);
                code[slot++] = 0xd65f03c0u;
            }
    __builtin___clear_cache((char*)code, (char*)(code + slot));
    slot = 0;
    unsigned checks = 0;
    for (unsigned width = 32; width <= 64; width += 32) {
        uint64_t container = width == 64 ? ~0ULL : 0xffffffffULL;
        for (unsigned r = 0; r < width; ++r)
            for (unsigned s = 0; s < width; ++s) {
                uint64_t (*fn)(uint64_t, uint64_t) = (void*)(code + slot);
                slot += 2;
                unsigned bits = s >= r ? s - r + 1 : s + 1;
                unsigned position = s >= r ? 0 : width - r;
                uint64_t field = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
                uint64_t mask = field << position;
                for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i)
                    for (unsigned j = 0; j < sizeof(values)/sizeof(values[0]); ++j) {
                        uint64_t src = values[j] & container;
                        uint64_t insert = ((s >= r ? src >> r : src) & field) << position;
                        uint64_t want = ((values[i] & ~mask) | insert) & container;
                        uint64_t got = fn(values[i], values[j]);
                        ++checks;
                        if (got != want) {
                            printf("BFM %u r=%u s=%u got=%llx want=%llx\n", width, r, s,
                                   (unsigned long long)got, (unsigned long long)want);
                            return 1;
                        }
                    }
            }
    }
    munmap(code, 65536);
    printf("jit_bfm_masks: %u checks passed\n", checks);
}
