// test_pthread_once.c — pthread_once (one-time initialization) test for bifrost-emu.
//
// Tests:
//   - pthread_once ensures init_routine runs exactly once, even when
//     called concurrently by multiple threads.
//   - The once_control prevents re-initialization on subsequent calls.
//   - Threads block until the init_routine completes.
//
// NOTE: Uses 4 threads (conservative) to stay within the emulator's
// current futex-wakeup throughput.
//
// Build: make cross SRC=ctest/test_pthread_once.c OUT=ctest/test_pthread_once.elf
// Run:   ./bifrost-emu ctest/test_pthread_once.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTHREADS 4

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static int init_count = 0;
static int initialized_value = 0;

static void init_routine(void) {
    usleep(10000);
    __sync_fetch_and_add(&init_count, 1);
    initialized_value = 42;
}

typedef struct {
    int id;
    int saw_value;
} targ_t;

static void* worker(void* arg) {
    targ_t* a = (targ_t*)arg;
    pthread_once(&once_control, init_routine);
    a->saw_value = initialized_value;
    return NULL;
}

int main(void) {
    printf("=== pthread_once test: %d threads ===\n", NTHREADS);

    pthread_t threads[NTHREADS];
    targ_t args[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        args[i].id = i;
        args[i].saw_value = 0;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("init_count = %d (expected 1)\n", init_count);
    int all_saw_value = 1;
    for (int i = 0; i < NTHREADS; i++) {
        if (args[i].saw_value != 42) {
            all_saw_value = 0;
            printf("thread %d saw value %d (expected 42)\n", i, args[i].saw_value);
        }
    }
    printf("all threads saw initialized value: %s\n", all_saw_value ? "YES" : "NO");

    // Call pthread_once again — should be a no-op.
    pthread_once(&once_control, init_routine);
    printf("after extra call: init_count = %d (expected 1)\n", init_count);

    int ok = (init_count == 1 && all_saw_value);
    printf("test_pthread_once: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
