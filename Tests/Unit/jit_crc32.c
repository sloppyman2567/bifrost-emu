#include <stdint.h>
#include <stdio.h>

#define CRC(word, reg) __asm__ volatile("mov x0, %2\n\tmov x1, %3\n\tmsr nzcv, %4\n\t.inst " #word \
    "\n\tmrs %1, nzcv\n\tmov %0, x" #reg \
    : "=&r"(result), "=&r"(*flags) : "r"(seed), "r"(value), "r"(before) : "x0", "x1", "x2", "cc")
#define FORM(base) do { \
    if (alias == 0) { CRC(base, 2); } \
    else if (alias == 1) { CRC(base - 2, 0); } \
    else if (alias == 2) { CRC(base - 1, 1); } \
    else { CRC(base - 0x10000 - 2, 0); } \
} while (0)
static uint64_t instruction(uint64_t seed, uint64_t value, int size, int castagnoli,
                            int alias, uint64_t before, uint64_t* flags) {
    uint64_t result;
    switch (size | (castagnoli << 2)) {
    case 0: FORM(0x1ac14002); break;
    case 1: FORM(0x1ac14402); break;
    case 2: FORM(0x1ac14802); break;
    case 3: FORM(0x9ac14c02); break;
    case 4: FORM(0x1ac15002); break;
    case 5: FORM(0x1ac15402); break;
    case 6: FORM(0x1ac15802); break;
    case 7: FORM(0x9ac15c02); break;
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
    for (int alias = 0; alias < 4; ++alias)
    for (int pattern = 0; pattern < 256; ++pattern) {
        const uint64_t seed = ((uint64_t)pattern << 40) | ((uint32_t)pattern * 0x9e3779b9u);
        const uint64_t value = pattern == 0 ? ~0ULL :
            (uint64_t)pattern * 0x1234567818374951ULL;
        const uint64_t before = (uint64_t)(pattern & 15) << 28;
        uint64_t flags;
        const uint32_t want = reference(seed, alias == 3 ? seed : value, 1 << size, poly);
        const uint64_t got = instruction(seed, value, size, poly, alias, before, &flags);
        if (got != want || flags != before) {
            printf("crc fail poly=%d size=%d alias=%d pattern=%d got=%llx want=%x flags=%llx\n",
                poly,size,alias,pattern,(unsigned long long)got,want,(unsigned long long)flags);
            return 1;
        }
        ++checks;
    }
    printf("jit_crc32: %u checks passed\n", checks);
    return 0;
}
