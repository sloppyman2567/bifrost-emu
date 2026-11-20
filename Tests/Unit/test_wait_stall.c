// test_wait_stall.c — three threads for BIFROST_PROF's `[stall]` lines (a call of a second or more on a busy thread).
//
// st-futex computes for 0.3 s, then waits on a futex for about 1.1 s. st-sleep computes, then sleeps for 1.2 s. st-idle only
// waits on a futex for 1.4 s: a thread that does not run is not a freeze, so it must not be reported. The main thread wakes the
// futex waiters after 1.4 s. The futex addresses are printed so Scripts/Testing/test_wait_stall.sh can match them.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

static uint32_t busy_word, idle_word;
static volatile int done[3];

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

int main(void) {
    printf("futex words: busy=%p idle=%p\n", (void *)&busy_word, (void *)&idle_word);
    fflush(stdout);
    pthread_t threads[3];
    CHECK(pthread_create(&threads[0], NULL, futex_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[1], NULL, sleep_thread, NULL) == 0, "pthread_create");
    CHECK(pthread_create(&threads[2], NULL, idle_thread, NULL) == 0, "pthread_create");
    struct timespec ts = {1, 400000000L};
    nanosleep(&ts, NULL);
    __atomic_store_n(&busy_word, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&idle_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &busy_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    syscall(SYS_futex, &idle_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    for (int i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
    for (int i = 0; i < 3; ++i) CHECK(done[i], "thread finished");
    printf(failures ? "test_wait_stall: %d FAILED\n" : "test_wait_stall: ALL PASS\n", failures);
    return failures != 0;
}
