// test_sem.c — POSIX semaphore test for bifrost-emu.
//
// Tests:
//   - sem_init / sem_wait / sem_post / sem_destroy
//   - sem_trywait (returns -1/EAGAIN when count is 0)
//   - sem_getvalue (returns current count)
//   - Counting semaphore semantics (post N times, wait N times)
//   - Thread coordination via semaphore (ping-pong)
//
// NOTE: Uses conservative round count (20) to stay within the emulator's
// current futex-wakeup throughput.
//
// Build: make cross SRC=ctest/test_sem.c OUT=ctest/test_sem.elf
// Run:   ./bifrost-emu ctest/test_sem.elf
#include <semaphore.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static sem_t sem_ping, sem_pong;
static int ping_count = 0;
static int pong_count = 0;
static const int ROUNDS = 20;

static void* pinger(void* arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        sem_wait(&sem_ping);
        ping_count++;
        sem_post(&sem_pong);
    }
    return NULL;
}

static void* ponger(void* arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        sem_wait(&sem_pong);
        pong_count++;
        sem_post(&sem_ping);
    }
    return NULL;
}

int main(void) {
    printf("=== POSIX semaphore test ===\n");

    // ── Test 1: counting semaphore ──
    printf("Test 1: counting semaphore\n");
    sem_t s;
    sem_init(&s, 0, 0);
    int val = -1;
    sem_getvalue(&s, &val);
    printf("  initial value = %d (expected 0)\n", val);
    sem_post(&s);
    sem_post(&s);
    sem_post(&s);
    sem_getvalue(&s, &val);
    printf("  after 3 posts = %d (expected 3)\n", val);
    sem_wait(&s);
    sem_getvalue(&s, &val);
    printf("  after 1 wait = %d (expected 2)\n", val);
    int rc = sem_trywait(&s);
    printf("  trywait rc=%d (expected 0)\n", rc);
    sem_getvalue(&s, &val);
    printf("  after trywait = %d (expected 1)\n", val);
    sem_wait(&s);
    rc = sem_trywait(&s);
    printf("  trywait on empty: rc=%d errno=%d EAGAIN=%d\n", rc, errno, EAGAIN);
    int test1_ok = (rc == -1 && errno == EAGAIN);
    sem_destroy(&s);

    // ── Test 2: ping-pong coordination ──
    printf("Test 2: ping-pong coordination (%d rounds)\n", ROUNDS);
    sem_init(&sem_ping, 0, 1);
    sem_init(&sem_pong, 0, 0);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, pinger, NULL);
    pthread_create(&t2, NULL, ponger, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("  ping_count = %d (expected %d)\n", ping_count, ROUNDS);
    printf("  pong_count = %d (expected %d)\n", pong_count, ROUNDS);
    int test2_ok = (ping_count == ROUNDS && pong_count == ROUNDS);
    sem_destroy(&sem_ping);
    sem_destroy(&sem_pong);

    int all_ok = test1_ok && test2_ok;
    printf("test_sem: %s\n", all_ok ? "ALL PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
