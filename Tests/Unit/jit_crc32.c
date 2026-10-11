#include <stdint.h>
#include <stdio.h>

#define CRC(mnemonic, reg) __asm__ volatile(".arch_extension crc\n\t" mnemonic " %w0, %w1, %" reg "2" \
    : "=r"(result) : "r"(seed), "r"(value))
static uint64_t instruction(uint32_t seed, uint64_t value, int size, int castagnoli) {
    uint64_t result;
    switch (size | (castagnoli << 2)) {
    case 0: CRC("crc32b", "w"); break;
    case 1: CRC("crc32h", "w"); break;
    case 2: CRC("crc32w", "w"); break;
    case 3: CRC("crc32x", "x"); break;
    case 4: CRC("crc32cb", "w"); break;
    case 5: CRC("crc32ch", "w"); break;
    case 6: CRC("crc32cw", "w"); break;
    case 7: CRC("crc32cx", "x"); break;
    default: return 0;
    }
    return result;
}
static uint32_t reference(uint32_t seed, uint64_t value, int bytes, int castagnoli) {
    const uint32_t polynomial = castagnoli ? 0x82f63b78u : 0xedb88320u;
    for (int i = 0; i < bytes; ++i) {
        seed ^= (uint8_t)(value >> (i * 8));
        for (int bit = 0; bit < 8; ++bit)
            seed = (seed >> 1) ^ ((seed & 1) ? polynomial : 0);
    }
    return seed;
}
int main(void) {
    unsigned checks = 0;
    for (int poly = 0; poly < 2; ++poly)
    for (int size = 0; size < 4; ++size)
    for (int pattern = 0; pattern < 256; ++pattern) {
        const uint32_t seed = (uint32_t)pattern * 0x9e3779b9u;
        const uint64_t value = pattern == 0 ? ~0ULL :
            (uint64_t)pattern * 0x1234567818374951ULL;
        const uint32_t want = reference(seed, value, 1 << size, poly);
        const uint64_t got = instruction(seed, value, size, poly);
        if (got != want) {
            printf("crc fail poly=%d size=%d pattern=%d got=%llx want=%x\n",
                poly,size,pattern,(unsigned long long)got,want);
            return 1;
        }
        ++checks;
    }
    printf("jit_crc32: %u checks passed\n", checks);
    return 0;
}
