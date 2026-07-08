// test_umaxp2.c — isolated tests for cmeq and umaxp.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void print_vec(const char *name, const uint8_t *v) {
    printf("%-30s: ", name);
    for (int i = 0; i < 16; i++) printf("%02x ", v[i]);
    printf("\n");
}

int main() {
    uint8_t in[16] __attribute__((aligned(16))) = {0};
    uint8_t out[16] __attribute__((aligned(16))) = {0};
    
    // Test 1: cmeq on all-zero input → should produce all 0xFF
    memset(in, 0x00, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q0, [%[in]]\n"
        "cmeq v1.16b, v0.16b, #0\n"
        "str q1, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v0", "v1", "memory"
    );
    print_vec("cmeq(0x00*16, #0)", out);
    // Expected: all 0xFF
    
    // Test 2: cmeq on all-0xAA input → should produce all 0x00
    memset(in, 0xAA, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q0, [%[in]]\n"
        "cmeq v1.16b, v0.16b, #0\n"
        "str q1, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v0", "v1", "memory"
    );
    print_vec("cmeq(0xAA*16, #0)", out);
    // Expected: all 0x00
    
    // Test 3: umaxp v2, v1, v1 with v1 = all 0xFF (no cmeq)
    memset(in, 0xFF, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v1", "v2", "memory"
    );
    print_vec("umaxp(0xFF*16)", out);
    // Expected: all 0xFF
    
    // Test 4: umaxp v2, v1, v1 with v1 = all 0x00
    memset(in, 0x00, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v1", "v2", "memory"
    );
    print_vec("umaxp(0x00*16)", out);
    // Expected: all 0x00
    
    // Test 5: umaxp v2, v1, v1 with v1 = mix of 0xFF and 0x00
    for (int i = 0; i < 16; i++) in[i] = (i % 2 == 0) ? 0xFF : 0x00;
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v1", "v2", "memory"
    );
    print_vec("umaxp(FF,00,FF,00,...)", out);
    // Expected: pairs (FF,00)→FF, so all 0xFF
    
    // Test 6: uminp
    memset(in, 0xFF, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "uminp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v1", "v2", "memory"
    );
    print_vec("uminp(0xFF*16)", out);
    // Expected: all 0xFF
    
    // Test 7: fmov x0, d2 (read bottom 8 bytes of v2)
    memset(in, 0xFF, 16);
    uint64_t x = 0;
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "fmov %x[x], d2\n"
        : [x] "=r"(x)
        : [in] "r"(in)
        : "v1", "v2", "memory"
    );
    printf("fmov x, d2 after umaxp(0xFF*16): 0x%016llx (expect 0xFFFFFFFFFFFFFFFF)\n", (unsigned long long)x);
    
    // Test 8: cmeq then umaxp then fmov (full strlen pipeline)
    memset(in, 0x00, 16);
    x = 0;
    __asm__ volatile(
        "ldr q0, [%[in]]\n"
        "cmeq v1.16b, v0.16b, #0\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "fmov %x[x], d2\n"
        : [x] "=r"(x)
        : [in] "r"(in)
        : "v0", "v1", "v2", "memory"
    );
    printf("strlen pipeline (in=0x00): fmov x, d2 = 0x%016llx (expect 0xFFFFFFFFFFFFFFFF)\n", (unsigned long long)x);
    
    return 0;
}
