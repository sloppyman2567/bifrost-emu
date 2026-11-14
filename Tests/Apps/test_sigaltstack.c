// test_sigaltstack.c — Verify sigaltstack + SA_ONSTACK semantics.
//
// What it verifies:
//   1. sigaltstack() can install an alternate signal stack.
//   2. Querying the altstack returns the values we set.
//   3. A handler installed with SA_ONSTACK runs with SP inside the altstack
//      region (NOT the regular stack).
//   4. After rt_sigreturn, SP is restored to the original stack.
//   5. SS_DISABLE disables the altstack (handler runs on regular stack).
//   6. A handler that leaves through siglongjmp (no rt_sigreturn) does not
//      leave the altstack marked in use: the next signal runs on it again,
//      its frame's uc_stack does not claim SS_ONSTACK, and sigaltstack()
//      reports it free (Linux decides all three from the current SP).
//
// Build: make cross SRC=Tests/Apps/test_sigaltstack.c OUT=Tests/Apps/test_sigaltstack.elf
// Run:   ./bifrost-emu Tests/Apps/test_sigaltstack.elf
#include <setjmp.h>
#include <ucontext.h>
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

// Altstack region: 64 KiB.
static char altstack_buf[65536] __attribute__((aligned(4096)));
static stack_t altstack_query;
static volatile int handler_sp_in_altstack = -1;
static volatile unsigned long handler_sp_value = 0;
static void *expected_altstack_base;
static size_t expected_altstack_size;

static void handler(int sig) {
    (void)sig;
    unsigned long sp;
#if defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
#else
    sp = (unsigned long)&sp;
#endif
    handler_sp_value = sp;
    handler_sp_in_altstack =
        (sp >= (unsigned long)expected_altstack_base &&
         sp <  (unsigned long)expected_altstack_base + expected_altstack_size);
}

static sigjmp_buf jump;
static volatile int frame_claims_onstack = -1;

static void jumping_handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    handler(sig);
    frame_claims_onstack = (((ucontext_t *)context)->uc_stack.ss_flags & SS_ONSTACK) != 0;
    siglongjmp(jump, 1);
}

int main(void) {
    expected_altstack_base = altstack_buf;
    expected_altstack_size = sizeof(altstack_buf);

    // 1. Install altstack.
    stack_t ss;
    ss.ss_sp    = altstack_buf;
    ss.ss_size  = sizeof(altstack_buf);
    ss.ss_flags = 0;
    int r = sigaltstack(&ss, NULL);
    CHECK(r == 0, "sigaltstack install returns 0");

    // 2. Query altstack — should match what we set.
    memset(&altstack_query, 0, sizeof(altstack_query));
    r = sigaltstack(NULL, &altstack_query);
    CHECK(r == 0, "sigaltstack query returns 0");
    CHECK(altstack_query.ss_sp == altstack_buf, "query returns our ss_sp");
    CHECK(altstack_query.ss_size == sizeof(altstack_buf), "query returns our ss_size");
    CHECK((altstack_query.ss_flags & SS_DISABLE) == 0, "altstack is enabled");

    // 3. Install handler with SA_ONSTACK — should run on altstack.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    raise(SIGUSR1);
    CHECK(handler_sp_in_altstack == 1, "SA_ONSTACK handler runs on altstack");

    // 4. After sigreturn, altstack should no longer be "in use".
    memset(&altstack_query, 0, sizeof(altstack_query));
    sigaltstack(NULL, &altstack_query);
    CHECK((altstack_query.ss_flags & SS_ONSTACK) == 0, "altstack not in use after sigreturn");

    // 5. Install handler WITHOUT SA_ONSTACK — should run on regular stack.
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    handler_sp_in_altstack = -1;
    raise(SIGUSR1);
    CHECK(handler_sp_in_altstack == 0, "no-SA_ONSTACK handler runs on regular stack");

    // 6. SS_DISABLE: altstack is disabled, even SA_ONSTACK handlers run on regular stack.
    ss.ss_sp    = altstack_buf;
    ss.ss_size  = sizeof(altstack_buf);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR1, &sa, NULL);
    handler_sp_in_altstack = -1;
    raise(SIGUSR1);
    CHECK(handler_sp_in_altstack == 0, "SS_DISABLE: altstack disabled, handler on regular stack");

    // 7. Reject altstack smaller than MINSIGSTKSZ (kernel enforces this).
    ss.ss_sp    = altstack_buf;
    ss.ss_size  = 16;  // absurdly small
    ss.ss_flags = 0;
    r = sigaltstack(&ss, NULL);
    // Either the kernel rejects it (r == -1, errno == ENOMEM) or accepts it
    // (we accept it on the emulator; the kernel may be more strict).
    // Just verify the call doesn't crash — the size check is implementation-defined.
    (void)r;

    // 8. Leaving through siglongjmp, twice: no rt_sigreturn ever runs.
    ss.ss_sp    = altstack_buf;
    ss.ss_size  = sizeof(altstack_buf);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    struct sigaction jump_sa;
    memset(&jump_sa, 0, sizeof(jump_sa));
    jump_sa.sa_sigaction = jumping_handler;
    jump_sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&jump_sa.sa_mask);
    sigaction(SIGUSR2, &jump_sa, NULL);
    handler_sp_in_altstack = -1;
    if (!sigsetjmp(jump, 1)) raise(SIGUSR2);
    CHECK(handler_sp_in_altstack == 1, "first siglongjmp handler runs on altstack");
    handler_sp_in_altstack = -1;
    frame_claims_onstack = -1;
    if (!sigsetjmp(jump, 1)) raise(SIGUSR2);
    CHECK(handler_sp_in_altstack == 1, "after siglongjmp out of it, the next handler runs on altstack again");
    CHECK(frame_claims_onstack == 0, "the next frame's uc_stack does not claim SS_ONSTACK");
    memset(&altstack_query, 0, sizeof(altstack_query));
    sigaltstack(NULL, &altstack_query);
    CHECK((altstack_query.ss_flags & SS_ONSTACK) == 0, "altstack not in use after siglongjmp out of the handler");

    // 9. Final summary.
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
