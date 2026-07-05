// test_sig_nested.c — Verify nested signal delivery.
//
// What it verifies:
//   1. A signal handler can itself be interrupted by another signal.
//   2. Both handlers run to completion.
//   3. The nesting order is correct (inner handler returns first, then outer).
//   4. The saved CPU state is correctly restored at each level.
//
// Build: make cross SRC=ctest_real/test_sig_nested.c OUT=ctest_real/test_sig_nested.elf
// Run:   ./bifrost-emu ctest_real/test_sig_nested.elf
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

// Order recorder: appends one char per handler entry/exit.
// E.g. "1in2in2out1out" means handler1 entered, handler2 entered (nested),
// handler2 returned, handler1 returned.
static char order_buf[64];
static size_t order_len = 0;

static void append_order(const char *s) {
    while (*s && order_len < sizeof(order_buf) - 1) {
        order_buf[order_len++] = *s++;
    }
    order_buf[order_len] = 0;
}

static void handler1(int sig) {
    (void)sig;
    append_order("1in");
    // Raise SIGUSR2 — since SIGUSR2 is NOT in our sa_mask (we set it
    // explicitly empty), it can interrupt us.
    raise(SIGUSR2);
    append_order("1out");
}

static void handler2(int sig) {
    (void)sig;
    append_order("2in");
    append_order("2out");
}

int main(void) {
    struct sigaction sa;

    // Install SIGUSR1 → handler1 (no extra mask, allows nesting).
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler1;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    // Install SIGUSR2 → handler2.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler2;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);

    // Trigger the chain: SIGUSR1 → handler1 → raise SIGUSR2 → handler2.
    order_len = 0;
    raise(SIGUSR1);

    // After everything returns, we expect: 1in 2in 2out 1out
    // (handler1 enters, raises SIGUSR2, handler2 enters and returns,
    //  then handler1 continues and returns.)
    char expected[32];
    int n = snprintf(expected, sizeof(expected), "1in2in2out1out");
    (void)n;

    char msg[160];
    int len = snprintf(msg, sizeof(msg),
                       "order=%s expected=%s\n", order_buf, expected);
    write(2, msg, len);

    CHECK(strcmp(order_buf, expected) == 0,
          "nested signal order is 1in2in2out1out");

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
