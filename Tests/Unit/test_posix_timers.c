// test_posix_timers.c — timer_create and friends (syscalls 107-111).
//
// They used to be ENOSYS. Android's media.extractor aborts on that the moment it handles a request, so a Unity game's
// video could never open. Runs under the JIT, --no-jit and qemu-aarch64 as the reference; "ALL PASS" is the marker.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SIGEV_THREAD_ID_RAW 4
#define SI_TIMER_RAW (-2)

static int checks, failures;
static void check(int cond, const char* what) {
    checks++;
    if (!cond) { failures++; printf("FAIL: %s (errno %d)\n", what, errno); }
}

struct raw_sigevent {
    unsigned long value;
    int signo, notify, tid;
    int pad[11];
};
static long raw_create(int clock, const struct raw_sigevent* event, int* id) {
    return syscall(SYS_timer_create, clock, event, id);
}
static long raw_set(int id, int flags, long interval_ns, long value_ns, struct itimerspec* old) {
    struct itimerspec spec = {{interval_ns / 1000000000, interval_ns % 1000000000},
                              {value_ns / 1000000000, value_ns % 1000000000}};
    return syscall(SYS_timer_settime, id, flags, &spec, old);
}
static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, ms * 1000000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static volatile sig_atomic_t fired, handler_code, handler_tid, handler_overrun;
static volatile unsigned long handler_value;
static volatile int handler_timer;
static void on_timer(int signo, siginfo_t* info, void* context) {
    (void)signo; (void)context;
    handler_code = info->si_code;
    handler_value = (unsigned long)info->si_value.sival_ptr;
    handler_timer = info->si_timerid;
    handler_overrun = info->si_overrun;
    handler_tid = (int)syscall(SYS_gettid);
    fired = fired + 1;
}
static void install(int signo) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_timer;
    action.sa_flags = SA_SIGINFO;
    sigaction(signo, &action, NULL);
}

static void test_signal(void) {
    install(SIGUSR1);
    fired = 0;
    struct raw_sigevent event = {0x1234567890UL, SIGUSR1, SIGEV_SIGNAL, 0, {0}};
    int id = -1;
    check(raw_create(CLOCK_MONOTONIC, &event, &id) == 0 && id >= 0, "timer_create");
    check(raw_set(id, 0, 0, 5000000, NULL) == 0, "timer_settime");
    for (int i = 0; i < 200 && !fired; i++) sleep_ms(5);
    check(fired == 1, "one-shot fires once");
    check(handler_code == SI_TIMER_RAW, "si_code is SI_TIMER");
    check(handler_value == 0x1234567890UL, "si_value is sigev_value");
    check(handler_timer == id, "si_timerid is the timer");
    struct itimerspec left;
    check(syscall(SYS_timer_gettime, id, &left) == 0 && left.it_value.tv_sec == 0 && left.it_value.tv_nsec == 0,
          "expired one-shot reads as disarmed");
    sleep_ms(30);
    check(fired == 1, "one-shot does not fire again");
    check(syscall(SYS_timer_delete, id) == 0, "timer_delete");
    check(syscall(SYS_timer_delete, id) < 0 && errno == EINVAL, "deleted timer is EINVAL");
}

static void test_null_event(void) {
    install(SIGALRM);
    fired = 0;
    int id = -1;
    check(raw_create(CLOCK_REALTIME, NULL, &id) == 0, "NULL sigevent");
    check(raw_set(id, 0, 0, 1000000, NULL) == 0, "arm NULL sigevent");
    for (int i = 0; i < 200 && !fired; i++) sleep_ms(5);
    check(fired == 1 && handler_value == (unsigned long)id, "NULL sigevent: SIGALRM carrying the id");
    syscall(SYS_timer_delete, id);
}

// A thread-directed signal interrupts its thread while it spins in guest code.
static volatile int spinner_tid, spinner_stop;
static void* spinner(void* arg) {
    (void)arg;
    spinner_tid = (int)syscall(SYS_gettid);
    while (!spinner_stop) {}
    return NULL;
}
static void test_thread_id(void) {
    install(SIGUSR2);
    fired = 0;
    spinner_stop = 0;
    spinner_tid = 0;
    pthread_t thread;
    pthread_create(&thread, NULL, spinner, NULL);
    while (!spinner_tid) sleep_ms(1);
    struct raw_sigevent event = {7, SIGUSR2, SIGEV_THREAD_ID_RAW, spinner_tid, {0}};
    int id = -1;
    check(raw_create(CLOCK_MONOTONIC, &event, &id) == 0, "SIGEV_THREAD_ID create");
    check(raw_set(id, 0, 0, 2000000, NULL) == 0, "SIGEV_THREAD_ID arm");
    for (int i = 0; i < 400 && !fired; i++) sleep_ms(5);
    check(fired == 1, "SIGEV_THREAD_ID fires");
    check(handler_tid == spinner_tid, "handler runs on the named thread");
    spinner_stop = 1;
    pthread_join(thread, NULL);
    syscall(SYS_timer_delete, id);
}

// While a timer's signal stays blocked one is queued, and expiries raise its overrun.
static void test_overrun(void) {
    sigset_t set, previous;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN + 3);
    pthread_sigmask(SIG_BLOCK, &set, &previous);
    struct raw_sigevent event = {9, SIGRTMIN + 3, SIGEV_THREAD_ID_RAW, (int)syscall(SYS_gettid), {0}};
    int id = -1;
    check(raw_create(CLOCK_MONOTONIC, &event, &id) == 0, "overrun create");
    check(raw_set(id, 0, 2000000, 2000000, NULL) == 0, "overrun arm");
    sleep_ms(80);
    struct timespec none = {1, 0};
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    check(sigtimedwait(&set, &info, &none) == SIGRTMIN + 3, "blocked timer signal waits");
    check(info.si_code == SI_TIMER_RAW && info.si_timerid == id, "sigtimedwait sees the timer");
    check(info.si_value.sival_ptr == (void*)9, "sigtimedwait sees si_value");
    check(info.si_overrun >= 3, "overrun counted the missed expiries");
    struct itimerspec old;
    check(raw_set(id, 0, 0, 0, &old) == 0, "disarm");
    check(old.it_interval.tv_nsec == 2000000, "old_value holds the interval");
    while (sigtimedwait(&set, &info, &(struct timespec){0, 5000000}) > 0) {}
    syscall(SYS_timer_delete, id);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
}

static void test_interval_and_absolute(void) {
    install(SIGUSR1);
    fired = 0;
    struct raw_sigevent event = {1, SIGUSR1, SIGEV_SIGNAL, 0, {0}};
    int id = -1;
    check(raw_create(CLOCK_MONOTONIC, &event, &id) == 0, "interval create");
    check(raw_set(id, 0, 5000000, 5000000, NULL) == 0, "interval arm");
    for (int i = 0; i < 400 && fired < 3; i++) sleep_ms(5);
    check(fired >= 3, "interval keeps firing");
    check(raw_set(id, 0, 0, 0, NULL) == 0, "interval disarm");
    sleep_ms(20);  // a signal already on its way
    const int after_disarm = fired;
    sleep_ms(40);
    check(fired == after_disarm, "disarm stops it");
    // An absolute time already past fires at once.
    fired = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec past = {{0, 0}, {now.tv_sec - 1, now.tv_nsec}};
    check(syscall(SYS_timer_settime, id, TIMER_ABSTIME, &past, NULL) == 0, "TIMER_ABSTIME arm");
    for (int i = 0; i < 200 && !fired; i++) sleep_ms(5);
    check(fired == 1, "TIMER_ABSTIME in the past fires");
    syscall(SYS_timer_delete, id);
}

static void test_none_and_gettime(void) {
    struct raw_sigevent event = {0, 0, SIGEV_NONE, 0, {0}};
    int id = -1;
    check(raw_create(CLOCK_BOOTTIME, &event, &id) == 0, "SIGEV_NONE create");
    check(raw_set(id, 0, 0, 2000000000L, NULL) == 0, "SIGEV_NONE arm");
    sleep_ms(20);
    struct itimerspec left;
    check(syscall(SYS_timer_gettime, id, &left) == 0, "timer_gettime");
    check(left.it_value.tv_sec < 2 && (left.it_value.tv_sec || left.it_value.tv_nsec), "time remaining counts down");
    check(syscall(SYS_timer_getoverrun, id) == 0, "no overrun");
    syscall(SYS_timer_delete, id);
}

static void test_errors(void) {
    struct raw_sigevent good = {0, SIGUSR1, SIGEV_SIGNAL, 0, {0}};
    int id = -1;
    check(raw_create(12345, &good, &id) < 0 && errno == EINVAL, "unknown clock");
    struct raw_sigevent bad_signo = {0, 200, SIGEV_SIGNAL, 0, {0}};
    check(raw_create(CLOCK_MONOTONIC, &bad_signo, &id) < 0 && errno == EINVAL, "bad signal");
    struct raw_sigevent bad_tid = {0, SIGUSR1, SIGEV_THREAD_ID_RAW, 99999, {0}};
    check(raw_create(CLOCK_MONOTONIC, &bad_tid, &id) < 0 && errno == EINVAL, "tid outside the process");
    struct raw_sigevent bad_notify = {0, SIGUSR1, 77, 0, {0}};
    check(raw_create(CLOCK_MONOTONIC, &bad_notify, &id) < 0 && errno == EINVAL, "bad sigev_notify");
    void* gone = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(gone, 4096);
    check(raw_create(CLOCK_MONOTONIC, gone, &id) < 0 && errno == EFAULT, "unreadable sigevent");
    check(syscall(SYS_timer_settime, 4000, 0, &(struct itimerspec){{0, 0}, {1, 0}}, NULL) < 0 && errno == EINVAL,
          "settime on an unknown timer");
    check(syscall(SYS_timer_gettime, 4000, &(struct itimerspec){{0, 0}, {0, 0}}) < 0 && errno == EINVAL,
          "gettime on an unknown timer");
    check(syscall(SYS_timer_getoverrun, 4000) < 0 && errno == EINVAL, "getoverrun on an unknown timer");
    check(raw_create(CLOCK_MONOTONIC, &good, &id) == 0, "create for bad settime");
    check(syscall(SYS_timer_settime, id, 0, NULL, NULL) < 0 && errno == EINVAL, "NULL new_value");
    check(syscall(SYS_timer_settime, id, 0, gone, NULL) < 0 && errno == EFAULT, "unreadable new_value");
    check(syscall(SYS_timer_settime, id, 0, &(struct itimerspec){{0, 0}, {0, 1000000000}}, NULL) < 0 && errno == EINVAL,
          "bad tv_nsec");
    syscall(SYS_timer_delete, id);
}

// A forked child inherits no timers; the parent's keeps running.
static void test_fork(void) {
    install(SIGUSR1);
    fired = 0;
    struct raw_sigevent event = {3, SIGUSR1, SIGEV_SIGNAL, 0, {0}};
    int id = -1;
    check(raw_create(CLOCK_MONOTONIC, &event, &id) == 0, "fork: create");
    check(raw_set(id, 0, 0, 60000000, NULL) == 0, "fork: arm");
    pid_t child = fork();
    if (child == 0) {
        struct itimerspec left;
        if (syscall(SYS_timer_gettime, id, &left) == 0 || errno != EINVAL) _exit(1);
        fired = 0;
        sleep_ms(120);
        if (fired) _exit(2);  // the parent's timer must not signal the child
        int other = -1;
        struct raw_sigevent own = {4, SIGUSR1, SIGEV_SIGNAL, 0, {0}};
        if (raw_create(CLOCK_MONOTONIC, &own, &other) != 0) _exit(3);
        if (raw_set(other, 0, 0, 1000000, NULL) != 0) _exit(4);
        for (int i = 0; i < 200 && !fired; i++) sleep_ms(5);
        _exit(fired && handler_value == 4 ? 0 : 5);
    }
    int status = -1;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}  // the parent's timer fires during the wait
    if (!WIFEXITED(status) || WEXITSTATUS(status)) printf("fork child status 0x%x\n", status);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork: the child has its own timers only");
    for (int i = 0; i < 200 && !fired; i++) sleep_ms(5);
    check(fired >= 1 && handler_value == 3, "fork: the parent's timer still fires");
    syscall(SYS_timer_delete, id);
}

// A default-fatal timer signal ends the process.
static void test_default_fatal(void) {
    pid_t child = fork();
    if (child == 0) {
        signal(SIGVTALRM, SIG_DFL);
        struct raw_sigevent event = {0, SIGVTALRM, SIGEV_SIGNAL, 0, {0}};
        int id = -1;
        if (raw_create(CLOCK_MONOTONIC, &event, &id) != 0) _exit(1);
        if (raw_set(id, 0, 0, 1000000, NULL) != 0) _exit(2);
        for (int i = 0; i < 400; i++) sleep_ms(5);
        _exit(3);
    }
    int status = -1;
    waitpid(child, &status, 0);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGVTALRM, "a default-fatal timer signal kills");
}

// execve destroys timers. The new image is this program with an id to probe.
static void test_exec(const char* self) {
    pid_t child = fork();
    if (child == 0) {
        struct raw_sigevent event = {0, 0, SIGEV_NONE, 0, {0}};
        int id = -1;
        if (raw_create(CLOCK_MONOTONIC, &event, &id) != 0) _exit(1);
        char text[16];
        snprintf(text, sizeof(text), "%d", id);
        execl(self, self, "probe-after-exec", text, (char*)NULL);
        _exit(2);
    }
    int status = -1;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve destroys the timers");
}

// The libc's SIGEV_THREAD, built on SIGEV_THREAD_ID.
static volatile int callback_value;
static void on_callback(union sigval value) { callback_value = value.sival_int; }
static void test_libc_thread(void) {
    struct sigevent event;
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD;
    event.sigev_notify_function = on_callback;
    event.sigev_value.sival_int = 42;
    timer_t timer;
    check(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0, "libc SIGEV_THREAD create");
    struct itimerspec spec = {{0, 0}, {0, 2000000}};
    check(timer_settime(timer, 0, &spec, NULL) == 0, "libc SIGEV_THREAD arm");
    for (int i = 0; i < 400 && !callback_value; i++) sleep_ms(5);
    check(callback_value == 42, "libc SIGEV_THREAD calls back");
    timer_delete(timer);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "probe-after-exec")) {
        int id = 0;
        sscanf(argv[2], "%d", &id);
        struct itimerspec left;
        return syscall(SYS_timer_gettime, id, &left) < 0 && errno == EINVAL ? 0 : 1;
    }
    test_signal();
    test_null_event();
    test_thread_id();
    test_overrun();
    test_interval_and_absolute();
    test_none_and_gettime();
    test_errors();
    test_fork();
    test_default_fatal();
    test_exec(argv[0]);
    test_libc_thread();
    printf("posix_timers: %d checks, %d failed\n", checks, failures);
    if (!failures) printf("posix_timers: ALL PASS\n");
    return failures != 0;
}
