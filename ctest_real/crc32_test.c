#include <stdio.h>
#include <stdint.h>
#include <string.h>

uint32_t crc32_table[256];

void crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        crc32_table[i] = crc;
    }
}

uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

int main() {
    crc32_init();
    
    // Test vectors
    const char *tests[] = {"", "a", "hello", "The quick brown fox jumps over the lazy dog", NULL};
    uint32_t expected[] = {0, 0xE8B7BE43, 0x3610A686, 0x414FA339};
    
    printf("CRC32 benchmark\n");
    for (int i = 0; tests[i]; i++) {
        uint32_t result = crc32(tests[i], strlen(tests[i]));
        printf("  crc32(\"%s\") = 0x%08X %s\n", tests[i], result,
               result == expected[i] ? "OK" : "FAIL");
        if (result != expected[i]) return 1;
    }
    
    // Benchmark: 10MB of data
    static char buf[10*1024*1024];
    memset(buf, 'A', sizeof(buf));
    uint32_t r = crc32(buf, sizeof(buf));
    printf("  crc32(10MB) = 0x%08X\n", r);
    printf("PASS\n");
    return 0;
}
