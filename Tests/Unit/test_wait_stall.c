// test_wait_stall.c — three threads for BIFROST_PROF's `[stall]` lines (a call of a second or more on a busy thread).
//
// st-futex computes for 0.3 s, then waits on a futex for about 1.1 s. st-sleep computes, then sleeps for 1.2 s. st-idle only
// waits on a futex for 1.4 s: a thread that does not run is not a freeze, so it must not be reported. st-pipe and st-sock compute,
// then block in read() on a pipe and on a unix socket until the main thread writes after 1.4 s; their descriptor numbers are
// printed. st-loop computes briefly and sleeps 1.05 s, nine times from one call site: only five stalls print, and the report
// counts all nine. Thirteen st-many threads each sleep seven times from their own site, so 14 sites pass five and the report
// shows twelve and then "and 2 more call sites". (A thread whose record is made late loses its first stall, hence seven.) The main thread wakes the futex waiters after 1.4 s. The futex addresses are printed so
// Scripts/Testing/test_wait_stall.sh can match them.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

static uint32_t busy_word, idle_word;
#define MANY 13
static volatile int done[6 + MANY];
static int pipe_fds[2], sock_fds[2];

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void spin(uint64_t ms) {
    volatile uint64_t x = 1;
    uint64_t end = now_ms() + ms;
    while (now_ms() < end)
        for (int i = 0; i < 20000; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
}

static void wait_on(uint32_t *word) {
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) == 0) syscall(SYS_futex, word, FUTEX_WAIT, 0, NULL, NULL, 0);
}

static void *futex_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-futex");
    spin(300);
    wait_on(&busy_word);
    done[0] = 1;
    return NULL;
}

static void *sleep_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-sleep");
    spin(300);
    struct timespec ts = {1, 200000000L};
    nanosleep(&ts, NULL);
    done[1] = 1;
    return NULL;
}

static void *idle_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-idle");
    wait_on(&idle_word);
    done[2] = 1;
    return NULL;
}

static void *pipe_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-pipe");
    spin(300);
    char c;
    CHECK(read(pipe_fds[0], &c, 1) == 1, "pipe read");
    done[3] = 1;
    return NULL;
}

static void *sock_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-sock");
    spin(300);
    char c;
    CHECK(read(sock_fds[0], &c, 1) == 1, "socket read");
    done[4] = 1;
    return NULL;
}

static void *many_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-many");
    for (int i = 0; i < 7; ++i) {
        spin(100);
        struct timespec ts = {1, 50000000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *loop_thread(void *unused) {
    (void)unused;
    prctl(PR_SET_NAME, "st-loop");
    for (int i = 0; i < 9; ++i) {
        spin(150);
        struct timespec ts = {1, 50000000L};
        nanosleep(&ts, NULL);
    }
    done[5] = 1;
    return NULL;
}

int main(void) {
    CHECK(pipe(pipe_fds) == 0, "pipe");
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sock_fds) == 0, "socketpair");
    printf("futex words: busy=%p idle=%p\n", (void *)&busy_word, (void *)&idle_word);
    printf("descriptors: pipe=%d socket=%d\n", pipe_fds[0], sock_fds[0]);
    fflush(stdout);
    pthread_t threads[6 + MANY];
    CHECK(pthread_create(&threads[0], NULL, futex_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[1], NULL, sleep_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[2], NULL, idle_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[3], NULL, pipe_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[4], NULL, sock_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[5], NULL, loop_thread, NULL) == 0, "pthread_create");
    for (int i = 0; i < MANY; ++i) CHECK(pthread_create(&threads[6 + i], NULL, many_thread, NULL) == 0, "pthread_create");
    struct timespec ts = {1, 400000000L};
    nanosleep(&ts, NULL);
    __atomic_store_n(&busy_word, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&idle_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &busy_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    syscall(SYS_futex, &idle_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    CHECK(write(pipe_fds[1], "x", 1) == 1, "pipe write");
    CHECK(write(sock_fds[1], "x", 1) == 1, "socket write");
    for (int i = 0; i < 6 + MANY; ++i) pthread_join(threads[i], NULL);
    for (int i = 0; i < 6; ++i) CHECK(done[i], "thread finished");
    printf(failures ? "test_wait_stall: %d FAILED\n" : "test_wait_stall: ALL PASS\n", failures);
    return failures != 0;
}
