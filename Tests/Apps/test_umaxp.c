// test_umaxp.c — test UMAXP instruction directly via inline asm.
// UMAXP v2.16b, v1.16b, v1.16b: pairwise unsigned max of bytes.
// For input all 0xFF, output should be all 0xFF.
// For input all 0x00, output should be all 0x00.
// For input [0xFF,0x00,0xFF,0x00,...], output should be all 0xFF (max of each pair).
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void print_vec(const char *name, const uint8_t *v) {
    printf("%s: ", name);
    for (int i = 0; i < 16; i++) printf("%02x ", v[i]);
    printf("\n");
}

int main() {
    uint8_t in[16] __attribute__((aligned(16)));
    uint8_t out[16] __attribute__((aligned(16)));
    
    // Test 1: all 0xFF (like strlen finding NUL bytes)
    memset(in, 0xFF, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q0, [%[in]]\n"
        "cmeq v1.16b, v0.16b, #0\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v0", "v1", "v2", "memory"
    );
    print_vec("umaxp(0xFF*16)", out);
    // Expected: all 0xFF (max of pairs of 0xFF,0xFF)
    
    // Test 2: all 0x00 (no NUL found by strlen)
    memset(in, 0x00, 16);
    memset(out, 0, 16);
    __asm__ volatile(
        "ldr q0, [%[in]]\n"
        "cmeq v1.16b, v0.16b, #0\n"
        "umaxp v2.16b, v1.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        : [out] "+r"(out)
        : [in] "r"(in)
        : "v0", "v1", "v2", "memory"
    );
    print_vec("umaxp(0x00*16)", out);
    // Expected: all 0x00 (cmeq on 0x00 produces 0xFF? wait no - cmeq produces 0xFF when byte IS zero)
    // Actually: cmeq v1, v0, #0 → v1[i] = 0xFF if v0[i]==0, else 0x00
    // For v0 = all 0x00, v1 = all 0xFF
    // Then umaxp(v1, v1) = all 0xFF
    // So output should be all 0xFF!
    
    // Test 3: cmeq only
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
    // Expected: all 0x00 (no byte is zero)
    
    // Test 4: strlen of a short string  
    const char *s = "hello";
    printf("strlen(\"hello\") = %zu (expect 5)\n", strlen(s));
    
    // Test 5: strlen of empty string
    const char *e = "";
    printf("strlen(\"\") = %zu (expect 0)\n", strlen(e));
    
    return 0;
}
