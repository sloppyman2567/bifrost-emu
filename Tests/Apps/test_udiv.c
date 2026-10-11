// test_udiv.c — test UDIV and MSDIV (unsigned/signed division)
// used by glibc's __mpn_divrem for float-to-decimal conversion.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

int main() {
    // Test UDIV (unsigned 64-bit division)
    uint64_t a, b, q;
    
    a = 1000; b = 7;
    __asm__ volatile("udiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
    printf("udiv(1000, 7) = %llu (expect 142)\n", (unsigned long long)q);

    a = 0xFFFFFFFFFFFFFFFFULL; b = 2;
    __asm__ volatile("udiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
    printf("udiv(0xFFFFFFFFFFFFFFFF, 2) = %llu (expect 9223372036854775807)\n", (unsigned long long)q);

    a = 0xFFFFFFFFFFFFFFFFULL; b = 0xFFFFFFFFFFFFFFFFULL;
    __asm__ volatile("udiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
    printf("udiv(max, max) = %llu (expect 1)\n", (unsigned long long)q);

    a = 7; b = 1000;
    __asm__ volatile("udiv %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
    printf("udiv(7, 1000) = %llu (expect 0)\n", (unsigned long long)q);

    // Test SDIV (signed 64-bit division)
    int64_t sa = -1000, sb = 7;
    int64_t sq;
    __asm__ volatile("sdiv %0, %1, %2" : "=r"(sq) : "r"(sa), "r"(sb));
    printf("sdiv(-1000, 7) = %lld (expect -142)\n", (long long)sq);

    // Test MSUB (multiply-subtract): result = ra - rn * rm
    uint64_t ra = 1000, rn = 5, rm = 3;
    uint64_t mr;
    __asm__ volatile("msub %0, %1, %2, %3" : "=r"(mr) : "r"(rn), "r"(rm), "r"(ra));
    printf("msub(5, 3, 1000) = %llu (expect 985)\n", (unsigned long long)mr);

    // Test MUL with large values
    a = 0xFFFFFFFF; b = 0xFFFFFFFF;
    __asm__ volatile("mul %0, %1, %2" : "=r"(q) : "r"(a), "r"(b));
    printf("mul(0xFFFFFFFF, 0xFFFFFFFF) = 0x%llx (expect 0xfffffffe00000001)\n", (unsigned long long)q);

    // Test the combination used by __mpn_divrem:
    //   udiv x10, x11, x0    ; q = x11 / x0
    //   msub x11, x10, x0, x11 ; r = x11 - q*x0 = x11 % x0
    a = 1000; b = 7;
    uint64_t rem;
    __asm__ volatile(
        "udiv %0, %2, %3\n"
        "msub %1, %0, %3, %2\n"
        : "=&r"(q), "=r"(rem)
        : "r"(a), "r"(b)
    );
    printf("1000 / 7 = %llu, 1000 %% 7 = %llu (expect 142, 6)\n",
           (unsigned long long)q, (unsigned long long)rem);

    write(1, "[done]\n", 7);
    return 0;
}
