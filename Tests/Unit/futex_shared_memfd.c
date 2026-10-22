// A FUTEX_WAIT (non-private) on one mapping of a shared memfd is woken by a
// FUTEX_WAKE on another mapping of the same pages, in one process and across
// fork. AudioTrack's client and server wait and wake this way on their
// control block (audio_track_cblk_t::mFutex).
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long futex(volatile int32_t *addr, int op, int32_t value, const struct timespec *timeout) {
    return syscall(SYS_futex, addr, op, value, timeout, NULL, 0);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

struct waiter {
    volatile int32_t *word;
    int64_t woke_after_ms;
    long result;
    int error;
};

static void *wait_thread(void *arg) {
    struct waiter *w = arg;
    struct timespec limit = {5, 0};
    int64_t start = now_ms();
    errno = 0;
    w->result = futex(w->word, FUTEX_WAIT, 0, &limit);
    w->error = errno;
    w->woke_after_ms = now_ms() - start;
    return NULL;
}

static void nap(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

int main(void) {
    int fd = (int)syscall(SYS_memfd_create, "futex-test", 0);
    if (fd < 0 || ftruncate(fd, 4096) != 0) { printf("FAIL memfd %d\n", errno); return 1; }
    volatile int32_t *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile int32_t *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED || b == MAP_FAILED || a == b) { printf("FAIL mmap\n"); return 1; }

    // Two mappings see the same page.
    *a = 7;
    if (*b != 7) { printf("FAIL mappings do not alias\n"); return 1; }
    *a = 0;

    // 1. Wait on mapping A in a thread, wake through mapping B.
    struct waiter w = {a, -1, -1, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, wait_thread, &w);
    nap(200);
    *b = 1;
    long woken = futex(b, FUTEX_WAKE, INT_MAX, NULL);
    pthread_join(thread, NULL);
    printf("thread: woken=%ld wait_result=%ld errno=%d after=%lldms\n", woken, w.result, w.error,
           (long long)w.woke_after_ms);
    if (!(w.result == 0 && w.woke_after_ms < 2000)) { printf("FAIL same-process wake\n"); return 1; }

    // 2. A child waits on its (inherited) mapping, the parent wakes through B.
    *a = 0;
    pid_t child = fork();
    if (child == 0) {
        struct timespec limit = {5, 0};
        int64_t start = now_ms();
        errno = 0;
        long r = futex(a, FUTEX_WAIT, 0, &limit);
        _exit(r == 0 && now_ms() - start < 2000 ? 0 : 2);
    }
    nap(300);
    *b = 1;
    futex(b, FUTEX_WAKE, INT_MAX, NULL);
    int status = 0;
    waitpid(child, &status, 0);
    printf("fork: child status=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { printf("FAIL cross-process wake\n"); return 1; }

    printf("FUTEX_SHARED_MEMFD_OK\n");
    return 0;
}
