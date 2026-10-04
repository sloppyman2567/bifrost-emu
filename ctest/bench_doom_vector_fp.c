/* Reproduce the indexed-FMUL/vector-FNEG mix observed in Doom's fallback
 * profile. Inputs stay exact and unchanged over five million iterations;
 * this measures math throughput, not gameplay FPS. Build with make cross.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    float input[4] = {2, 4, 8, 16}, scalar[4] = {1, 1, 1, 1}, output[4];
    uint64_t iterations = 5000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    __asm__ volatile(
        "ldr q26,[%1]\n ldr q31,[%2]\n"
        "1:\n"
        "fmul v26.4s,v26.4s,v31.s[0]\n"
        "fneg v26.4s,v26.4s\n"
        "fmul v26.4s,v26.4s,v31.s[2]\n"
        "fneg v26.4s,v26.4s\n"
        "subs %0,%0,#1\n b.ne 1b\n"
        "str q26,[%3]"
        : "+r"(iterations)
        : "r"(input), "r"(scalar), "r"(output)
        : "v26", "v31", "cc", "memory");
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - start.tv_sec +
                     (end.tv_nsec - start.tv_nsec) * 1e-9;
    printf("seconds=%.6f output=%.0f,%.0f,%.0f,%.0f\n", elapsed,
           output[0], output[1], output[2], output[3]);
    return !(output[0] == 2 && output[1] == 4 &&
             output[2] == 8 && output[3] == 16);
}
