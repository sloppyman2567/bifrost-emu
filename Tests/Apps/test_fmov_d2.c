// test_fmov_d2.c — test fmov x, dN properly.
// The previous test had wrong inline asm constraints.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

int main() {
    // Test 1: Load double into d0, then fmov to GPR
    double d = 3.14;
    uint64_t bits;
    // Use "w" constraint for FP register, and explicitly use d0
    register double d0 __asm__("d0") = d;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d0));
    printf("3.14 bits = 0x%016llx (expect 0x40091EB851EB851F)\n", (unsigned long long)bits);

    // Test 2: 1.0
    d = 1.0;
    d0 = d;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d0));
    printf("1.0 bits = 0x%016llx (expect 0x3FF0000000000000)\n", (unsigned long long)bits);

    // Test 3: 0.5
    d = 0.5;
    d0 = d;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d0));
    printf("0.5 bits = 0x%016llx (expect 0x3FE0000000000000)\n", (unsigned long long)bits);

    // Test 4: large double
    d = 1234.5678;
    d0 = d;
    __asm__ volatile("fmov %0, d0" : "=r"(bits) : "w"(d0));
    printf("1234.5678 bits = 0x%016llx (expect 0x40934A437B1A89C8 approx)\n", (unsigned long long)bits);

    // Test 5: fmov d0, x (reverse direction)
    uint64_t val = 0x40091EB851EB851FULL;
    register double result __asm__("d0");
    __asm__ volatile("fmov d0, %1" : "=w"(result) : "r"(val));
    printf("0x40091EB851EB851F -> double = %.6f (expect 3.140000)\n", result);

    write(1, "[done]\n", 7);
    return 0;
}
