// test_wait_profile.c — five threads that each spend about 0.6 s doing one thing, for BIFROST_PROF's waiting report.
//
// w-sleep sleeps (nanosleep), w-futex waits on a futex, w-poll waits in poll, w-io waits for a pipe to fill and w-spin
// computes. Scripts/Testing/test_wait_profile.sh runs it under BIFROST_PROF and checks that the report charges each
// thread's time to the right kind. The program itself only checks that every thread did its job.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long work_ms = 600;

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (errno %d)\n", what, errno); failures++; } } while (0)

static uint32_t futex_word;
static int pipe_fds[2];
static volatile int done[5];

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void *sleeper(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "w-sleep");
    struct timespec ts = {0, work_ms * 1000000L};
    nanosleep(&ts, NULL);
    done[0] = 1;
    return NULL;
}

static void *futex_waiter(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "w-futex");
    while (__atomic_load_n(&futex_word, __ATOMIC_ACQUIRE) == 0)
        syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, NULL, NULL, 0);
    done[1] = 1;
    return NULL;
}

static void *poller(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "w-poll");
    poll(NULL, 0, (int)work_ms);
    done[2] = 1;
    return NULL;
}

static void *pipe_reader(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "w-io");
    char c;
    CHECK(read(pipe_fds[0], &c, 1) == 1, "pipe read");
    done[3] = 1;
    return NULL;
}

static void *spinner(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "w-spin");
    volatile uint64_t x = 1;
    uint64_t end = now_ms() + (uint64_t)work_ms;
    while (now_ms() < end)
        for (int i = 0; i < 20000; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
    done[4] = 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) work_ms = atol(argv[1]);
    CHECK(pipe(pipe_fds) == 0, "pipe");
    void *(*bodies[5])(void *) = {sleeper, futex_waiter, poller, pipe_reader, spinner};
    pthread_t threads[5];
    for (int i = 0; i < 5; ++i) CHECK(pthread_create(&threads[i], NULL, bodies[i], NULL) == 0, "pthread_create");
    struct timespec ts = {0, work_ms * 1000000L};
    nanosleep(&ts, NULL);
    __atomic_store_n(&futex_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    CHECK(write(pipe_fds[1], "x", 1) == 1, "pipe write");
    for (int i = 0; i < 5; ++i) pthread_join(threads[i], NULL);
    for (int i = 0; i < 5; ++i) CHECK(done[i], "thread finished its job");
    printf(failures ? "test_wait_profile: %d FAILED\n" : "test_wait_profile: ALL PASS\n", failures);
    return failures != 0;
}
