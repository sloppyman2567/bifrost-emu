// test_lsl_var.c — test LSL (variable shift) instruction
#include <stdio.h>

// Force the compiler to emit LSL (variable) by using inline asm
static unsigned int lsl_var(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile ("lsl %w0, %w1, %w2" : "=r"(result) : "r"(a), "r"(b));
    return result;
}

int main(void) {
    // Test cases: lsl(a, b) = a << (b & 31)
    struct { unsigned a, b, expected; } tests[] = {
        {1, 0, 1},
        {1, 1, 2},
        {1, 10, 1024},
        {1, 31, 0x80000000},
        {1, 32, 1},      // 32 & 31 = 0, so lsl by 0 = 1
        {0, 10, 0},
        {0xFF, 8, 0xFF00},
        {3, 5, 96},
    };
    int pass = 0, fail = 0;
    for (int i = 0; i < 8; i++) {
        unsigned result = lsl_var(tests[i].a, tests[i].b);
        if (result == tests[i].expected) {
            pass++;
        } else {
            fail++;
            printf("FAIL: lsl(%u, %u) = %u, expected %u\n",
                   tests[i].a, tests[i].b, result, tests[i].expected);
        }
    }
    printf("lsl_var: %d pass, %d fail\n", pass, fail);
    if (fail == 0) printf("ALL PASS\n");
    return fail > 0 ? 1 : 0;
}
