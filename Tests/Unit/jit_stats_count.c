// jit_stats_count.c — the JIT's guest-instruction count covers chained blocks and self-loop iterations.
//
// BIFROST_PROF (or BIFROST_STATS_PERIOD) makes the translator emit a counter in every block entry and self-loop back
// edge. Scripts/Testing/test_jit_instruction_count.sh runs this under the JIT and under the interpreter and requires
// the two totals to agree; a dispatcher-only counter saw almost none of the work here.
#include <stdio.h>
#include <stdint.h>

static uint64_t mix(uint64_t a, uint64_t b) { return (a * 0x9e3779b97f4a7c15ULL + b) ^ (a >> 11); }

int main(void) {
    uint64_t acc = 1;
    for (int round = 0; round < 200; round++) {
        for (uint64_t i = 0; i < 40000; i++) acc ^= (acc << 7) + i;  // a self-loop block
        acc = mix(acc, (uint64_t)round);                            // one chained call per round
    }
    printf("done: acc=0x%llx\n", (unsigned long long)acc);
    return 0;
}
