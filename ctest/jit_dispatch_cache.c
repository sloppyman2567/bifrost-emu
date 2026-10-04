/* Repeated indirect calls to conflicting code addresses, then synchronized
 * code replacement. Results must survive shared-JIT cache hits/eviction. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define NFUN 8
#define STRIDE 1024
static uint8_t *code;
static unsigned loops = 4096;
static int failed;
static unsigned increments[NFUN];
static int churn;

static void *run(void *arg) {
    uint64_t base = (uintptr_t)arg, sum = 0, increment_sum = 0;
    for (unsigned i = 0; i < NFUN; ++i) increment_sum += increments[i];
    for (unsigned r = 0; r < loops; ++r) {
        for (unsigned i = 0; i < NFUN; ++i) {
            uint64_t (*fn)(uint64_t) = (void *)(code + i * STRIDE);
            sum += fn(base + r);
        }
        if (churn) {
            uint64_t *data = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (data == MAP_FAILED) { __atomic_store_n(&failed, 1, __ATOMIC_RELAXED); return NULL; }
            *data = sum;
            if (*data != sum || munmap(data, 4096))
                __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
        }
    }
    uint64_t expected = NFUN * (uint64_t)loops * base +
                        NFUN * (uint64_t)loops * (loops - 1) / 2 +
                        increment_sum * loops;
    if (sum != expected) {
        fprintf(stderr, "FAIL base=%llu got=%llu expected=%llu\n",
                (unsigned long long)base, (unsigned long long)sum,
                (unsigned long long)expected);
        __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void set_function(unsigned i, unsigned increment) {
    uint32_t *p = (void *)(code + i * STRIDE);
    p[0] = 0x91000000u | (increment << 10); /* add x0,x0,#increment */
    p[1] = 0xd65f03c0u; /* ret */
    increments[i] = increment;
    __builtin___clear_cache((char *)p, (char *)(p + 2));
}

int main(int argc, char **argv) {
    if (argc > 1) {
        unsigned long n = strtoul(argv[1], NULL, 10);
        if (!n || n > 10000000) return 2;
        loops = n;
    }
    code = mmap(NULL, NFUN * STRIDE, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 2;
    for (unsigned i = 0; i < NFUN; ++i) set_function(i, i + 1);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i)
        if (pthread_create(&threads[i], NULL, run, (void *)(uintptr_t)(10000 * (i + 1)))) return 3;
    run(NULL);
    for (unsigned i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
    set_function(7, 17);
    loops = 512;
    churn = 1;
    run(NULL);
    munmap(code, NFUN * STRIDE);
    if (failed) return 1;
    puts("jit_dispatch_cache: ALL PASS");
    return 0;
}
