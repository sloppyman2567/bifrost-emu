// Exclusive pairs (LDXP/LDAXP/STXP/STLXP): element order, status results,
// XZR operands (o1 separates pairs from LDXR/STXR), W pairs and a 128-bit
// counter incremented by competing threads.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); ++failures; }
}

static uint64_t pair[2] __attribute__((aligned(16)));
static uint32_t wpair[2] __attribute__((aligned(8)));

enum { THREADS = 4, ROUNDS = 20000 };
static uint64_t counter[2] __attribute__((aligned(16)));

static void *increment(void *unused) {
    (void)unused;
    for (int i = 0; i < ROUNDS; ++i) {
        uint64_t lo, hi;
        uint32_t status;
        do {
            __asm__ __volatile__("ldaxp %0, %1, [%3]\n"
                                 "add %0, %0, #1\n"
                                 "add %1, %1, #2\n"
                                 "stlxp %w2, %0, %1, [%3]\n"
                                 : "=&r"(lo), "=&r"(hi), "=&r"(status)
                                 : "r"(counter) : "memory");
        } while (status);
    }
    return 0;
}

int main(void) {
    uint64_t a, b;
    uint32_t status, wa, wb;

    pair[0] = 0x1111111122222222ULL;
    pair[1] = 0x3333333344444444ULL;
    __asm__ __volatile__("ldxp %0, %1, [%2]" : "=&r"(a), "=&r"(b) : "r"(pair) : "memory");
    check(a == pair[0] && b == pair[1], "LDXP loads Rt from the lower address");
    __asm__ __volatile__("stxp %w0, %1, %2, [%3]"
                         : "=&r"(status) : "r"(0xAULL), "r"(0xBULL), "r"(pair) : "memory");
    check(status == 0 && pair[0] == 0xA && pair[1] == 0xB, "STXP stores a reserved pair");

    __asm__ __volatile__("stxp %w0, %1, %2, [%3]"
                         : "=&r"(status) : "r"(1ULL), "r"(2ULL), "r"(pair) : "memory");
    check(status == 1 && pair[0] == 0xA && pair[1] == 0xB, "STXP without a reservation fails");

    __asm__ __volatile__("ldaxp %0, %1, [%2]" : "=&r"(a), "=&r"(b) : "r"(pair) : "memory");
    __asm__ __volatile__("stlxp %w0, %1, %2, [%3]"
                         : "=&r"(status) : "r"(b), "r"(a), "r"(pair) : "memory");
    check(status == 0 && pair[0] == 0xB && pair[1] == 0xA, "LDAXP/STLXP swap a pair");

    // Rt2 = XZR: still a pair. LDXR/STXR would leave pair[1] untouched.
    pair[0] = 7; pair[1] = 9;
    __asm__ __volatile__("ldxp %0, xzr, [%1]" : "=&r"(a) : "r"(pair) : "memory");
    __asm__ __volatile__("stxp %w0, %1, xzr, [%2]"
                         : "=&r"(status) : "r"(a + 1), "r"(pair) : "memory");
    check(a == 7 && status == 0 && pair[0] == 8 && pair[1] == 0, "XZR second register of a pair");

    wpair[0] = 0x01020304; wpair[1] = 0x05060708;
    __asm__ __volatile__("ldxp %w0, %w1, [%2]" : "=&r"(wa), "=&r"(wb) : "r"(wpair) : "memory");
    check(wa == 0x01020304 && wb == 0x05060708, "LDXP W pair");
    __asm__ __volatile__("stxp %w0, %w1, %w2, [%3]"
                         : "=&r"(status) : "r"(wb), "r"(wa), "r"(wpair) : "memory");
    check(status == 0 && wpair[0] == 0x05060708 && wpair[1] == 0x01020304, "STXP W pair");

    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) pthread_create(&threads[i], 0, increment, 0);
    for (int i = 0; i < THREADS; ++i) pthread_join(threads[i], 0);
    check(counter[0] == (uint64_t)THREADS * ROUNDS && counter[1] == 2ULL * THREADS * ROUNDS,
          "128-bit LDAXP/STLXP counter under contention");

    if (failures) return 1;
    puts("test_exclusive_pair: ALL PASS");
    return 0;
}
