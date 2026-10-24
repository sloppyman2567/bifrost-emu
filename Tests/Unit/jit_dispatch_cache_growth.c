/* Thread-local dispatch caches that grow while threads dispatch, and are
 * cleared by invalidations while grown. Four threads call 2048 distinct
 * functions through a table (far more targets than a starting cache holds);
 * another thread maps, runs and unmaps a code page, which retires a block
 * and changes the dispatch generation. Results use a closed-form oracle. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* f_0abcd is function number 0abcd (an octal literal): its table index. */
#define F(p) static __attribute__((noinline)) uint64_t f_##p(uint64_t x) { return x * (2 * (uint64_t)(p) + 1) + (p); }
#define F8(p) F(p##0) F(p##1) F(p##2) F(p##3) F(p##4) F(p##5) F(p##6) F(p##7)
#define F64(p) F8(p##0) F8(p##1) F8(p##2) F8(p##3) F8(p##4) F8(p##5) F8(p##6) F8(p##7)
#define F512(p) F64(p##0) F64(p##1) F64(p##2) F64(p##3) F64(p##4) F64(p##5) F64(p##6) F64(p##7)
F512(00) F512(01) F512(02) F512(03)
#define T(p) f_##p,
#define T8(p) T(p##0) T(p##1) T(p##2) T(p##3) T(p##4) T(p##5) T(p##6) T(p##7)
#define T64(p) T8(p##0) T8(p##1) T8(p##2) T8(p##3) T8(p##4) T8(p##5) T8(p##6) T8(p##7)
#define T512(p) T64(p##0) T64(p##1) T64(p##2) T64(p##3) T64(p##4) T64(p##5) T64(p##6) T64(p##7)
static uint64_t (*const table[])(uint64_t) = {T512(00) T512(01) T512(02) T512(03)};
enum { FUNCTIONS = sizeof(table) / sizeof(table[0]), ROUNDS = 48, CALLERS = 4 };

static int failed;
static volatile int callers_done;

static void *caller(void *arg) {
    const unsigned id = (unsigned)(uintptr_t)arg;
    uint64_t got = id, want = id;
    for (unsigned round = 0; round < ROUNDS; ++round)
        for (unsigned i = 0; i < FUNCTIONS; ++i) {
            const unsigned k = (i * 7 + round * 13 + id * 101) % FUNCTIONS;
            got = table[k](got);
            want = want * (2 * (uint64_t)k + 1) + k;
        }
    if (got != want) {
        fprintf(stderr, "FAIL caller %u: got=%llx want=%llx\n", id, (unsigned long long)got,
                (unsigned long long)want);
        __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* Maps `mov x0, #n; ret`, runs it and unmaps it: every unmap retires the block. */
static void *churn(void *arg) {
    (void)arg;
    unsigned runs = 0;
    while (!__atomic_load_n(&callers_done, __ATOMIC_ACQUIRE) || runs < 64) {
        uint32_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) { perror("mmap"); __atomic_store_n(&failed, 1, __ATOMIC_RELAXED); break; }
        const unsigned n = runs & 0xffff;
        page[0] = 0xd2800000 | (n << 5); /* mov x0, #n */
        page[1] = 0xd65f03c0;            /* ret */
        __builtin___clear_cache((char *)page, (char *)(page + 2));
        const uint64_t got = ((uint64_t (*)(void))page)();
        munmap(page, 4096);
        if (got != n) {
            fprintf(stderr, "FAIL churn run %u: got=%llu\n", runs, (unsigned long long)got);
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            break;
        }
        ++runs;
    }
    return NULL;
}

int main(void) {
    pthread_t callers[CALLERS], churner;
    pthread_create(&churner, NULL, churn, NULL);
    for (unsigned i = 0; i < CALLERS; ++i) pthread_create(&callers[i], NULL, caller, (void *)(uintptr_t)i);
    for (unsigned i = 0; i < CALLERS; ++i) pthread_join(callers[i], NULL);
    __atomic_store_n(&callers_done, 1, __ATOMIC_RELEASE);
    pthread_join(churner, NULL);
    if (failed) return 1;
    printf("jit_dispatch_cache_growth: ALL PASS\n");
    return 0;
}
