// bench_mips.c — MIPS benchmark for bifrost-emu
// Measures instructions/second for compute-intensive loops.
#include <stdio.h>
#include <stdint.h>

volatile uint64_t sink;

int main() {
    // 100M iterations of simple ALU work (4 instructions per iteration)
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 100000000ULL; i++) {
        acc += i;
        acc ^= (acc << 3);
        acc += (i * 7);
        acc ^= (acc >> 5);
    }
    sink = acc;
    printf("done: acc=0x%llx\n", (unsigned long long)sink);
    return 0;
}
