// test_sigaction.c — Comprehensive rt_sigaction / signal registration test.
//
// Exercises the signal registration path (rt_sigaction syscall #134, mapped
// to SignalTable::install) plus rt_sigprocmask (#135), rt_sigpending (#136),
// rt_sigsuspend (#133), and rt_sigreturn (#139) to validate end-to-end
// behavior of the signal subsystem.
//
// What it verifies:
//   1. Installing a handler returns 0 and the handler is invoked on signal.
//   2. Querying (sigaction with old_act only) does NOT mutate the handler
//      (the Turn 42 regression). Verifies that a NULL `act` keeps the
//      previously-installed handler.
//   3. SA_RESETHAND resets the handler to SIG_DFL after one delivery.
//   4. SIG_IGN disposition drops the signal silently.
//   5. SIGKILL cannot be caught (sigaction returns EINVAL).
//   6. rt_sigprocmask SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK round-trips.
//   7. A blocked signal is queued as pending and delivered on unblock.
//   8. raise() delivers a signal to the calling thread.
//
// Build: make cross SRC=ctest_real/test_sigaction.c OUT=ctest_real/test_sigaction.elf
// Run:   ./bifrost-emu ctest_real/test_sigaction.elf
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int failures = 0;
static int passes = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

static volatile sig_atomic_t usr1_count = 0;
static volatile sig_atomic_t last_sig = 0;

static void handler_count(int sig) {
    usr1_count++;
    last_sig = sig;
}

static void handler_record(int sig) {
    last_sig = sig;
}

int main(void) {
    struct sigaction sa, old_sa;
    sigset_t mask, old_mask, pending;

    // 1. Install a SIGUSR1 handler and verify sigaction returns 0.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_count;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    int r = sigaction(SIGUSR1, &sa, NULL);
    CHECK(r == 0, "sigaction install returns 0");

    // 2. raise() and verify handler ran.
    usr1_count = 0;
    r = raise(SIGUSR1);
    CHECK(r == 0 && usr1_count == 1, "raise SIGUSR1 invokes handler");

    // 3. Query-only: act=NULL, old_act=&old_sa must NOT change the handler.
    //    This is the Turn 42 regression: previously the install function
    //    reset the handler to SIG_DFL on a query-only call, breaking
    //    musl's raise() implementation.
    memset(&old_sa, 0, sizeof(old_sa));
    r = sigaction(SIGUSR1, NULL, &old_sa);
    CHECK(r == 0, "sigaction query returns 0");
    CHECK(old_sa.sa_handler == handler_count, "query returns previously installed handler");

    // Verify the handler is still installed (raise should still work).
    usr1_count = 0;
    raise(SIGUSR1);
    CHECK(usr1_count == 1, "handler unchanged after query-only sigaction");

    // 4. SA_RESETHAND: handler should be reset to SIG_DFL after delivery.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_count;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    r = sigaction(SIGUSR2, &sa, NULL);
    CHECK(r == 0, "sigaction SA_RESETHAND install returns 0");

    usr1_count = 0;
    last_sig = 0;
    raise(SIGUSR2);
    CHECK(last_sig == SIGUSR2, "SA_RESETHAND handler ran once");

    // After SA_RESETHAND, querying should show SIG_DFL (handler == NULL or SIG_DFL).
    memset(&old_sa, 0, sizeof(old_sa));
    sigaction(SIGUSR2, NULL, &old_sa);
    CHECK(old_sa.sa_handler == SIG_DFL, "SA_RESETHAND reset handler to SIG_DFL");

    // 5. SIG_IGN: signal should be silently dropped.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    r = sigaction(SIGUSR1, &sa, NULL);
    CHECK(r == 0, "sigaction SIG_IGN install returns 0");

    last_sig = 0;
    raise(SIGUSR1);
    CHECK(last_sig == 0, "SIG_IGN drops the signal (handler not called)");

    // Restore default for SIGUSR1.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    // 6. SIGKILL cannot be caught.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_count;
    sigemptyset(&sa.sa_mask);
    r = sigaction(SIGKILL, &sa, NULL);
    CHECK(r == -1 && errno == EINVAL, "sigaction(SIGKILL) returns EINVAL");

    // SIGSTOP cannot be caught.
    r = sigaction(SIGSTOP, &sa, NULL);
    CHECK(r == -1 && errno == EINVAL, "sigaction(SIGSTOP) returns EINVAL");

    // 7. rt_sigprocmask round-trip: SIG_BLOCK a signal, raise it (should
    //    be queued as pending), then SIG_UNBLOCK to deliver it.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_record;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    r = sigprocmask(SIG_BLOCK, &mask, &old_mask);
    CHECK(r == 0, "sigprocmask SIG_BLOCK returns 0");

    // Raise SIGUSR1 while blocked — should be queued as pending, not delivered.
    last_sig = 0;
    raise(SIGUSR1);
    CHECK(last_sig == 0, "blocked signal is queued, not delivered");

    // Check pending: should have SIGUSR1 in the pending set.
    sigemptyset(&pending);
    r = sigpending(&pending);
    CHECK(r == 0, "sigpending returns 0");
    CHECK(sigismember(&pending, SIGUSR1), "blocked SIGUSR1 is pending");

    // Unblock — should deliver the queued signal synchronously.
    r = sigprocmask(SIG_UNBLOCK, &mask, NULL);
    CHECK(r == 0, "sigprocmask SIG_UNBLOCK returns 0");
    CHECK(last_sig == SIGUSR1, "queued signal delivered on unblock");

    // 8. SIG_SETMASK: restore old mask via SIG_SETMASK.
    r = sigprocmask(SIG_SETMASK, &old_mask, NULL);
    CHECK(r == 0, "sigprocmask SIG_SETMASK returns 0");

    // 9. rt_sigsuspend: basic smoke test. sigsuspend's full async-delivery
    // path is tested separately by test_sigsuspend.elf (which forks a
    // clean child without prior signal handling). Here we just verify
    // that sigsuspend returns -1/EINTR when a signal arrives. We use
    // raise() which delivers synchronously — sigsuspend will be woken
    // by the host signal forwarding path.
    //
    // Set up handler to record the signal.
    sigaction(SIGUSR1, &sa, NULL);
    // Block SIGUSR1 first, then sigsuspend with empty mask to atomically
    // unblock + wait.
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    // Raise SIGUSR1 — it's currently blocked, so it gets queued as
    // pending. Then sigsuspend with empty mask will unblock + the
    // pending signal should be delivered.
    sigemptyset(&mask);  // empty mask = unblock everything during sigsuspend
    last_sig = 0;
    raise(SIGUSR1);  // queues SIGUSR1 as pending (it's blocked)
    // Now sigsuspend — the pending SIGUSR1 should be delivered immediately
    // on entry (since the sigsuspend mask unblocks it).
    r = sigsuspend(&mask);
    CHECK(r == -1 && errno == EINTR, "sigsuspend returns -1 with EINTR");
    CHECK(last_sig == SIGUSR1, "sigsuspend unblocked and delivered SIGUSR1");

    // 10. Final summary.
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
