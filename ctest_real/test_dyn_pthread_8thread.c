// test_dyn_pthread_8thread.c — 8-thread multi-wave TLS isolation test.
//
// This is the regression test for the "8-thread race" bug documented in
// context.md Turn 78 (Issue #3): with 8+ threads across multiple waves,
// glibc's stack-cache reuse caused TLS blocks to overlap, so thread N
// read tls_array[0] = N*25 instead of N*100 (the "got = expected/4"
// pattern indicating 4 threads' TLS blocks overlapped).
//
// The test creates 8 threads × 4 waves × 500 iterations. Each thread:
//   1. Writes its ID into a multi-element __thread array (tls_array[8])
//   2. Increments a shared mutex-protected counter 500 times
//   3. Verifies its tls_array wasn't clobbered by another thread
//
// Success criteria:
//   - counter == 8 * 4 * 500 = 16000 (no lost increments)
//   - All threads verify tls_array integrity (no TLS overlap)
//   - No hangs, crashes, or decode errors
//
// Build (glibc dynamic):
//   tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc \
//     -O2 -o ctest_real/test_dyn_pthread_8thread.elf \
//     ctest_real/test_dyn_pthread_8thread.c -lpthread
//
// Run:
//   BIFROST_ROOT=./rootfs ./bifrost-emu ctest_real/test_dyn_pthread_8thread.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define NTHREADS 8
#define WAVES    4
#define ITERS    500

static atomic_long shared_counter = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Multi-element __thread array in the main binary. This exercises the
// main-exe TLS block at positive TP offsets. The original bug caused
// these to overlap across threads when glibc reused cached stacks.
static __thread long tls_array[8] = {0};

static void* worker(void* arg) {
    long id = (long)arg;

    // Initialize the TLS array with a pattern unique to this thread.
    // tls_array[i] = id * 100 + i  (e.g., thread 3: 300, 301, 302, ...)
    for (int i = 0; i < 8; i++) {
        tls_array[i] = id * 100 + i;
    }

    // Mutex-protected counter increments.
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
    }

    // Verify TLS wasn't clobbered by another thread.
    // The original bug: thread N read tls_array[0] = N*25 instead of N*100,
    // indicating 4 threads' TLS blocks overlapped (100/4 = 25).
    for (int i = 0; i < 8; i++) {
        long expected = id * 100 + i;
        if (tls_array[i] != expected) {
            fprintf(stderr, "thread %ld: TLS corrupted at [%d]: "
                    "got %ld, expected %ld (ratio=%.2f)\n",
                    id, i, tls_array[i], expected,
                    expected ? (double)tls_array[i] / expected : 0.0);
            return (void*)1;
        }
    }

    return NULL;
}

int main(void) {
    int failures = 0;

    printf("=== 8-thread multi-wave test: %d threads x %d waves x %d iters ===\n",
           NTHREADS, WAVES, ITERS);
    printf("=== TLS isolation: __thread long tls_array[8] per thread ===\n");

    for (int w = 0; w < WAVES; w++) {
        pthread_t threads[NTHREADS];
        int wave_failures = 0;

        // Create all threads in this wave.
        for (long i = 0; i < NTHREADS; i++) {
            int rc = pthread_create(&threads[i], NULL, worker, (void*)i);
            if (rc != 0) {
                fprintf(stderr, "wave %d create %ld: %s\n", w, i, strerror(rc));
                failures++;
                wave_failures++;
                threads[i] = 0;
            }
        }

        // Join all threads in this wave.
        for (long i = 0; i < NTHREADS; i++) {
            if (threads[i] == 0) continue;
            void* ret = NULL;
            int rc = pthread_join(threads[i], &ret);
            if (rc != 0) {
                fprintf(stderr, "wave %d join %ld: rc=%d\n", w, i, rc);
                failures++;
                wave_failures++;
            } else if (ret != NULL) {
                fprintf(stderr, "wave %d join %ld: thread reported TLS corruption\n",
                        w, i);
                failures++;
                wave_failures++;
            }
        }

        if (wave_failures == 0) {
            printf("wave %d: OK (8 threads joined, TLS intact)\n", w);
        }
    }

    long expected = (long)NTHREADS * WAVES * ITERS;
    int ok = (atomic_load(&shared_counter) == expected);
    printf("counter = %ld (expected %ld)  %s\n",
           atomic_load(&shared_counter), expected, ok ? "OK" : "FAIL");
    if (!ok) failures++;

    printf("test_dyn_pthread_8thread: %s\n",
           failures == 0 ? "ALL PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
