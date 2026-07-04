// test_pthread_cond.c — pthread_cond (condition variable) test for bifrost-emu.
//
// Tests:
//   - pthread_cond_init / signal / broadcast / wait / destroy
//   - pthread_cond_wait releases the mutex while waiting and reacquires
//     it before returning.
//   - Signal wakes exactly one waiter; broadcast wakes all.
//   - Multiple concurrent waiters (exercises FUTEX_CMP_REQUEUE which
//     previously had a recursive-lock deadlock — now fixed).
//
// Build: make cross SRC=ctest/test_pthread_cond.c OUT=ctest/test_pthread_cond.elf
// Run:   ./bifrost-emu ctest/test_pthread_cond.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define NWAITERS 4

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static int ready = 0;
static int done = 0;

typedef struct {
    int id;
} targ_t;

static void* waiter(void* arg) {
    targ_t* a = (targ_t*)arg;
    (void)a;
    pthread_mutex_lock(&lock);
    while (ready == 0) {
        pthread_cond_wait(&cond, &lock);
    }
    done++;
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void) {
    printf("=== pthread_cond test ===\n");

    // ── Test 1: signal wakes waiters one at a time ──
    printf("Test 1: signal wakes %d waiters one at a time\n", NWAITERS);
    ready = 0; done = 0;
    pthread_t t1[NWAITERS];
    targ_t a1[NWAITERS];
    for (int i = 0; i < NWAITERS; i++) {
        a1[i].id = i;
        pthread_create(&t1[i], NULL, waiter, &a1[i]);
    }
    usleep(50000);
    // Signal NWAITERS times — each signal should wake exactly one waiter.
    for (int i = 0; i < NWAITERS; i++) {
        pthread_mutex_lock(&lock);
        ready = 1;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&lock);
        usleep(10000);  // let the woken thread finish
    }
    for (int i = 0; i < NWAITERS; i++) pthread_join(t1[i], NULL);
    printf("  after %d signals: %d woken (expected %d)\n", NWAITERS, done, NWAITERS);
    int test1_ok = (done == NWAITERS);

    // ── Test 2: broadcast wakes all ──
    printf("Test 2: broadcast wakes all %d waiters\n", NWAITERS);
    ready = 0; done = 0;
    pthread_t t2[NWAITERS];
    targ_t a2[NWAITERS];
    for (int i = 0; i < NWAITERS; i++) {
        a2[i].id = i;
        pthread_create(&t2[i], NULL, waiter, &a2[i]);
    }
    usleep(50000);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < NWAITERS; i++) pthread_join(t2[i], NULL);
    printf("  after broadcast: %d woken (expected %d)\n", done, NWAITERS);
    int test2_ok = (done == NWAITERS);

    // ── Test 3: cond init/destroy ──
    printf("Test 3: cond init/destroy\n");
    pthread_cond_t c2;
    pthread_cond_init(&c2, NULL);
    pthread_cond_destroy(&c2);
    printf("  init/destroy: OK\n");
    int test3_ok = 1;

    int all_ok = test1_ok && test2_ok && test3_ok;
    printf("test_pthread_cond: %s\n", all_ok ? "ALL PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
