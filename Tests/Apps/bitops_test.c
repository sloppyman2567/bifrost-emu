#include <stdio.h>
#include <stdint.h>

int popcount64(uint64_t x) {
    int count = 0;
    while (x) { count += x & 1; x >>= 1; }
    return count;
}

int ctz64(uint64_t x) {
    if (x == 0) return 64;
    int count = 0;
    while (!(x & 1)) { count++; x >>= 1; }
    return count;
}

int clz64(uint64_t x) {
    if (x == 0) return 64;
    int count = 0;
    while (!(x & (1ULL << 63))) { count++; x <<= 1; }
    return count;
}

int main() {
    printf("Bit manipulation test\n");
    
    struct { uint64_t val; int pop, ctz, clz; } tests[] = {
        {0, 0, 64, 64},
        {1, 1, 0, 63},
        {0xFF, 8, 0, 56},
        {0x100, 1, 8, 55},
        {0xDEADBEEF, 24, 0, 32},
        {0x8000000000000000ULL, 1, 63, 0},
        {0xFFFFFFFFFFFFFFFFULL, 64, 0, 0},
        {0, 0, 0, 0} // sentinel
    };
    
    for (int i = 0; tests[i].val || tests[i].pop; i++) {
        uint64_t v = tests[i].val;
        int p = popcount64(v), c = ctz64(v), l = clz64(v);
        printf("  0x%016llx: popcount=%d(ctz=%d clz=%d", 
               (unsigned long long)v, p, c, l);
        if (v == 0) { printf(") OK\n"); continue; }
        printf(") %s\n", (p == tests[i].pop && c == tests[i].ctz && l == tests[i].clz) ? "OK" : "FAIL");
        if (p != tests[i].pop || c != tests[i].ctz || l != tests[i].clz) return 1;
    }
    
    // Test __builtin_ functions if available
    printf("  __builtin_popcount(0xABCD) = %d\n", __builtin_popcount(0xABCD));
    printf("  __builtin_ctz(0x100) = %d\n", __builtin_ctz(0x100));
    printf("  __builtin_clz(0x100) = %d\n", __builtin_clz(0x100));
    
    printf("PASS\n");
    return 0;
}
