// test_futex_requeue.c — FUTEX_CMP_REQUEUE argument semantics.
//
// futex(uaddr, FUTEX_CMP_REQUEUE, nr_wake, nr_requeue, uaddr2, val3)
// passes nr_requeue in the timeout slot; val3 is only compared with
// *uaddr. Bifrost used val3 as the requeue count, so ART's condition
// variable broadcast (nr_wake 0, nr_requeue INT32_MAX, val3 = sequence
// 0) moved no waiter and VM startup hung.
//
// Runs under both JIT and --no-jit; "checks passed" is the pass marker.
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int checks = 0, failures = 0;

static void check(int cond, const char* what) {
    checks++;
    printf("%s: %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

static long futex(atomic_int* uaddr, int op, int val, uintptr_t val2,
                  atomic_int* uaddr2, int val3) {
    return syscall(SYS_futex, uaddr, op, val, val2, uaddr2, val3);
}

static atomic_int cond_seq = 0;   // the condition variable's futex word
static atomic_int mutex_word = 0; // the futex its waiters are moved to
static atomic_int waiting = 0, woke = 0;

static void* waiter(void* arg) {
    (void)arg;
    struct timespec limit = {3, 0};
    atomic_store(&waiting, 1);
    // Like ART's ConditionVariable::Wait, any return from the wait ends
    // it (the caller then re-takes the mutex); the 3 s limit only stops a
    // lost wake-up from hanging the test. Bifrost wakes requeued waiters
    // early instead of moving them (see AGENTS.md), which this pattern
    // tolerates; a waiter that re-waits on the old word would not.
    long r = futex(&cond_seq, FUTEX_WAIT_PRIVATE, 0, (uintptr_t)&limit, NULL, 0);
    if (r == 0) atomic_store(&woke, 1);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    errno = 0;
    check(futex(&cond_seq, FUTEX_CMP_REQUEUE_PRIVATE, 0, 1, &mutex_word, 1) == -1 &&
          errno == EAGAIN, "val3 mismatch -> EAGAIN");
    errno = 0;
    check(futex(&cond_seq, FUTEX_CMP_REQUEUE_PRIVATE, -1, 1, &mutex_word, 0) == -1 &&
          errno == EINVAL, "negative nr_wake -> EINVAL");

    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
    while (!atomic_load(&waiting)) sched_yield();
    struct timespec settle = {0, 50 * 1000 * 1000};
    nanosleep(&settle, NULL);  // let the waiter block on cond_seq

    // Broadcast with sequence 0 as ART's first broadcast does: val3 = 0
    // matches *uaddr, wake nobody, move everyone (nr_requeue INT_MAX).
    // The old code read val3 as the count and moved nobody.
    long moved = futex(&cond_seq, FUTEX_CMP_REQUEUE_PRIVATE, 0, INT_MAX, &mutex_word, 0);
    check(moved == 1, "CMP_REQUEUE with sequence 0 moves the waiter");
    atomic_store(&cond_seq, 1);
    futex(&mutex_word, FUTEX_WAKE_PRIVATE, 1, 0, NULL, 0);
    pthread_join(t, NULL);
    check(atomic_load(&woke) == 1, "requeued waiter wakes");

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
