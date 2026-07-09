// test_dyn_threads.c — glibc dynamically-linked pthread test.
//
// Spawns N threads, each incrementing a shared counter under a mutex,
// then joins them and verifies the total. This exercises the dynamic
// linker's _dl_allocate_tls path (glibc's pthread_create calls it to
// allocate a per-thread TLS block). Without a working _dl_allocate_tls,
// glibc's allocatestack.c hits an assertion and crashes.
//
// Build (glibc dynamic):
//   tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc \
//     -O2 -o ctest_real/test_dyn_threads.elf ctest_real/test_dyn_threads.c \
//     -lpthread
//
// Run:
//   ./bifrost-emu --rootfs ./rootfs ctest_real/test_dyn_threads.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS 4
#define ITERS    1000

static long shared_counter = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static __thread int thread_local_var = 0;  // exercises per-thread TLS

typedef struct {
    int id;
} targ_t;

static void* worker(void* arg) {
    targ_t* a = (targ_t*)arg;
    thread_local_var = a->id;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&lock);
        shared_counter++;
        pthread_mutex_unlock(&lock);
    }
    if (thread_local_var != a->id) {
        fprintf(stderr, "thread %d: TLS corrupted (got %d)\n",
                a->id, thread_local_var);
        return (void*)1;
    }
    return NULL;
}

int main(void) {
    printf("=== dyn pthread test: %d threads x %d iters ===\n",
           NTHREADS, ITERS);

    pthread_t threads[NTHREADS];
    targ_t args[NTHREADS];
    int failures = 0;

    for (int i = 0; i < NTHREADS; i++) {
        args[i].id = i;
        int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create %d: %s\n", i, strerror(rc));
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        void* ret = NULL;
        int rc = pthread_join(threads[i], &ret);
        if (rc != 0) {
            fprintf(stderr, "pthread_join %d: %s\n", i, strerror(rc));
            failures++;
        } else if (ret != NULL) {
            failures++;
        }
    }

    long expected = (long)NTHREADS * ITERS;
    int ok = (shared_counter == expected);
    printf("counter = %ld (expected %ld)  %s\n",
           shared_counter, expected, ok ? "OK" : "FAIL");
    if (!ok) failures++;

    printf("test_dyn_threads: %s\n",
           failures == 0 ? "ALL PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
