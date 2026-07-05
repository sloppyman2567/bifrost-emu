// test_sig_sa_mask.c — Verify sa_mask blocks additional signals during handler.
//
// What it verifies:
//   1. When sa_mask includes SIGUSR2, raising SIGUSR2 inside the SIGUSR1
//      handler queues it as pending instead of interrupting.
//   2. After the handler returns (rt_sigreturn restores the original mask),
//      the queued SIGUSR2 is delivered.
//   3. The handler for SIGUSR2 runs exactly once after SIGUSR1's handler
//      completes.
//
// Build: make cross SRC=ctest_real/test_sig_sa_mask.c OUT=ctest_real/test_sig_sa_mask.elf
// Run:   ./bifrost-emu ctest_real/test_sig_sa_mask.elf
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

static volatile int usr1_in_handler = 0;
static volatile int usr2_in_handler = 0;
static volatile int usr2_count = 0;
static volatile int usr1_count = 0;

static void handler_usr2(int sig) {
    (void)sig;
    usr2_count++;
    // If we're inside usr1's handler when this fires, it means sa_mask
    // did NOT block SIGUSR2 — that's a bug.
    if (usr1_in_handler) {
        usr2_in_handler = 1;
    }
}

static void handler_usr1(int sig) {
    (void)sig;
    usr1_count++;
    usr1_in_handler = 1;
    // Raise SIGUSR2 — should be blocked (it's in our sa_mask).
    raise(SIGUSR2);
    // Check: SIGUSR2 should NOT have fired yet.
    if (usr2_count == 0) {
        // Good — it's blocked, queued as pending.
    } else {
        // Bad — it fired during our handler.
        usr2_in_handler = 1;
    }
    usr1_in_handler = 0;
}

int main(void) {
    struct sigaction sa;

    // Install SIGUSR2 handler first (simple, no mask).
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr2;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);

    // Install SIGUSR1 handler with SIGUSR2 in sa_mask.
    // During handler_usr1, SIGUSR2 should be blocked.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGUSR1, &sa, NULL);

    // Reset state.
    usr1_count = 0;
    usr2_count = 0;
    usr2_in_handler = 0;

    // Trigger.
    raise(SIGUSR1);

    // After SIGUSR1's handler returns, the original mask is restored,
    // and the pending SIGUSR2 should be delivered.
    char msg[160];
    int len = snprintf(msg, sizeof(msg),
                       "usr1_count=%d usr2_count=%d usr2_in_handler=%d\n",
                       usr1_count, usr2_count, usr2_in_handler);
    write(2, msg, len);

    CHECK(usr1_count == 1, "SIGUSR1 handler ran exactly once");
    CHECK(usr2_count == 1, "SIGUSR2 handler ran exactly once (after unblock)");
    CHECK(usr2_in_handler == 0, "SIGUSR2 did NOT fire inside SIGUSR1 handler");

    // Final summary.
    len = snprintf(msg, sizeof(msg),
                   "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
