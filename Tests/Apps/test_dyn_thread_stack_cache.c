// test_dyn_thread_stack_cache.c — glibc keeps joined threads' stacks in a 40 MiB cache and
// frees the oldest ones through _dl_deallocate_tls once it is full. Twelve overlapping
// 8 MiB-stack threads fill the cache whatever RLIMIT_STACK is (sequential threads would just
// reuse one cached stack), so the native linker must route that call to its own TLS teardown
// rather than ld-linux's (which calls a NULL __rtld_free).
#include <pthread.h>
#include <stdio.h>

#define THREADS 12

static pthread_barrier_t all_running;
static __thread long tls_value;

static void* worker(void* arg) {
    tls_value = (long)arg * 3;
    pthread_barrier_wait(&all_running);
    return (void*)tls_value;
}

int main(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_barrier_init(&all_running, NULL, THREADS);
    pthread_t threads[THREADS];
    for (long i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], &attr, worker, (void*)(i + 1)) != 0) {
            printf("pthread_create %ld failed\n", i);
            return 1;
        }
    }
    for (long i = 0; i < THREADS; i++) {
        void* ret = NULL;
        if (pthread_join(threads[i], &ret) != 0 || (long)ret != (i + 1) * 3) {
            printf("thread %ld returned the wrong value\n", i);
            return 1;
        }
    }
    puts("test_dyn_thread_stack_cache: ALL PASS");
    return 0;
}
