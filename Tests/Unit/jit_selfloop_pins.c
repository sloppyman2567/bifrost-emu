/* Loop-carried pins of a self-loop block. Its back-edge jumps past the
 * prologue's pin loads, so a pin register can be ahead of cpu.regs[] at the
 * body start. Two ways that went wrong, each in a CBZ form and a `subs; b.ne`
 * form:
 *  A. An op that reads its GPR through the allocator (scvtf) as the block's
 *     first op missed the pin because the preload did not raise max_vreg_,
 *     and read the value cpu.regs[] had when the loop was entered.
 *  B. A fault in an iteration before the pin is rewritten must report the
 *     current pointer: the body start must treat the pins as dirty, or the
 *     fault exit skips their store.
 * The jit_fp_ldst_cache entity loop (scvtf of its counter, then b.ne) found A
 * once BRCOND stopped storing the pins every iteration. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static double sum_cbnz(unsigned n) {
    double s;
    register unsigned c __asm__("w5") = n;  // not x0: max_vreg_ starts at 0
    __asm__ volatile(
        "movi d0, #0\n"
        "1: scvtf d1, %w1\n fadd d0, d0, d1\n sub %w1, %w1, #1\n cbnz %w1, 1b\n"
        "fmov %d0, d0\n"
        : "=w"(s), "+r"(c) : : "v0", "v1");
    return s;
}
static double sum_bne(unsigned n) {
    double s;
    register unsigned c __asm__("w5") = n;  // not x0: max_vreg_ starts at 0
    __asm__ volatile(
        "movi d0, #0\n"
        "1: scvtf d1, %w1\n fadd d0, d0, d1\n subs %w1, %w1, #1\n b.ne 1b\n"
        "fmov %d0, d0\n"
        : "=w"(s), "+r"(c) : : "v0", "v1", "cc");
    return s;
}

static sigjmp_buf env;
static volatile uint64_t fault_x0, fault_addr;
static void on_segv(int sig, siginfo_t* info, void* context) {
    (void)sig;
    fault_x0 = ((ucontext_t*)context)->uc_mcontext.regs[0];
    fault_addr = (uint64_t)info->si_addr;
    siglongjmp(env, 1);
}
// Walk x0 forward eight bytes per iteration until it reaches the guard page.
__attribute__((noinline)) static void walk_cbnz(uint64_t p) {
    __asm__ volatile(
        "mov x0, %0\n mov x1, #-1\n"
        "1: ldr x2, [x0]\n add x0, x0, #8\n sub x1, x1, #1\n cbnz x1, 1b\n"
        : : "r"(p) : "x0", "x1", "x2", "memory");
}
__attribute__((noinline)) static void walk_bne(uint64_t p) {
    __asm__ volatile(
        "mov x0, %0\n mov x1, #-1\n"
        "1: ldr x2, [x0]\n add x0, x0, #8\n subs x1, x1, #1\n b.ne 1b\n"
        : : "r"(p) : "x0", "x1", "x2", "memory", "cc");
}

int main(void) {
    int failures = 0;
    for (int r = 0; r < 4; ++r) {
        const unsigned n = 1000 + 37 * r;
        const double want = (double)n * (n + 1) / 2;
        const double a = sum_cbnz(n), b = sum_bne(n);
        if (a != want) { ++failures; printf("FAIL A cbnz n=%u: %.1f, expected %.1f\n", n, a, want); }
        if (b != want) { ++failures; printf("FAIL A b.ne n=%u: %.1f, expected %.1f\n", n, b, want); }
    }

    const long page = sysconf(_SC_PAGESIZE);
    uint8_t* map = mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED || mprotect(map + page, page, PROT_NONE) != 0) {
        printf("jit_selfloop_pins: mmap/mprotect failed\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    const uint64_t guard = (uint64_t)(map + page);
    for (int r = 0; r < 8; ++r) {
        const int bne = r & 1;
        const uint64_t start = guard - 8 * (64 + 16 * r);
        if (sigsetjmp(env, 1) == 0) {
            if (bne) walk_bne(start); else walk_cbnz(start);
            ++failures; printf("FAIL B %s: no fault\n", bne ? "b.ne" : "cbnz");
            continue;
        }
        if (fault_addr != guard || fault_x0 != guard) {
            ++failures;
            printf("FAIL B %s start=-%llu: fault at %+lld, x0 at %+lld (both should be +0)\n", bne ? "b.ne" : "cbnz",
                   (unsigned long long)(guard - start), (long long)(fault_addr - guard), (long long)(fault_x0 - guard));
        }
    }
    if (failures) {
        printf("jit_selfloop_pins: %d failures\n", failures);
        return 1;
    }
    printf("jit_selfloop_pins: ALL PASS\n");
    return 0;
}
