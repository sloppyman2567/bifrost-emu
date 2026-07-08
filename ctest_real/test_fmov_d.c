// test_fmov_d.c — test fmov x, d0 (move double from FP to GPR)
// This is used by glibc's __mpn_extract_double to extract the mantissa.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

int main() {
    // Test 1: fmov x, d0 with 3.14
    double d = 3.14;
    uint64_t bits;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d));
    printf("3.14 bits = 0x%016llx (expect 0x40091EB851EB851F)\n", (unsigned long long)bits);

    // Test 2: fmov d0, x (move GPR to FP)
    uint64_t val = 0x40091EB851EB851FULL;
    double result;
    __asm__ volatile("fmov d0, %1\n" : "=w"(result) : "r"(val));
    printf("0x40091EB851EB851F -> double = %f (expect 3.140000)\n", result);

    // Test 3: extract mantissa manually (like __mpn_extract_double)
    d = 3.14;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d));
    uint64_t sign = bits >> 63;
    uint64_t exp = (bits >> 52) & 0x7FF;
    uint64_t mant = bits & 0xFFFFFFFFFFFFFULL;
    printf("3.14: sign=%llu exp=%llu mant=0x%llx\n",
           (unsigned long long)sign,
           (unsigned long long)exp,
           (unsigned long long)mant);
    // Expected: sign=0, exp=1024 (0x400), mant=0x1EB851EB851F

    // Test 4: 1.0
    d = 1.0;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d));
    printf("1.0 bits = 0x%016llx (expect 0x3FF0000000000000)\n", (unsigned long long)bits);

    // Test 5: 0.1
    d = 0.1;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d));
    printf("0.1 bits = 0x%016llx (expect 0x3FB999999999999A)\n", (unsigned long long)bits);

    // Test 6: Verify we can round-trip
    d = 3.14;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d));
    __asm__ volatile("fmov d0, %1\n" : "=w"(result) : "r"(bits));
    printf("round-trip: 3.14 -> 0x%llx -> %f\n", (unsigned long long)bits, result);

    write(1, "[done]\n", 7);
    return 0;
}
