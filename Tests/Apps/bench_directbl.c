// minimal direct BL test
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) static uint64_t leaf_a(uint64_t x) {
    return x ^ (x << 3) ^ 0x9e3779b97f4a7c15ULL;
}
__attribute__((noinline)) static uint64_t leaf_b(uint64_t x) {
    return (x * 0x2545f4914f6cdd1dULL) >> 17;
}

volatile uint64_t sink;

int main() {
    uint64_t acc = 0x123456789abcdef0ULL;
    for (uint64_t i = 0; i < 1ULL; i++) {
        acc = leaf_a(acc);
        acc = leaf_b(acc);
        acc ^= acc << 11;
    }
    sink = acc;
    printf("done: acc=0x%llx\n", (unsigned long long)sink);
    return 0;
}