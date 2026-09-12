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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // progress visible even if we hang
    printf("=== signal semantics test ===\n");
    int r1 = test_sigaction_alias();
    printf("sigaction alias: %s\n", r1 == 0 ? "ok" : "bad");
    int r2 = test_sigsuspend();
    printf("sigsuspend mask: %s\n", r2 == 0 ? "ok" : "bad");
    int r3 = test_cross_thread();
    printf("cross-thread delivery: %s (%d/%d)\n",
           r3 == 0 ? "ok" : "bad", (int)g_recv, XT_N);
    if (r1 || r2 || r3) {
        printf("FAIL (r1=%d r2=%d r3=%d)\n", r1, r2, r3);
        return 1;
    }
    printf("signal semantics: ALL PASS\n");
    return 0;
}
