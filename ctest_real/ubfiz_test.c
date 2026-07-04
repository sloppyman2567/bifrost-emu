// ubfiz_test.c — test 64-bit UBFIZ which MD5 uses for index calculation.
#include <stdio.h>
#include <stdint.h>

int main(void) {
    uint64_t v = 0x51;  // 81 decimal, like toybox MD5 uses
    uint64_t r;

    // UBFIZ X13, X6, #2, #4 — take 4 bits of X6, place at bit 2 of X13
    __asm__ volatile ("ubfiz %0, %1, #2, #4" : "=r"(r) : "r"(v));
    printf("ubfiz(0x%lx, #2, #4) = 0x%lx (exp=0x%lx)  %s\n",
           v, r, (v & 0xF) << 2, r == ((v & 0xF) << 2) ? "OK" : "FAIL");

    // Chained UBFIZ: UBFIZ X13, X13, #2, #4 (simulate toybox index computation)
    uint64_t x = 1;  // start
    for (int i = 0; i < 4; i++) {
        __asm__ volatile ("ubfiz %0, %0, #2, #4" : "+r"(x));
    }
    // Each UBFIZ takes low 4 bits and shifts to bit 2.
    // Start: x=1, after 1: (1&0xf)<<2 = 4, after 2: (4&0xf)<<2 = 16, after 3: (16&0xf)<<2 = 0, after 4: 0
    printf("chained ubfiz 4x from 1: got=0x%lx\n", x);

    // Test 32-bit UBFIZ too
    uint32_t w = 0x51;
    uint32_t wr;
    __asm__ volatile ("ubfiz %w0, %w1, #2, #4" : "=r"(wr) : "r"(w));
    printf("ubfiz32(0x%x, #2, #4) = 0x%x (exp=0x%x)  %s\n",
           w, wr, (w & 0xF) << 2, wr == ((w & 0xF) << 2) ? "OK" : "FAIL");

    return 0;
}
