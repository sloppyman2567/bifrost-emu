// test_atomic_stress.c — high-contention atomic operations stress test.
//
// Validates that LSE CAS atomics and LL/SC (via mutex) are correct under
// high contention (8 threads). Games use these patterns heavily:
//   - CAS loops for lock-free refcounting, job queues, state machines
//   - Mutex-protected shared state for render/audio threads
//
// This test catches:
//   - Lost updates (CAS or LL/SC race conditions)
//   - Deadlocks (futex wake failures)
//   - Global exclusive monitor correctness (sharded monitor)
//
// Build: make cross SRC=ctest/test_atomic_stress.c OUT=ctest/test_atomic_stress.elf
// Run:   ./bifrost-emu ctest/test_atomic_stress.elf
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NTHREADS 8

// ── Test 1: CAS atomic counter (no mutex) ──
#define CAS_ITERS 3000
static volatile int64_t cas_counter = 0;

static void* cas_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < CAS_ITERS; i++) {
        int64_t old, new;
        do {
            old = cas_counter;
            new = old + 1;
        } while (!__sync_bool_compare_and_swap(&cas_counter, old, new));
    }
    return NULL;
}

// ── Test 2: Mutex-protected counter (LL/SC path) ──
#define MUTEX_ITERS 3000
static long mutex_counter = 0;
static pthread_mutex_t mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static void* mutex_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < MUTEX_ITERS; i++) {
        pthread_mutex_lock(&mutex_lock);
        mutex_counter++;
        pthread_mutex_unlock(&mutex_lock);
    }
    return NULL;
}

// ── Test 3: __atomic_add (LDADD instruction) ──
#define ADD_ITERS 3000
static volatile int64_t add_counter = 0;

static void* add_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < ADD_ITERS; i++) {
        __atomic_add_fetch(&add_counter, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void) {
    printf("=== atomic stress test: %d threads ===\n", NTHREADS);
    int all_ok = 1;

    // Test 1: CAS
    {
        cas_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, cas_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        int64_t exp = (int64_t)NTHREADS * CAS_ITERS;
        int ok = (cas_counter == exp);
        printf("CAS:         %lld (exp %lld)  %s\n",
               (long long)cas_counter, (long long)exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    // Test 2: Mutex
    {
        mutex_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, mutex_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        long exp = (long)NTHREADS * MUTEX_ITERS;
        int ok = (mutex_counter == exp);
        printf("Mutex (LL/SC): %ld (exp %ld)  %s\n",
               mutex_counter, exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    // Test 3: __atomic_add (LDADD)
    {
        add_counter = 0;
        pthread_t t[NTHREADS];
        for (int i = 0; i < NTHREADS; i++)
            pthread_create(&t[i], NULL, add_worker, NULL);
        for (int i = 0; i < NTHREADS; i++)
            pthread_join(t[i], NULL);
        int64_t exp = (int64_t)NTHREADS * ADD_ITERS;
        int ok = (add_counter == exp);
        printf("atomic_add:  %lld (exp %lld)  %s\n",
               (long long)add_counter, (long long)exp, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    }

    printf("test_atomic_stress: %s\n", all_ok ? "ALL PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
