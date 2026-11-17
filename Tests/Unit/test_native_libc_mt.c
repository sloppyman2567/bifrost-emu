// test_native_libc_mt.c — the host-native memcpy/memmove/memset path with several guest threads.
//
// Once a second guest CPU runs, generated code is the multithreaded kind (immutable, chained through data cells) and a
// native call leaves it through the dispatcher. Four workers and the main thread each fill, copy and move their own
// buffers with sizes from 1 KiB to 256 KiB and verify every byte against a byte loop, while all of them also copy from
// one shared read-only source. (jit_native_libc.c has the single-thread checks; verify mode forbids a second CPU, so
// this one is not a jit_ fixture.)
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *(*volatile call_memset)(void *, int, size_t) = memset;
static void *(*volatile call_memcpy)(void *, const void *, size_t) = memcpy;
static void *(*volatile call_memmove)(void *, const void *, size_t) = memmove;

#pragma GCC optimize("no-tree-loop-distribute-patterns")
static int all_equal(const uint8_t *a, uint8_t v, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != v) return 0; return 1; }
static int same(const uint8_t *a, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
#pragma GCC reset_options

enum { WORKERS = 4, ROUNDS = 150, BUF = 300 * 1024, SHARED = 256 * 1024 };
static uint8_t shared_src[SHARED];
static volatile int failures;
static pthread_barrier_t start;

struct Job { int id; uint32_t rng; long checks; };
static uint32_t next(struct Job *j) { j->rng = j->rng * 1664525u + 1013904223u; return j->rng >> 8; }

static void *worker(void *arg) {
    struct Job *j = arg;
    uint8_t *a = malloc(BUF), *b = malloc(BUF);
    pthread_barrier_wait(&start);
    for (int r = 0; r < ROUNDS; r++) {
        size_t n = 1024 + next(j) % (256 * 1024 - 1024);
        int v = (int)(next(j) & 0xff);
        size_t off = next(j) % 64;
        // memset, then copy to the other buffer, then move within it
        if (call_memset(a + off, v, n) != a + off || !all_equal(a + off, (uint8_t)v, n)) failures++;
        if (call_memcpy(b + off, a + off, n) != b + off || !same(b + off, a + off, n)) failures++;
        size_t m = n / 2, shift = 1 + next(j) % 4000;
        if (m + shift < BUF - 64) {
            memcpy(a, shared_src, m + shift);                         // the shared source, read by every thread
            uint8_t first = a[0], last = a[m - 1];
            call_memmove(a + shift, a, m);                           // overlapping forward move
            if (a[shift] != first || a[shift + m - 1] != last || !same(a + shift, shared_src, m)) failures++;
        }
        if (call_memcpy(b, shared_src, 200 * 1024) != b || !same(b, shared_src, 200 * 1024)) failures++;
        j->checks += 4;
    }
    free(a);
    free(b);
    return NULL;
}

int main(void) {
    for (size_t i = 0; i < SHARED; i++) shared_src[i] = (uint8_t)(i * 2654435761u >> 13);
    pthread_t threads[WORKERS];
    struct Job jobs[WORKERS + 1];
    pthread_barrier_init(&start, NULL, WORKERS + 1);
    for (int i = 0; i < WORKERS; i++) {
        jobs[i] = (struct Job){i, 1000u + (uint32_t)i * 77u, 0};
        pthread_create(&threads[i], NULL, worker, &jobs[i]);
    }
    jobs[WORKERS] = (struct Job){WORKERS, 5555, 0};
    worker(&jobs[WORKERS]);  // the main thread is the fifth to reach the barrier inside the worker
    long checks = 0;
    for (int i = 0; i < WORKERS; i++) pthread_join(threads[i], NULL);
    for (int i = 0; i <= WORKERS; i++) checks += jobs[i].checks;
    if (failures) {
        printf("%d of %ld checks failed\n", failures, checks);
        return 1;
    }
    printf("test_native_libc_mt: %ld checks on %d threads: ALL PASS\n", checks, WORKERS + 1);
    return 0;
}
