/* Two threads map, run and unmap executable file pages in a loop. The first thread's page returns 1, the
 * second thread's returns 2, and neither runs cache maintenance: the kernel keeps a freshly mapped file
 * coherent, so the emulator must drop the translation of a page that an unmap just released before a
 * mapping at the same address can run. Each thread checks its own result. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

enum { ROUNDS = 20000, THREADS = 2 };

static int fd;
static int failed;

static void *worker(void *arg) {
    const unsigned id = (unsigned)(uintptr_t)arg; /* 0 or 1: the page of the file it maps */
    for (unsigned round = 0; round < ROUNDS; ++round) {
        void *page = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, (off_t)id * 4096);
        if (page == MAP_FAILED) { perror("mmap"); __atomic_store_n(&failed, 1, __ATOMIC_RELAXED); break; }
        const uint64_t got = ((uint64_t (*)(void))page)();
        munmap(page, 4096);
        if (got != id + 1) {
            fprintf(stderr, "FAIL thread %u round %u: got=%llu want=%u\n", id, round, (unsigned long long)got, id + 1);
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    return NULL;
}

int main(void) {
    char path[] = "/tmp/jit_unmap_reuse_XXXXXX";
    fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    unlink(path);
    uint32_t code[2 * 1024] = {0};
    for (unsigned id = 0; id < THREADS; ++id) {
        code[id * 1024] = 0xd2800000 | ((id + 1) << 5); /* mov x0, #id + 1 */
        code[id * 1024 + 1] = 0xd65f03c0;               /* ret */
    }
    if (write(fd, code, sizeof code) != (ssize_t)sizeof code) { perror("write"); return 1; }
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i);
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    if (failed) return 1;
    printf("jit_unmap_reuse_race: ALL PASS\n");
    return 0;
}
