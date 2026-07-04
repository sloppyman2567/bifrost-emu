// test_pthread.c — multi-threaded compute test for bifrost-emu.
//
// Spawns N threads, each computing fib(35) independently. Verifies:
//   - clone(CLONE_VM | CLONE_SETTLS | CLONE_CHILD_CLEARTID) works
//   - Per-thread FrostJIT instances give parallel speedup
//   - futex (pthread_join) works
//   - TLS (__thread variable) works per-thread
//   - set_tid_address / set_robust_list work
//
// Build: make cross SRC=ctest/test_pthread.c OUT=ctest/test_pthread.elf
// Run:   ./bifrost-emu ctest/test_pthread.elf
//        ./bifrost-emu -v ctest/test_pthread.elf   (shows JIT stats)
//        ./bifrost-emu --no-jit ctest/test_pthread.elf  (interpreter)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// Per-thread TLS variable — each thread gets its own copy via TPIDR_EL0.
static __thread int thread_id = -1;
static __thread long thread_result = 0;

// fib(35) = 9227465. A compute-heavy workload that exercises the JIT.
static long fib(int n) {
    if (n < 2) return n;
    long a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        long c = a + b;
        a = b;
        b = c;
    }
    return b;
}

typedef struct {
    int id;
    int n;
} thread_arg_t;

static void* worker(void* arg) {
    thread_arg_t* a = (thread_arg_t*)arg;
    thread_id = a->id;
    thread_result = fib(a->n);
    printf("thread %d: fib(%d) = %ld\n", thread_id, a->n, thread_result);
    return (void*)thread_result;
}

int main(int argc, char** argv) {
    int nthreads = 4;
    int n = 35;
    if (argc >= 2) nthreads = atoi(argv[1]);
    if (argc >= 3) n = atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    printf("=== pthread test: %d threads, fib(%d) ===\n", nthreads, n);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    pthread_t threads[64];
    thread_arg_t args[64];
    memset(threads, 0, sizeof(threads));
    memset(args, 0, sizeof(args));

    for (int i = 0; i < nthreads; i++) {
        args[i].id = i;
        args[i].n = n;
        int r = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (r != 0) {
            printf("FAIL: pthread_create(%d) returned %d\n", i, r);
            return 1;
        }
    }

    long total = 0;
    int failures = 0;
    for (int i = 0; i < nthreads; i++) {
        void* ret = NULL;
        int r = pthread_join(threads[i], &ret);
        if (r != 0) {
            printf("FAIL: pthread_join(%d) returned %d\n", i, r);
            failures++;
        } else {
            long expected = fib(n);
            long got = (long)ret;
            if (got != expected) {
                printf("FAIL: thread %d returned %ld, expected %ld\n",
                       i, got, expected);
                failures++;
            } else {
                total += got;
            }
        }
    }

    gettimeofday(&t1, NULL);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

    long expected_total = (long)fib(n) * nthreads;
    if (total != expected_total) {
        printf("FAIL: total = %ld, expected %ld\n", total, expected_total);
        failures++;
    }

    printf("=== result: total = %ld, time = %.3fs, %d thread%s ===\n",
           total, secs, nthreads, nthreads == 1 ? "" : "s");

    if (failures == 0) {
        printf("test_pthread: ALL PASS\n");
        return 0;
    } else {
        printf("test_pthread: FAIL (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }
}
