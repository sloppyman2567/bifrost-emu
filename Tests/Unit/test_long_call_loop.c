// test_long_call_loop.c — a valid loop that calls one function 60M times
// must run to completion on the main thread and on a worker thread.
//
// Under the JIT every iteration returned to the dispatcher at the same
// block, so the same-PC "hang watchdog" (50M) killed the program with exit
// code 70 after well under a second of legitimate work. That diagnostic is
// now opt-in (BIFROST_HANG_WATCHDOG=1). JIT-only in run_tests.sh: the
// interpreter never had the problem and needs ~12 s for this loop.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define ITERATIONS 60000000ULL

__attribute__((noinline)) static uint64_t step(uint64_t x) { return x * 3 + 1; }

static uint64_t call_loop(uint64_t seed) {
    uint64_t acc = seed;
    for (uint64_t i = 0; i < ITERATIONS; i++) {
        acc = step(acc);
        __asm__ volatile("" : "+r"(acc));
    }
    return acc;
}

static uint64_t inline_loop(uint64_t seed) {
    uint64_t acc = seed;
    for (uint64_t i = 0; i < ITERATIONS; i++) {
        acc = acc * 3 + 1;
        __asm__ volatile("" : "+r"(acc));
    }
    return acc;
}

static void* worker(void* arg) {
    *(uint64_t*)arg = call_loop(7);
    return NULL;
}

int main(void) {
    uint64_t threaded = 0;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, &threaded) != 0) {
        printf("FAIL: pthread_create\n");
        return 1;
    }
    uint64_t main_result = call_loop(1);
    pthread_join(thread, NULL);
    int ok = main_result == inline_loop(1) && threaded == inline_loop(7);
    printf("test_long_call_loop: %s\n", ok ? "ALL PASS" : "FAIL (wrong result)");
    return !ok;
}
