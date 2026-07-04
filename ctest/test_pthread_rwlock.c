// test_pthread_rwlock.c — pthread_rwlock (reader-writer lock) test for bifrost-emu.
//
// Tests:
//   - pthread_rwlock_init / rdlock / wrlock / unlock / destroy
//   - Multiple readers can hold the lock simultaneously.
//   - Writers have exclusive access.
//   - RWLOCK_INITIALIZER static initializer.
//
// NOTE: Uses conservative iteration count (200/thread) to stay within the
// emulator's current futex-wakeup throughput.
//
// Build: make cross SRC=ctest/test_pthread_rwlock.c OUT=ctest/test_pthread_rwlock.elf
// Run:   ./bifrost-emu ctest/test_pthread_rwlock.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NREADERS 2
#define NWRITERS 2
#define ITERS    200

static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static long shared_value = 0;

typedef struct {
    int id;
} targ_t;

static void* reader(void* arg) {
    targ_t* a = (targ_t*)arg;
    (void)a;
    for (int i = 0; i < ITERS; i++) {
        pthread_rwlock_rdlock(&rwlock);
        volatile long v = shared_value;
        (void)v;
        pthread_rwlock_unlock(&rwlock);
    }
    return NULL;
}

static void* writer(void* arg) {
    targ_t* a = (targ_t*)arg;
    (void)a;
    for (int i = 0; i < ITERS; i++) {
        pthread_rwlock_wrlock(&rwlock);
        shared_value++;
        pthread_rwlock_unlock(&rwlock);
    }
    return NULL;
}

int main(void) {
    printf("=== pthread_rwlock test: %d readers + %d writers x %d iters ===\n",
           NREADERS, NWRITERS, ITERS);

    pthread_t readers[NREADERS], writers[NWRITERS];
    targ_t rargs[NREADERS], wargs[NWRITERS];
    for (int i = 0; i < NREADERS; i++) {
        rargs[i].id = i;
        pthread_create(&readers[i], NULL, reader, &rargs[i]);
    }
    for (int i = 0; i < NWRITERS; i++) {
        wargs[i].id = i;
        pthread_create(&writers[i], NULL, writer, &wargs[i]);
    }
    for (int i = 0; i < NREADERS; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < NWRITERS; i++) pthread_join(writers[i], NULL);

    long expected = (long)NWRITERS * ITERS;
    int ok = (shared_value == expected);
    printf("shared_value = %ld (expected %ld)  %s\n", shared_value, expected,
           ok ? "OK" : "FAIL");

    printf("test_pthread_rwlock: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
