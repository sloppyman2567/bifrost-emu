/* Host-only fault injection for Scripts/Testing/test_audio_callbacks.sh.
 * Widen the predicate-read -> unlock-and-wait interval once per thread.
 * A correct notifier takes the wait mutex and therefore cannot lose its
 * notification in this interval. This is never linked into the emulator. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int injected;

static void delay_once(void) {
    if (!injected) {
        injected = 1;
        const struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000 };
        struct timespec remaining = delay;
        while (nanosleep(&remaining, &remaining) != 0) {
            if (errno != EINTR) _exit(125);
        }
    }
}

int pthread_cond_timedwait(pthread_cond_t* cv, pthread_mutex_t* mu,
                          const struct timespec* until) {
    int (*const real_wait)(pthread_cond_t*, pthread_mutex_t*,
                          const struct timespec*) =
        dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    if (!real_wait) _exit(125);
    delay_once();
    return real_wait(cv, mu, until);
}

/* libstdc++ uses clockwait when available; older glibc uses timedwait. */
int pthread_cond_clockwait(pthread_cond_t* cv, pthread_mutex_t* mu,
                          clockid_t clock, const struct timespec* until) {
    int (*const real_wait)(pthread_cond_t*, pthread_mutex_t*, clockid_t,
                          const struct timespec*) =
        dlsym(RTLD_NEXT, "pthread_cond_clockwait");
    if (!real_wait) _exit(125);
    delay_once();
    return real_wait(cv, mu, clock, until);
}
