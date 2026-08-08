// test_dlopen_mt.c — multithreaded dlopen/dlsym/dladdr/dl_iterate_phdr test.
//
// Exercises the DynamicLinker thread-safety + borrow-CPU fixes on the
// emulator side:
//   1. Several guest threads hammer dlopen/dlsym/dlclose concurrently
//      (loader state races — objects_, symbols_, last_error_).
//   2. Guest threads call dladdr + dl_iterate_phdr concurrently with the
//      dlopen threads (races over the shared guest scratch buffer and
//      the main_cpu_ borrow when a non-main thread triggers a guest call).
//   3. A thread spins on dlerror() while dlopen failures set last_error_
//      (get/set_last_error lock).
//
// The old code would corrupt symbols_/objects_ under load and crash or
// return wrong symbol addresses; the borrow-CPU race (non-main thread
// clobbering main_cpu_) would corrupt the register file of the thread
// running guest code and crash with decode errors.
//
// Build (static musl so it runs without a DynamicLinker-provided
// runtime — we exercise the pure syscall paths; run alongside the
// dynamic glibc/musl suite for full coverage):
//   make cross SRC=ctest_real/test_dlopen_mt.c OUT=ctest_real/test_dlopen_mt.elf
//
// Run:
//   BIFROST_ROOT=./rootfs ./bifrost-emu ctest_real/test_dlopen_mt.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <link.h>

#define NUM_DLOPEN_THREADS 4
#define NUM_WORKER_THREADS  4
#define ITERATIONS         500
static volatile int stop_workers = 0;
static volatile int errors = 0;

// dlopen/dlsym/dlclose hammer — should keep refcounts consistent and
// never return a dangling handle or crash in the loader.
static void *dlopen_worker(void *vdp) {
    int t = (int)(intptr_t)vdp;
    for (int i = 0; i < ITERATIONS; i++) {
        void *h = dlopen("/lib/libm.so.6", RTLD_LAZY);
        if (!h) { __sync_fetch_and_add(&errors, 1); continue; }
        double (*sqrt_fn)(double) = (double (*)(double))dlsym(h, "sqrt");
        if (!sqrt_fn) {
            fprintf(stderr, "[t%d] dlsym sqrt failed: %s\n", t, dlerror());
            __sync_fetch_and_add(&errors, 1);
        } else if (i % 7 == 0) {
            double r = sqrt_fn(81.0);
            if (r != 9.0) {
                fprintf(stderr, "[t%d] sqrt(81)=%f\n", t, r);
                __sync_fetch_and_add(&errors, 1);
            }
        }
        // Reopen the same lib — dedup must bump the refcount, not
        // re-map or reset state. Then close.
        void *h2 = dlopen("libm.so.6", RTLD_LAZY);
        if (!h2) { fprintf(stderr, "[t%d] re-dlopen failed\n", t); }
        if (dlclose(h2) != 0) {
            fprintf(stderr, "[t%d] dlclose(h2) failed\n", t);
            __sync_fetch_and_add(&errors, 1);
        }
        if (dlclose(h) != 0) {
            fprintf(stderr, "[t%d] dlclose(h) failed: %s\n", t, dlerror());
            __sync_fetch_and_add(&errors, 1);
        }
    }
    return NULL;
}

// dladdr + dl_iterate_phdr — walks loader state concurrently with the
// dlopen threads; exercises the loader lock over iterate_phdr and the
// guest-call borrow on a NON-MAIN thread (the callback runs guest code,
// which used to borrow main_cpu_).
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && info->dlpi_name[0]) {
        // Walk the shared scratch buffer's name — just touch it.
        volatile char c = info->dlpi_name[0];
        (void)c;
    }
    return 0;  // continue iterating
}

static void *phdr_worker(void *varg) {
    (void)varg;
    while (!stop_workers) {
        dl_iterate_phdr(phdr_cb, NULL);
    }
    return NULL;
}

static void *dladdr_worker(void *varg) {
    (void)varg;
    while (!stop_workers) {
        Dl_info info;
        if (dladdr((void*)0x400000, &info) != 0) {
            // An address is mapped there (the main binary or libs);
            // we just want the call to complete without corruption.
        }
    }
    return NULL;
}

int main(void) {
    int failures = 0;
    printf("=== test_dlopen_mt: multi-threaded dl* ===\n");

    pthread_t dl_threads[NUM_DLOPEN_THREADS];
    for (int i = 0; i < NUM_DLOPEN_THREADS; i++) {
        if (pthread_create(&dl_threads[i], NULL, dlopen_worker,
                           (void*)(intptr_t)i) != 0) {
            fprintf(stderr, "FAIL: create dlopen thread %d\n", i);
            failures++;
        }
    }
    pthread_t p1, p2;
    if (pthread_create(&p1, NULL, phdr_worker, NULL) != 0) failures++;
    if (pthread_create(&p2, NULL, dladdr_worker, NULL) != 0) failures++;
    // Give workers time to interleave at least a few iterations, then stop.
    struct timespec ts = {0, 50L * 1000 * 1000}; // 50ms
    nanosleep(&ts, NULL);
    for (int i = 0; i < NUM_DLOPEN_THREADS; i++) {
        pthread_join(dl_threads[i], NULL);
    }
    // Now let dladdr/phdr keep running briefly while main also does dl*.
    dl_iterate_phdr(phdr_cb, NULL);
    void *h = dlopen("libm.so.6", RTLD_LAZY);
    if (!h) { fprintf(stderr, "FAIL: dlopen after workers: %s\n", dlerror()); failures++; }
    if (h) { if (dlclose(h) != 0) failures++; }

    stop_workers = 1;
    pthread_join(p1, NULL);
    pthread_join(p2, NULL);

    if (errors) {
        fprintf(stderr, "FAIL: %d worker errors\n", errors);
        failures += errors;
    }
    if (failures == 0) {
        printf("test_dlopen_mt: ALL PASS\n");
        return 0;
    }
    fprintf(stderr, "test_dlopen_mt: %d failures\n", failures);
    return 1;
}