// bench_fib.c — recursive Fibonacci benchmark.
// Tests function call overhead and recursion depth.
#include <stdio.h>
#include <time.h>

static long fib(long n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long result = fib(35);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("fib(35) = %ld in %.3fs\n", result, secs);
    return 0;
}
