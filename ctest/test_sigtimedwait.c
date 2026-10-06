// test_sigtimedwait.c — rt_sigtimedwait / sigwait semantics.
//
// rt_sigtimedwait used to be a stub that always returned EAGAIN, so
// sigwait() never blocked. ART's signal-catcher thread spun on it at
// ~1M calls/s and the VM never finished starting.
//
// Runs under both JIT and --no-jit; "checks passed" is the pass marker.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

static int checks = 0, failures = 0;

static void check(int cond, const char* what) {
    checks++;
    printf("%s: %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

static volatile sig_atomic_t usr1_handled = 0, alrm_handled = 0;
static void on_usr1(int s) { (void)s; usr1_handled = 1; }
static void on_alrm(int s) { (void)s; alrm_handled = 1; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static pthread_t main_thread;
static void* sender(void* arg) {
    (void)arg;
    struct timespec d = {0, 30 * 1000 * 1000};
    nanosleep(&d, NULL);
    pthread_kill(main_thread, SIGUSR2);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGUSR1, on_usr1);
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    struct timespec zero = {0, 0};
    siginfo_t info;
    errno = 0;
    check(sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN,
          "nothing pending + zero timeout -> EAGAIN");

    raise(SIGUSR1);
    memset(&info, 0, sizeof(info));
    check(sigtimedwait(&set, &info, &zero) == SIGUSR1, "pending SIGUSR1 is returned");
    check(info.si_signo == SIGUSR1, "siginfo carries si_signo");
    check(!usr1_handled, "dequeued signal does not run its handler");
    sigset_t pending;
    sigpending(&pending);
    check(!sigismember(&pending, SIGUSR1), "dequeued signal is no longer pending");

    struct timespec t50 = {0, 50 * 1000 * 1000};
    double start = now_ms();
    errno = 0;
    int r = sigtimedwait(&set, &info, &t50);
    double waited = now_ms() - start;
    check(r == -1 && errno == EAGAIN, "50 ms timeout -> EAGAIN");
    check(waited >= 40.0, "timeout actually waits");

    main_thread = pthread_self();
    pthread_t t;
    pthread_create(&t, NULL, sender, NULL);
    struct timespec t2s = {2, 0};
    start = now_ms();
    r = sigtimedwait(&set, &info, &t2s);
    waited = now_ms() - start;
    pthread_join(t, NULL);
    check(r == SIGUSR2, "signal from another thread wakes the waiter");
    check(waited < 1000.0, "cross-thread wake is prompt");

    raise(SIGUSR1);
    int sig = 0;
    check(sigwait(&set, &sig) == 0 && sig == SIGUSR1, "libc sigwait returns SIGUSR1");

    // An unrelated, unblocked signal runs its handler and ends the wait.
    // Use the raw syscall: musl's sigtimedwait() retries on EINTR.
    signal(SIGALRM, on_alrm);
    struct itimerval it = {{0, 0}, {0, 20 * 1000}};
    setitimer(ITIMER_REAL, &it, NULL);
    long rr = syscall(SYS_rt_sigtimedwait, &set, &info, &t2s, 8);
    check(rr == -1 && errno == EINTR, "other signal interrupts with EINTR");
    check(alrm_handled, "interrupting handler ran");
    sigset_t cur;
    pthread_sigmask(SIG_SETMASK, NULL, &cur);
    check(sigismember(&cur, SIGUSR1) && sigismember(&cur, SIGUSR2) &&
          !sigismember(&cur, SIGALRM), "caller's mask is restored");

    struct timespec bad = {0, 1000000000};
    errno = 0;
    check(sigtimedwait(&set, &info, &bad) == -1 && errno == EINVAL,
          "invalid tv_nsec -> EINVAL");
    errno = 0;
    check(syscall(SYS_rt_sigtimedwait, &set, &info, &zero, 4) == -1 && errno == EINVAL,
          "wrong sigsetsize -> EINVAL");

    pthread_sigmask(SIG_SETMASK, &old, NULL);
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
