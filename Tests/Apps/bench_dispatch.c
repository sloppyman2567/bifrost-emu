// bench_dispatch.c — dispatch-path stress benchmark for bifrost-emu
// Heavy BL (direct call) + BLR (indirect/function-pointer call) traffic
// forces the caller block to exit to the dispatcher on every iteration,
// so this measures the block-dispatch + call-helper path (as opposed to
// bench_mips, which is a single self-loop block that never dispatches).
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) static uint64_t leaf_a(uint64_t x) {
    return x ^ (x << 3) ^ 0x9e3779b97f4a7c15ULL;
}
__attribute__((noinline)) static uint64_t leaf_b(uint64_t x) {
    return (x * 0x2545f4914f6cdd1dULL) >> 17;
}
__attribute__((noinline)) static uint64_t leaf_c(uint64_t x) {
    return x + (x >> 5) + 0x517cc1b727220a95ULL;
}
__attribute__((noinline)) static uint64_t leaf_d(uint64_t x) {
    return x ^ (x * 7) ^ (x << 13);
}

volatile uint64_t sink;

int main() {
    uint64_t acc = 0x123456789abcdef0ULL;
    typedef uint64_t (*fp)(uint64_t);
    fp fns[4] = {leaf_a, leaf_b, leaf_c, leaf_d};
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        acc = fns[i & 3](acc);   // BLR — indirect call, unchainable
        acc = leaf_a(acc);       // BL  — direct call
        acc = leaf_b(acc);       // BL
        acc ^= acc << 11;        // ALU so the leaves aren't tail-dead
        acc ^= acc >> 7;
    }
    sink = acc;
    printf("done: acc=0x%llx\n", (unsigned long long)sink);
    return 0;
}
