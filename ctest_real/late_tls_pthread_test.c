// glibc regression for late dynamic TLS on existing and recycled pthreads.
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NTHREADS 8
#define WAVES 4
#define TLS_INITIAL 0x13579bdf

typedef int *(*tls_getter_t)(void);
static tls_getter_t tls_getter;
static _Atomic int start_flag;

struct worker_arg {
    unsigned id;
    unsigned wave;
};

static void *worker(void *opaque) {
    const struct worker_arg *arg = opaque;
    while (!atomic_load_explicit(&start_flag, memory_order_acquire))
        __asm__ volatile("yield");

    int *value = tls_getter();
    if (value == NULL || *value != TLS_INITIAL)
        return (void *)1;

    const int expected = (int)(0x40000000u | (arg->wave << 8) | arg->id);
    *value = expected;
    for (int i = 0; i < 100; ++i) {
        if (tls_getter() != value || *value != expected)
            return (void *)1;
    }
    return NULL;
}

static int run_wave(unsigned wave) {
    pthread_t threads[NTHREADS];
    struct worker_arg args[NTHREADS];
    atomic_store_explicit(&start_flag, 0, memory_order_release);

    for (unsigned i = 0; i < NTHREADS; ++i) {
        args[i] = (struct worker_arg){i, wave};
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0)
            return 1;
    }

    atomic_store_explicit(&start_flag, 1, memory_order_release);
    int failed = 0;
    for (unsigned i = 0; i < NTHREADS; ++i) {
        void *result = NULL;
        if (pthread_join(threads[i], &result) != 0 || result != NULL)
            failed = 1;
    }
    return failed;
}

int main(void) {
    pthread_t threads[NTHREADS];
    struct worker_arg args[NTHREADS];
    atomic_store(&start_flag, 0);

    // Start these threads before dlopen so they have no DTV entry for the
    // module when it is loaded.
    for (unsigned i = 0; i < NTHREADS; ++i) {
        args[i] = (struct worker_arg){i, 0};
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0)
            return 1;
    }

    void *handle = dlopen("/tmp/bifrost_late_tls_pthread/liblate_tls_pthread.so",
                          RTLD_NOW);
    if (handle == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    tls_getter = (tls_getter_t)dlsym(handle, "tls_value_addr");
    if (tls_getter == NULL) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 3;
    }

    atomic_store_explicit(&start_flag, 1, memory_order_release);
    int failed = 0;
    for (unsigned i = 0; i < NTHREADS; ++i) {
        void *result = NULL;
        if (pthread_join(threads[i], &result) != 0 || result != NULL)
            failed = 1;
    }

    // Reuse cached stacks repeatedly. The DTV must remain readable until
    // glibc has recycled it, then its emulator-owned TLS can be reclaimed.
    for (unsigned wave = 1; wave < WAVES; ++wave)
        failed |= run_wave(wave);

    int *main_value = tls_getter();
    if (main_value == NULL || *main_value != TLS_INITIAL)
        failed = 1;

    puts(failed ? "late TLS pthread waves: FAIL"
                : "late TLS pthread waves: PASS");
    return failed;
}
