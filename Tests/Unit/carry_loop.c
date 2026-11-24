// A hot loop of four blocks with a data-dependent branch and no calls, for the Carry loop report
// (BIFROST_PROF with BIFROST_PC_HIST, Scripts/Testing/test_carry_report.sh). Hand-written so the compiler cannot turn the
// branch into a select: the loop is a cycle of several blocks over five guest registers.
#include <stdint.h>
#include <stdio.h>

int main(void) {
    register uint64_t i asm("x1") = 0;
    register uint64_t sum asm("x2") = 0;
    register uint64_t up asm("x3") = 3;
    register uint64_t down asm("x4") = 1;
    register uint64_t limit asm("x5") = 400000000ull;
    __asm__ volatile(
        "1:\n"
        "tbz x1, #0, 2f\n"
        "add x2, x2, x3\n"
        "b 3f\n"
        "2:\n"
        "sub x2, x2, x4\n"
        "3:\n"
        "add x1, x1, #1\n"
        "cmp x1, x5\n"
        "b.ne 1b\n"
        : "+r"(i), "+r"(sum)
        : "r"(up), "r"(down), "r"(limit)
        : "cc");
    // Half the iterations add 3 and half subtract 1.
    uint64_t want = (limit / 2) * 3 - (limit / 2) * 1;
    if (sum != want) {
        printf("FAIL: sum=%llu want=%llu\n", (unsigned long long)sum, (unsigned long long)want);
        return 1;
    }
    puts("carry_loop: ALL PASS");
    return 0;
}
