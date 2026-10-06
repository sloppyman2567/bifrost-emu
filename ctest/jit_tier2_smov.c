// Loop-carried pinning regression: a direct GPR writer must not leave a
// stale loop-carried value pinned in a host register.  The loop reads x3,
// then SMOV writes x3 without a STORE_REG; the next iteration must read
// that newly written value.
//
// Two entries: falling into the loop runs one iteration in the preceding
// block, so x3 already equals the SMOV result when the self-loop block
// starts (a lost write is invisible). Branching straight to the loop
// makes the self-loop block see x3 = 1 first, which exposes a lost SMOV
// write (Tier-1 self-loop pinning lacked SIMD_SMOV: got 4000).
#include <stdint.h>
#include <stdio.h>

static uint64_t fall_in(const int8_t* lanes) {
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
    return sum;
}

static uint64_t branch_in(const int8_t* lanes) {
    register uint64_t count asm("x0") = 4000;
    register uint64_t sum asm("x1") = 0;
    register uint64_t carried asm("x3") = 1;
    register const int8_t *src asm("x2") = lanes;
    __asm__ volatile(
        "ldr q0, [x2]\n"
        "b 1f\n"
        "1:\n"
        "add x1, x1, x3\n"
        "smov w3, v0.b[0]\n"
        "subs x0, x0, #1\n"
        "b.ne 1b\n"
        : "+r"(count), "+r"(sum), "+r"(carried)
        : "r"(src)
        : "v0", "cc", "memory");
    return sum;
}

int main(void) {
    static const int8_t lanes[16] = {2};
    const uint64_t want = 1 + 2 * (4000 - 1);
    uint64_t got[2] = {fall_in(lanes), branch_in(lanes)};
    const char* name[2] = {"fall-in", "branch-in"};
    int failed = 0;
    for (int i = 0; i < 2; ++i) {
        if (got[i] != want) {
            printf("FAIL: tier2 smov %s loop got=%llu want=%llu\n", name[i],
                   (unsigned long long)got[i], (unsigned long long)want);
            failed = 1;
        }
    }
    if (failed) return 1;
    puts("ALL PASS: tier2 smov loop-carried register");
    return 0;
}
