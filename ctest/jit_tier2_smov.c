// Tier-2 pinning regression: a direct GPR writer must not leave a stale
// loop-carried value pinned in a host register.  The loop reads x3, then
// SMOV writes x3 without a STORE_REG; the next iteration must read that
// newly written value.
#include <stdint.h>
#include <stdio.h>

int main(void) {
    static const int8_t lanes[16] = {2};
    register uint64_t count asm("x0") = 4000;
    register uint64_t sum asm("x1") = 0;
    register uint64_t carried asm("x3") = 1;
    register const int8_t *src asm("x2") = lanes;

    __asm__ volatile(
        "ldr q0, [x2]\n"
        "1:\n"
        "add x1, x1, x3\n"
        "smov w3, v0.b[0]\n"
        "subs x0, x0, #1\n"
        "b.ne 1b\n"
        : "+r"(count), "+r"(sum), "+r"(carried)
        : "r"(src)
        : "v0", "cc", "memory");

    const uint64_t want = 1 + 2 * (4000 - 1);
    if (sum != want) {
        printf("FAIL: tier2 smov loop got=%llu want=%llu\n",
               (unsigned long long)sum, (unsigned long long)want);
        return 1;
    }
    puts("ALL PASS: tier2 smov loop-carried register");
    return 0;
}
