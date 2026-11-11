// Tier-2 regions through BL and RET (run with BIFROST_TIER2=1 by the suite).
//
// Under chain-skip a BL is a jump that sets X30. The trace walker follows it
// into the callee and turns the matching RET into a guard on X30, continuing
// at the return address; a RET that ends the path leaves through its register.
// Each loop below runs long enough to be promoted and checks its results
// against closed forms:
//   guard    - the callee adds 0 or 4 to X30 before returning, so every 1024th
//              return skips an instruction at the call site: the guard must
//              fail and leave through X30, not continue at the traced address.
//   nested   - a call inside a called function (two return addresses).
//   twice    - the same callee twice per iteration (its second call ends the
//              trace with a static exit into the callee).
//   linear   - a branchy leaf whose own trace ends at its RET.
#include <stdint.h>
#include <stdio.h>

// x0 += 3; return to x30 + x1 (x1 is 0 or 4).
__asm__(".text\n"
        ".global t2_skip_ret\n"
        ".type t2_skip_ret, %function\n"
        "t2_skip_ret:\n"
        "    add x0, x0, #3\n"
        "    add x30, x30, x1\n"
        "    ret\n"
        ".size t2_skip_ret, .-t2_skip_ret\n");

static int failures;

static void check(const char* name, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %s: got %llu want %llu\n", name, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void guard_loop(uint64_t n) {
    register uint64_t left asm("x19") = n;
    register uint64_t counted asm("x20") = 0;  // skipped when the return moves
    register uint64_t summed asm("x21") = 0;
    register uint64_t k asm("x22") = 0;
    __asm__ volatile(
        "1:\n"
        "    tst x22, #0x3ff\n"
        "    cset x1, eq\n"
        "    lsl x1, x1, #2\n"        // 4 on every 1024th iteration, else 0
        "    mov x0, x22\n"
        "    bl t2_skip_ret\n"
        "    add x20, x20, #1\n"      // the instruction a moved return skips
        "    add x21, x21, x0\n"
        "    add x22, x22, #1\n"
        "    subs x19, x19, #1\n"
        "    b.ne 1b\n"
        : "+r"(left), "+r"(counted), "+r"(summed), "+r"(k)
        :
        : "x0", "x1", "x30", "cc", "memory");
    const uint64_t moved = (n + 1023) / 1024;
    check("guard counted", counted, n - moved);
    check("guard summed", summed, n * (n - 1) / 2 + 3 * n);
}

__attribute__((noinline)) static uint64_t inner(uint64_t x) {
    __asm__ volatile("" : "+r"(x));
    return x * 3 + 1;
}

__attribute__((noinline)) static uint64_t outer(uint64_t x) {
    __asm__ volatile("" : "+r"(x));
    return inner(x) + 2;
}

static void nested_loop(uint64_t n) {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum += outer(i);
        __asm__ volatile("" : "+r"(sum));
    }
    check("nested", sum, 3 * (n * (n - 1) / 2) + 3 * n);
}

static void twice_loop(uint64_t n) {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum += inner(i) + inner(i + 1);
        __asm__ volatile("" : "+r"(sum));
    }
    check("twice", sum, 3 * (n * (n - 1) / 2) + 3 * (n * (n + 1) / 2) + 2 * n);
}

__attribute__((noinline)) static uint64_t classify(uint64_t x) {
    __asm__ volatile("" : "+r"(x));
    if (x % 3 == 0) return x * 5;
    if (x % 3 == 1) return x + 7;
    return x ^ 0x55;
}

static void linear_loop(uint64_t n) {
    uint64_t sum = 0, want = 0;
    for (uint64_t i = 0; i < n; ++i) {
        sum += classify(i);
        __asm__ volatile("" : "+r"(sum));
    }
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t r = i % 3;
        want += r == 0 ? i * 5 : r == 1 ? i + 7 : (i ^ 0x55);
    }
    check("linear", sum, want);
}

int main(void) {
    for (int round = 0; round < 3; ++round) {
        guard_loop(400000);
        nested_loop(300000);
        twice_loop(300000);
        linear_loop(300000);
    }
    if (failures) return 1;
    puts("jit_tier2_calls: ALL PASS");
    return 0;
}
