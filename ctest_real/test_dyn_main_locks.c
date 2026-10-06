// test_dyn_main_locks.c — owner-checked locks on the glibc main thread.
//
// glibc's ld.so stores the main thread's tid in its struct pthread
// (__tls_init_tp). Bifrost's native linker skipped that, so pd->tid was 0
// and every rwlock and error-checking/recursive mutex on the main thread
// saw itself as the owner: pthread_rwlock_rdlock/wrlock and
// pthread_mutex_lock returned EDEADLK. OpenSSL 3 takes an rwlock while
// creating its default library context, so no provider could load.
//
// glibc-dynamic test: run with BIFROST_ROOT=rootfs. "ALL PASS" marker.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char* what) {
    printf("%s: %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

static pthread_rwlock_t shared = PTHREAD_RWLOCK_INITIALIZER;
static int worker_trywr = -1, worker_rd = -1;

static void* worker(void* arg) {
    worker_trywr = pthread_rwlock_trywrlock(&shared);   // main holds rdlock
    worker_rd = pthread_rwlock_rdlock(&shared);
    if (worker_rd == 0) pthread_rwlock_unlock(&shared);
    return arg;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);  // keep progress visible if a lock hangs
    pthread_rwlock_t rw;
    pthread_rwlock_init(&rw, NULL);
    check(pthread_rwlock_rdlock(&rw) == 0, "main rwlock rdlock");
    check(pthread_rwlock_unlock(&rw) == 0, "main rwlock rdlock unlock");
    check(pthread_rwlock_wrlock(&rw) == 0, "main rwlock wrlock");
    check(pthread_rwlock_trywrlock(&rw) == EBUSY, "main rwlock trywrlock while held");
    check(pthread_rwlock_unlock(&rw) == 0, "main rwlock wrlock unlock");
    pthread_rwlock_destroy(&rw);

    pthread_mutexattr_t attr;
    pthread_mutex_t m;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m, &attr);
    check(pthread_mutex_lock(&m) == 0, "errorcheck mutex lock");
    check(pthread_mutex_lock(&m) == EDEADLK, "errorcheck mutex relock is EDEADLK");
    check(pthread_mutex_unlock(&m) == 0, "errorcheck mutex unlock");
    check(pthread_mutex_unlock(&m) == EPERM, "errorcheck mutex unlock unowned is EPERM");
    pthread_mutex_destroy(&m);

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &attr);
    check(pthread_mutex_lock(&m) == 0 && pthread_mutex_lock(&m) == 0,
          "recursive mutex locks twice");
    check(pthread_mutex_unlock(&m) == 0 && pthread_mutex_unlock(&m) == 0,
          "recursive mutex unlocks twice");
    pthread_mutex_destroy(&m);
    pthread_mutexattr_destroy(&attr);

    pthread_t t;
    check(pthread_rwlock_rdlock(&shared) == 0, "main holds shared rdlock");
    check(pthread_create(&t, NULL, worker, NULL) == 0 && pthread_join(t, NULL) == 0,
          "worker thread runs");
    check(worker_trywr == EBUSY, "worker trywrlock sees main's reader");
    check(worker_rd == 0, "worker shares the read lock");
    check(pthread_rwlock_unlock(&shared) == 0, "main releases shared rdlock");
    check(pthread_rwlock_wrlock(&shared) == 0 && pthread_rwlock_unlock(&shared) == 0,
          "main wrlock after worker");

    printf("test_dyn_main_locks: %s\n", failures ? "FAIL" : "ALL PASS");
    return failures ? 1 : 0;
}
