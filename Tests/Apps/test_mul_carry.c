// test_mul_carry.c — test MUL/MADD/ADCS instruction combinations used by
// glibc's __mpn_mul_1. The float printf precision bug may be caused by
// incorrect carry propagation in multi-precision multiply.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

// Replicate the __mpn_mul_1 inner loop in C with inline asm to test
// each instruction individually.
int main() {
    // Test 1: basic 64-bit multiply
    uint64_t a = 0xFFFFFFFF;  // 2^32 - 1
    uint64_t b = 0xFFFFFFFF;
    uint64_t result;
    __asm__ volatile("mul %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    printf("mul(0xFFFFFFFF, 0xFFFFFFFF) = 0x%016lx (expect 0xfffffffe00000001)\n", result);

    // Test 2: MADD (multiply-add)
    uint64_t c = 0x12345678;
    __asm__ volatile("madd %0, %1, %2, %3" : "=r"(result) : "r"(a), "r"(b), "r"(c));
    printf("madd(0xFFFFFFFF, 0xFFFFFFFF, 0x12345678) = 0x%016lx (expect 0xfffffffe012345679)\n", result);

    // Test 3: UMULH (high 64 bits of 64×64 multiply)
    __asm__ volatile("umulh %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
    printf("umulh(0xFFFFFFFF, 0xFFFFFFFF) = 0x%016lx (expect 0x00000000fffffffe)\n", result);

    // Test 4: ADCS (add with carry and set flags)
    uint64_t x = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t y = 0x1;
    uint64_t z;
    uint64_t flags;
    __asm__ volatile(
        "adds xzr, %1, #0\n"      // set carry=0
        "adcs %0, %2, %3\n"        // x + y + carry, set flags
        "mrs %4, nzcv\n"           // get flags
        : "=r"(z), "+r"(x), "+r"(y), "=r"(flags)
        : "r"(0)
    );
    // Hmm, this won't work as expected. Let me do it differently.
    
    // Test 5: 128-bit multiply via 32-bit decomposition (like __mpn_mul_1)
    // Multiply 0x0000000100000000 (2^32) by 10
    uint64_t val = 0x0000000100000000ULL;
    uint64_t mult = 10;
    uint64_t lo32 = val & 0xFFFFFFFF;
    uint64_t hi32 = val >> 32;
    uint64_t mlo32 = mult & 0xFFFFFFFF;
    uint64_t mhi32 = mult >> 32;
    
    uint64_t p0, p1, p2, p3;
    __asm__ volatile("mul %0, %1, %2" : "=r"(p0) : "r"(lo32), "r"(mlo32));
    __asm__ volatile("mul %0, %1, %2" : "=r"(p1) : "r"(hi32), "r"(mlo32));
    __asm__ volatile("mul %0, %1, %2" : "=r"(p2) : "r"(lo32), "r"(mhi32));
    __asm__ volatile("mul %0, %1, %2" : "=r"(p3) : "r"(hi32), "r"(mhi32));
    
    printf("32-bit decomp: p0=0x%lx p1=0x%lx p2=0x%lx p3=0x%lx\n", p0, p1, p2, p3);
    printf("  expected: p0=0 p1=0xa p2=0 p3=0\n");

    // Test 6: __int128 multiply (ground truth)
    unsigned __int128 full = (unsigned __int128)val * (unsigned __int128)mult;
    printf("128-bit: val*10 = 0x%016lx%016lx (expect 0x000000000000000a0000000000000000)\n",
           (uint64_t)(full >> 64), (uint64_t)full);

    write(1, "[done]\n", 7);
    return 0;
}
