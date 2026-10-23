#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Sixteen live vectors in a self-loop: v27 and v28 share a demand slot. */
int main(void) {
    uint8_t acc[2][16];
    long n = 6;
    __asm__ volatile(
        "movi v27.16b, #0x11\n\tmovi v28.16b, #0x22\n\tmovi v29.16b, #0x33\n\t"
        "movi v20.16b, #0x40\n\tmovi v21.16b, #0x10\n\tmovi v22.16b, #3\n\tmovi v23.16b, #5\n\t"
        "movi v5.16b, #0xa5\n\tmovi v6.16b, #0x3c\n\tmovi v7.16b, #0xc3\n"
        "1:\n\t"
        "mul v25.8h, v24.8h, v18.8h\n\t"
        "mla v27.16b, v22.16b, v23.16b\n\t"
        "mla v28.16b, v20.16b, v21.16b\n\t"
        "eor v26.16b, v26.16b, v3.16b\n\t"
        "movi v5.16b, #0x5a\n\tmovi v6.16b, #0x69\n\tmovi v7.16b, #0x96\n\t"
        "subs %0, %0, #1\n\tb.ne 1b\n\t"
        "str q27, [%1]\n\tstr q28, [%1, #16]"
        : "+r"(n) : "r"(acc) : "memory", "cc", "v3", "v5", "v6", "v7", "v18", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29");
    /* v27 = 0x11 + 6 * (3 * 5), v28 = 0x22 + 6 * (0x40 * 0x10 mod 256 = 0) */
    unsigned bad = 0;
    for (int i = 0; i < 16; ++i) bad += acc[0][i] != (uint8_t)(0x11 + 6 * 15) || acc[1][i] != 0x22;
    if (bad) { printf("jit_vec_pressure_loop: FAILED\n"); return 1; }
    printf("jit_vec_pressure_loop: 32 lanes passed\n");
    return 0;
}
