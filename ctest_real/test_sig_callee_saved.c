// test_sig_callee_saved.c — Verify callee-saved registers are preserved across signal.
//
// What it verifies:
//   1. The signal handler runs without corrupting x19-x28 (callee-saved).
//   2. After rt_sigreturn, the values of x19-x28 match what they were
//      before the signal.
//   3. The handler can use x19-x28 internally without affecting the
//      caller's view (because the kernel saves/restores them).
//
// Note: this is a runtime check — it cannot catch JIT vs interpreter
// divergences directly, but it does catch signal-frame save/restore bugs.
//
// Build: make cross SRC=ctest_real/test_sig_callee_saved.c OUT=ctest_real/test_sig_callee_saved.elf
// Run:   ./bifrost-emu ctest_real/test_sig_callee_saved.elf
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

// Clobber callee-saved registers in the handler. The kernel must save
// and restore them across the signal.
static void handler(int sig) {
    (void)sig;
    // Clobber x19-x28 with garbage. If the kernel doesn't save/restore
    // these, the test will fail.
    register unsigned long x19 __asm__("x19");
    register unsigned long x20 __asm__("x20");
    register unsigned long x21 __asm__("x21");
    register unsigned long x22 __asm__("x22");
    register unsigned long x23 __asm__("x23");
    register unsigned long x24 __asm__("x24");
    register unsigned long x25 __asm__("x25");
    register unsigned long x26 __asm__("x26");
    register unsigned long x27 __asm__("x27");
    register unsigned long x28 __asm__("x28");

    x19 = 0xDEADBEEF; x20 = 0xCAFEBABE; x21 = 0xFEEDFACE;
    x22 = 0xBAADF00D; x23 = 0xDECAFBAD; x24 = 0x0BADF00D;
    x25 = 0x1BADB002; x26 = 0x2BADB003; x27 = 0x3BADB004;
    x28 = 0x4BADB005;

    __asm__ volatile("" ::
        "r"(x19), "r"(x20), "r"(x21), "r"(x22), "r"(x23),
        "r"(x24), "r"(x25), "r"(x26), "r"(x27), "r"(x28));
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    // Load distinctive values into x19-x28.
    register unsigned long x19 __asm__("x19") = 0x1111111111111111UL;
    register unsigned long x20 __asm__("x20") = 0x2222222222222222UL;
    register unsigned long x21 __asm__("x21") = 0x3333333333333333UL;
    register unsigned long x22 __asm__("x22") = 0x4444444444444444UL;
    register unsigned long x23 __asm__("x23") = 0x5555555555555555UL;
    register unsigned long x24 __asm__("x24") = 0x6666666666666666UL;
    register unsigned long x25 __asm__("x25") = 0x7777777777777777UL;
    register unsigned long x26 __asm__("x26") = 0x8888888888888888UL;
    register unsigned long x27 __asm__("x27") = 0x9999999999999999UL;
    register unsigned long x28 __asm__("x28") = 0xAAAAAAAAAAAAAAAAUL;

    // Force them onto the stack / into the CPU state — without this, the
    // compiler may not actually keep them in registers across the call.
    __asm__ volatile("" ::
        "r"(x19), "r"(x20), "r"(x21), "r"(x22), "r"(x23),
        "r"(x24), "r"(x25), "r"(x26), "r"(x27), "r"(x28));

    raise(SIGUSR1);

    // Re-read the registers (compiler will reload them).
    __asm__ volatile("" :
        "=r"(x19), "=r"(x20), "=r"(x21), "=r"(x22), "=r"(x23),
        "=r"(x24), "=r"(x25), "=r"(x26), "=r"(x27), "=r"(x28));

    char msg[256];
    int len = snprintf(msg, sizeof(msg),
        "x19=0x%lx x20=0x%lx x21=0x%lx x22=0x%lx x23=0x%lx\n"
        "x24=0x%lx x25=0x%lx x26=0x%lx x27=0x%lx x28=0x%lx\n",
        x19, x20, x21, x22, x23,
        x24, x25, x26, x27, x28);
    write(2, msg, len);

    CHECK(x19 == 0x1111111111111111UL, "x19 preserved");
    CHECK(x20 == 0x2222222222222222UL, "x20 preserved");
    CHECK(x21 == 0x3333333333333333UL, "x21 preserved");
    CHECK(x22 == 0x4444444444444444UL, "x22 preserved");
    CHECK(x23 == 0x5555555555555555UL, "x23 preserved");
    CHECK(x24 == 0x6666666666666666UL, "x24 preserved");
    CHECK(x25 == 0x7777777777777777UL, "x25 preserved");
    CHECK(x26 == 0x8888888888888888UL, "x26 preserved");
    CHECK(x27 == 0x9999999999999999UL, "x27 preserved");
    CHECK(x28 == 0xAAAAAAAAAAAAAAAAUL, "x28 preserved");

    len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
