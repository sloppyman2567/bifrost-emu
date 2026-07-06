// test_sig_pending.c — Verify multiple pending signals are delivered on unblock.
//
// What it verifies:
//   1. Blocking SIGUSR1 and SIGUSR2, then raising both, queues both as pending.
//   2. sigpending() reports both as pending.
//   3. On unblock, BOTH signals are delivered (in some order — kernel chooses).
//   4. Each handler runs exactly once.
//
// Build: make cross SRC=ctest_real/test_sig_pending.c OUT=ctest_real/test_sig_pending.elf
// Run:   ./bifrost-emu ctest_real/test_sig_pending.elf
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

static volatile int usr1_count = 0;
static volatile int usr2_count = 0;

static void handler1(int sig) { (void)sig; usr1_count++; }
static void handler2(int sig) { (void)sig; usr2_count++; }

int main(void) {
    struct sigaction sa;
    sigset_t mask, pending;

    // Install handlers.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);

    // Block both.
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    int r = sigprocmask(SIG_BLOCK, &mask, NULL);
    CHECK(r == 0, "sigprocmask SIG_BLOCK USR1+USR2 returns 0");

    // Raise both — should be queued, not delivered.
    usr1_count = 0;
    usr2_count = 0;
    raise(SIGUSR1);
    raise(SIGUSR2);

    CHECK(usr1_count == 0, "SIGUSR1 queued (not delivered while blocked)");
    CHECK(usr2_count == 0, "SIGUSR2 queued (not delivered while blocked)");

    // Verify pending.
    sigemptyset(&pending);
    r = sigpending(&pending);
    CHECK(r == 0, "sigpending returns 0");
    CHECK(sigismember(&pending, SIGUSR1), "SIGUSR1 is pending");
    CHECK(sigismember(&pending, SIGUSR2), "SIGUSR2 is pending");

    // Unblock — both should be delivered.
    r = sigprocmask(SIG_UNBLOCK, &mask, NULL);
    CHECK(r == 0, "sigprocmask SIG_UNBLOCK returns 0");
    CHECK(usr1_count == 1, "SIGUSR1 delivered on unblock");
    CHECK(usr2_count == 1, "SIGUSR2 delivered on unblock");

    // Final summary.
    char msg[128];
    int len = snprintf(msg, sizeof(msg),
                       "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
