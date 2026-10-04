/* Concurrent hot-region publication, both branch edges, and invalidation of
 * a region's non-head page. Results use an independent closed-form oracle. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned char *code;
static unsigned delta;
static int failed;
static pthread_barrier_t start;
static uint64_t (*loop)(uint64_t);

static void fill_body(void) {
    uint32_t body[] = {
        0x8b020021, /* add x1,x1,x2 */
        0x91000021 | (delta << 10), /* add x1,x1,#delta */
        0x91000442, /* add x2,x2,#1 */
        0x17fffffb, /* b head (-20) */
        0xaa0103e0, /* mov x0,x1 */
        0xd65f03c0  /* ret */
    };
    memcpy(code + 4096, body, sizeof(body));
    __builtin___clear_cache((char *)code + 4096, (char *)code + 4096 + sizeof(body));
}
static void *worker(void *arg) {
    unsigned id = (uintptr_t)arg;
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < 128; ++i) {
        uint64_t n = 1000 + id * 29 + (i % 17);
        uint64_t got = loop(n), want = n * (n - 1) / 2 + delta * n;
        if (got != want) {
            fprintf(stderr, "FAIL delta=%u id=%u got=%llu want=%llu\n", delta, id,
                    (unsigned long long)got, (unsigned long long)want);
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            break;
        }
        if (id == 0 && (i % 16) == 0)
            __builtin___clear_cache((char *)code + 4096, (char *)code + 4120);
        /* Data-only mapping churn must not revoke unrelated hot regions. */
        if (id == 0 && (i % 8) == 0) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED || munmap(p, 4096))
                __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}
int main(void) {
    code = mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 2;
    const uint32_t head[] = {0xd2800001, 0xd2800002, 0xeb00005f, 0x540000a0};
    memcpy(code + 4080, head, sizeof(head));
    loop = (void *)(code + 4080);
    for (unsigned phase = 0; phase < 3; ++phase) {
        delta = 3 + phase * 4;
        if (phase == 2 && mmap(code + 4096, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != code + 4096) return 3;
        fill_body();
        if (phase == 0) __builtin___clear_cache((char *)code, (char *)code + 8192);
        if (pthread_barrier_init(&start, NULL, 8)) return 4;
        pthread_t threads[7];
        for (unsigned i = 0; i < 7; ++i)
            if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1))) return 5;
        worker(NULL);
        for (unsigned i = 0; i < 7; ++i) pthread_join(threads[i], NULL);
        pthread_barrier_destroy(&start);
        if (failed) return 1;
    }
    munmap(code, 8192);
    puts("jit_mt_tier2: ALL PASS");
    return 0;
}
