// fcvtzu_test.c — test the FCVTZU (fixed-point to unsigned) instruction.
// MD5 uses: fcvtzu w1, d0, #32 → w1 = (uint32_t)(d0 * 2^32)
#include <stdio.h>
#include <stdint.h>

int main(void) {
    double d = 0.841470984807897;  // sin(1.0)
    uint32_t got;

    // fcvtzu w1, d0, #32  — convert d0 to uint32 with 32 fractional bits
    __asm__ volatile ("fcvtzu %w0, %d1, #32" : "=r"(got) : "w"(d));
    uint32_t exp = (uint32_t)(d * 4294967296.0);
    printf("fcvtzu(%.15f, #32) = 0x%08x  exp=0x%08x  %s\n",
           d, got, exp, got == exp ? "OK" : "FAIL");

    // Test with a few more values
    double vals[] = {0.5, 0.25, 0.75, 0.1, 0.99, 0.841470984807897, 0.909297426825682};
    for (int i = 0; i < 7; i++) {
        __asm__ volatile ("fcvtzu %w0, %d1, #32" : "=r"(got) : "w"(vals[i]));
        exp = (uint32_t)(vals[i] * 4294967296.0);
        printf("fcvtzu(%.6f, #32) = 0x%08x  exp=0x%08x  %s\n",
               vals[i], got, exp, got == exp ? "OK" : "FAIL");
    }

    // Test 64-bit form
    uint64_t got64;
    __asm__ volatile ("fcvtzu %0, %d1, #32" : "=r"(got64) : "w"(d));
    uint64_t exp64 = (uint64_t)(d * 4294967296.0);
    printf("fcvtzu64(%.15f, #32) = 0x%016lx  exp=0x%016lx  %s\n",
           d, got64, exp64, got64 == exp64 ? "OK" : "FAIL");

    // Test the standard FCVTZU (no fixed-point, just integer conversion)
    uint32_t got_s;
    __asm__ volatile ("fcvtzu %w0, %d1" : "=r"(got_s) : "w"(123.456));
    printf("fcvtzu(123.456) = %u (exp=123)  %s\n", got_s, got_s == 123 ? "OK" : "FAIL");

    return 0;
}
