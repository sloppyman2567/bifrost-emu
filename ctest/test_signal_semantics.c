// test_signal_semantics.c — dedicated signal-delivery regression tests.
//
// Covers the 1.5.5-alpha fixes:
//   1. rt_sigaction aliasing: act == oldact must behave like a swap
//      (the new action is read BEFORE the old one is written back).
//   2. sigsuspend restores the PRE-suspend mask after a caught signal
//      (the handler frame must save the original mask, not the temporary
//      one passed to sigsuspend).
//   3. Cross-thread delivery: pthread_kill -> tgkill -> per-CPU pending
//      queue -> per-CPU signal frame stack.
//
// Build: make setup-tests
// Run:   ./bifrost-emu ctest/test_signal_semantics.elf
// Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/mman.h>

static volatile sig_atomic_t g_got = 0;
static void h_simple(int sig) { g_got = sig; }

// ── 1. sigaction aliasing ─────────────────────────────────────────────
static int test_sigaction_alias(void) {
    struct sigaction a, b, q;
    memset(&a, 0, sizeof a);
    a.sa_handler = h_simple;
    sigemptyset(&a.sa_mask);
    if (sigaction(SIGUSR1, &a, NULL) != 0) return -1;

    memset(&b, 0, sizeof b);
    b.sa_handler = SIG_IGN;
    sigemptyset(&b.sa_mask);
    // Alias in/out: must read the NEW action before writing the OLD one.
    if (sigaction(SIGUSR1, &b, &b) != 0) return -2;

    memset(&q, 0, sizeof q);
    if (sigaction(SIGUSR1, NULL, &q) != 0) return -3;
    int installed_ok = (q.sa_handler == SIG_IGN);
    int old_ok       = (b.sa_handler == h_simple);

    // restore the handler for later tests
    sigaction(SIGUSR1, &a, NULL);
    if (!installed_ok) return -4;
    if (!old_ok)       return -5;
    return 0;
}

// ── 2. sigsuspend mask restore ────────────────────────────────────────
static volatile sig_atomic_t g_susp_count = 0;
static void h_susp(int sig) { (void)sig; g_susp_count++; }

static int test_sigsuspend(void) {
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = h_susp;
    sigemptyset(&a.sa_mask);
    if (sigaction(SIGUSR1, &a, NULL) != 0) return -1;

    // Block both signals, then make SIGUSR1 pending while blocked.
    sigset_t base, empty, cur, orig;
    sigemptyset(&base);
    sigaddset(&base, SIGUSR1);
    sigaddset(&base, SIGUSR2);            // marker: must remain blocked
    if (sigprocmask(SIG_SETMASK, &base, &orig) != 0) return -2;

    g_susp_count = 0;
    raise(SIGUSR1);                        // pending (blocked)

    // sigsuspend with an empty mask temporarily unblocks SIGUSR1, so the
    // pending signal is delivered; on return the PRE-suspend mask must be
    // in effect again.
    sigemptyset(&empty);
    errno = 0;
    int r = sigsuspend(&empty);
    int e = errno;

    memset(&cur, 0, sizeof cur);
    if (sigprocmask(SIG_SETMASK, NULL, &cur) != 0) return -3;
    int usr2_blocked = sigismember(&cur, SIGUSR2);
    int usr1_blocked = sigismember(&cur, SIGUSR1);
    sigprocmask(SIG_SETMASK, &orig, NULL); // restore the ORIGINAL mask

    if (r != -1 || e != EINTR) return -4;
    if (g_susp_count == 0)     return -5;
    if (!usr2_blocked)         return -6;   // pre-fix: temporary mask leaked
    if (!usr1_blocked)         return -7;
    return 0;
}

// ── 3. cross-thread signal storm (per-CPU frames + pending queue) ─────
#define XT_N 16
static volatile sig_atomic_t g_recv = 0;
static volatile int g_recv_tid = 0;
static void h_recv(int sig) { (void)sig; g_recv++; }

// The sender targets the receiver's actual guest TID via tgkill, so this
// exercises the emulator's cross-thread pending queue directly (rather
// than depending on musl's pthread_kill internals).
static void* recv_loop(void* arg) {
    (void)arg;
    g_recv_tid = (int)syscall(SYS_gettid);
    // Raw syscalls (no libc cancellation mask) so the receiver crosses the
    // emulator's 4096-instruction pending-drain boundary quickly.
    unsigned long spins = 0;
    while (g_recv < XT_N && spins < 100000000UL) {
        syscall(SYS_getpid);
        spins++;
    }
    return NULL;
}
static void* send_loop(void* arg) {
    (void)arg;
    int mypid = (int)syscall(SYS_getpid);
    while (g_recv_tid == 0) usleep(1000);
    for (int i = 0; i < XT_N; i++) {
        usleep(2000);
        syscall(SYS_tgkill, mypid, g_recv_tid, SIGUSR2);
    }
    return NULL;
}

static int test_cross_thread(void) {
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = h_recv;
    sigemptyset(&a.sa_mask);
    if (sigaction(SIGUSR2, &a, NULL) != 0) return -1;

    g_recv = 0;
    g_recv_tid = 0;
    pthread_t rt;
    if (pthread_create(&rt, NULL, recv_loop, NULL) != 0) return -2;
    pthread_t s;
    if (pthread_create(&s, NULL, send_loop, NULL) != 0) return -3;
    pthread_join(s, NULL);
    pthread_join(rt, NULL);

    if (g_recv != XT_N) return -4;
    return 0;
}

// Blocked signals must remain pending without hiding an unblocked signal
// that interrupts a futex, host read, or all-but-one sigsuspend mask.
static atomic_int wait_tid, wait_done;
static volatile sig_atomic_t wait_usr1, wait_usr2;
static int wait_kind, wait_word, wait_pipe[2], wait_result, wait_errno;
static void h_wait(int sig) {
    if (sig == SIGUSR1) ++wait_usr1;
    if (sig == SIGUSR2) ++wait_usr2;
}
static void* blocked_wait(void* unused) {
    (void)unused;
    sigset_t blocked, original;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, &original);
    atomic_store(&wait_tid, (int)syscall(SYS_gettid));
    errno = 0;
    if (wait_kind == 0) {
        struct timespec timeout = {2, 0};
        wait_result = syscall(SYS_futex, &wait_word, FUTEX_WAIT_PRIVATE,
                              0, &timeout, NULL, 0);
    } else if (wait_kind == 1) {
        char byte;
        wait_result = read(wait_pipe[0], &byte, 1);
    } else {
        sigset_t mask;
        sigfillset(&mask);
        sigdelset(&mask, SIGUSR2);
        wait_result = sigsuspend(&mask);
    }
    wait_errno = errno;
    // The earlier blocked signal has not run inside the interrupted wait.
    if (wait_usr1 != 0) wait_result = -2;
    pthread_sigmask(SIG_SETMASK, &original, NULL);
    for (int i = 0; i < 10000 && !wait_usr1; ++i) syscall(SYS_getpid);
    atomic_store(&wait_done, 1);
    return NULL;
}
static int test_blocked_wait(int kind) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = h_wait;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGUSR2, &action, NULL);
    wait_kind = kind;
    wait_word = wait_usr1 = wait_usr2 = 0;
    atomic_store(&wait_tid, 0);
    atomic_store(&wait_done, 0);
    if (pipe(wait_pipe) != 0) return -1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, blocked_wait, NULL) != 0) return -2;
    while (!atomic_load(&wait_tid)) usleep(1000);
    usleep(30000);
    syscall(SYS_tgkill, getpid(), atomic_load(&wait_tid), SIGUSR1);
    usleep(10000);
    syscall(SYS_tgkill, getpid(), atomic_load(&wait_tid), SIGUSR2);
    // An outer alarm bounds the sigsuspend case even on a broken emulator.
    for (int i = 0; i < 300 && !atomic_load(&wait_done); ++i) usleep(1000);
    if (!atomic_load(&wait_done) && kind == 1) write(wait_pipe[1], "x", 1);
    pthread_join(thread, NULL);
    close(wait_pipe[0]);
    close(wait_pipe[1]);
    if (wait_result != -1 || wait_errno != EINTR || wait_usr1 != 1 || wait_usr2 != 1)
        printf("wait kind=%d result=%d errno=%d usr1=%d usr2=%d\n",
               kind, wait_result, wait_errno, (int)wait_usr1, (int)wait_usr2);
    return wait_result == -1 && wait_errno == EINTR &&
           wait_usr1 == 1 && wait_usr2 == 1 ? 0 : -3;
}

// A signal at SVC entry must not drop an unstarted syscall. In particular,
// returning mmap's NULL hint as a successful result corrupts the linker.
static atomic_int boundary_stop;
static int boundary_pid, boundary_tid;
static volatile sig_atomic_t boundary_hits;
static void h_boundary(int sig) { (void)sig; ++boundary_hits; }
static void* boundary_sender(void* arg) {
    (void)arg;
    while (!atomic_load(&boundary_stop)) {
        syscall(SYS_tgkill, boundary_pid, boundary_tid, SIGUSR1);
        usleep(50);
    }
    return NULL;
}
static int test_syscall_entry(void) {
    struct sigaction action, old;
    memset(&action, 0, sizeof action);
    action.sa_handler = h_boundary;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &old) != 0) return -1;
    boundary_pid = getpid();
    boundary_tid = syscall(SYS_gettid);
    boundary_hits = 0;
    atomic_store(&boundary_stop, 0);
    pthread_t sender;
    if (pthread_create(&sender, NULL, boundary_sender, NULL) != 0) {
        sigaction(SIGUSR1, &old, NULL);
        return -2;
    }
    int bad_pid = 0, bad_map = 0, bad_unmap = 0;
    for (int i = 0; i < 10000; ++i) {
        // getpid ignores its arguments; a leaked first argument is visible.
        if (syscall(SYS_getpid, 0x12345678) != boundary_pid) ++bad_pid;
        void* map = (void*)syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map == NULL || map == MAP_FAILED) {
            ++bad_map;
        } else {
            *(volatile int*)map = i;
            if (syscall(SYS_munmap, map, 4096) != 0) ++bad_unmap;
        }
    }
    atomic_store(&boundary_stop, 1);
    pthread_join(sender, NULL);
    sigaction(SIGUSR1, &old, NULL);
    printf("syscall entry signals: hits=%d bad_pid=%d bad_map=%d bad_unmap=%d\n",
           (int)boundary_hits, bad_pid, bad_map, bad_unmap);
    return boundary_hits && !bad_pid && !bad_map && !bad_unmap ? 0 : -3;
}

int main(void) {
    alarm(15);
    setvbuf(stdout, NULL, _IONBF, 0);   // progress visible even if we hang
    printf("=== signal semantics test ===\n");
    int r1 = test_sigaction_alias();
    printf("sigaction alias: %s\n", r1 == 0 ? "ok" : "bad");
    int r2 = test_sigsuspend();
    printf("sigsuspend mask: %s\n", r2 == 0 ? "ok" : "bad");
    int r3 = test_cross_thread();
    printf("cross-thread delivery: %s (%d/%d)\n",
           r3 == 0 ? "ok" : "bad", (int)g_recv, XT_N);
    int r4 = test_blocked_wait(0);
    int r5 = test_blocked_wait(1);
    int r6 = test_blocked_wait(2);
    printf("cross-thread blocked waits: futex=%d read=%d sigsuspend=%d\n", r4, r5, r6);
    int r7 = test_syscall_entry();
    if (r1 || r2 || r3 || r4 || r5 || r6 || r7) {
        printf("FAIL (r1=%d r2=%d r3=%d r4=%d r5=%d r6=%d r7=%d)\n", r1, r2, r3, r4, r5, r6, r7);
        return 1;
    }
    printf("signal semantics: ALL PASS\n");
    return 0;
}
