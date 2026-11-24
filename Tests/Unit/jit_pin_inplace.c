/* A pinned register that a self-loop updates (`add x0, x0, #1`) is computed in its pin, not in a scratch copy that is moved
 * back. The fused shape is LOAD_REG; OP; [ZEXT 32]; STORE_REG, and what could go wrong is around it:
 *  A. The result: every ALU form, 32- and 64-bit, immediate and register second operand, in x0 (vreg 0 is also the IR's "no
 *     operand") and in other registers.
 *  B. The old value read earlier in the same iteration, and the new value read after the update.
 *  C. A dead second operand (`mul` then `add`, the madd shape) must not pull the result out of the pin.
 *  D. Precise state: a load that faults after the update sees the new value, one that faults before it the old value. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* A1: counter in x0 with a later reader of the new value, and a 32-bit accumulator reading it. */
static void counter_x0(uint64_t out[2], uint32_t n) {
    uint64_t x0, x3;
    __asm__ volatile(
        "mov x0, #0\n mov x3, #0\n mov w1, %w2\n"
        "1: add w0, w0, #1\n add w3, w3, w0\n cmp w0, w1\n b.lo 1b\n"
        "mov %0, x0\n mov %1, x3\n"
        : "=r"(x0), "=r"(x3) : "r"(n) : "x0", "x1", "x3", "cc");
    out[0] = x0; out[1] = x3;
}
/* A2: 64-bit forms with an immediate and a register second operand. */
static void forms64(uint64_t out[4], uint64_t n) {
    uint64_t a, b, c, d;
    __asm__ volatile(
        "mov x4, #1\n mov x5, #7\n mov x6, #0x1234\n mov x7, #3\n mov x1, %4\n"
        "1: add x4, x4, #5\n sub x5, x5, x7\n eor x6, x6, x4\n orr x7, x7, #1\n sub x1, x1, #1\n cbnz x1, 1b\n"
        "mov %0, x4\n mov %1, x5\n mov %2, x6\n mov %3, x7\n"
        : "=r"(a), "=r"(b), "=r"(c), "=r"(d) : "r"(n) : "x1", "x4", "x5", "x6", "x7", "cc");
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}
/* A3: 32-bit forms (the ZEXT 32 chain) of add, sub, and, orr, eor and mul with an immediate. */
static void forms32(uint64_t out[4], uint32_t n) {
    uint64_t a, b, c, d;
    __asm__ volatile(
        "mov x4, #0xfffffff0\n mov x5, #7\n mov x6, #0x1234\n mov x7, #3\n mov w1, %w4\n"
        "1: add w4, w4, #5\n sub w5, w5, #3\n eor w6, w6, w4\n mul w7, w7, w4\n subs w1, w1, #1\n b.ne 1b\n"
        "mov %0, x4\n mov %1, x5\n mov %2, x6\n mov %3, x7\n"
        : "=r"(a), "=r"(b), "=r"(c), "=r"(d) : "r"(n) : "x1", "x4", "x5", "x6", "x7", "cc");
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}
/* B: the old value is read before the update (x6 = x4 + x5 first), and the new value after it (the compare). */
static void old_and_new(uint64_t out[3], uint32_t n) {
    uint64_t a, b, c;
    __asm__ volatile(
        "mov x4, #0\n mov x5, #3\n mov x6, #0\n mov w1, %w3\n"
        "1: add w6, w4, w5\n add w4, w4, #1\n add w5, w5, w6\n cmp w1, w4\n b.hi 1b\n"
        "mov %0, x4\n mov %1, x5\n mov %2, x6\n"
        : "=r"(a), "=r"(b), "=r"(c) : "r"(n) : "x1", "x4", "x5", "x6", "cc");
    out[0] = a; out[1] = b; out[2] = c;
}
/* C: the madd shape: the product is a dead second operand of the update. */
static void madd_shape(uint64_t out[2], uint32_t n) {
    uint64_t a, b;
    __asm__ volatile(
        "mov x3, #5\n mov x8, #3\n mov x9, #0\n mov w1, %w2\n"
        "1: add x9, x9, #1\n madd x3, x9, x8, x3\n cmp w1, w9\n b.hi 1b\n"
        "mov %0, x3\n mov %1, x9\n"
        : "=r"(a), "=r"(b) : "r"(n) : "x1", "x3", "x8", "x9", "cc");
    out[0] = a; out[1] = b;
}

static sigjmp_buf env;
static volatile uint64_t fault_x0, fault_x3, fault_addr;
static void on_segv(int sig, siginfo_t* info, void* context) {
    (void)sig;
    const ucontext_t* uc = context;
    fault_x0 = uc->uc_mcontext.regs[0];
    fault_x3 = uc->uc_mcontext.regs[3];
    fault_addr = (uint64_t)info->si_addr;
    siglongjmp(env, 1);
}
/* D: x3 counts iterations; the update comes before or after the load that faults at the guard page. */
__attribute__((noinline)) static void walk_update_first(uint64_t p) {
    __asm__ volatile(
        "mov x0, %0\n mov x3, #0\n mov x1, #-1\n"
        "1: add x3, x3, #1\n ldr x2, [x0]\n add x0, x0, #8\n sub x1, x1, #1\n cbnz x1, 1b\n"
        : : "r"(p) : "x0", "x1", "x2", "x3", "memory", "cc");
}
__attribute__((noinline)) static void walk_load_first(uint64_t p) {
    __asm__ volatile(
        "mov x0, %0\n mov x3, #0\n mov x1, #-1\n"
        "1: ldr x2, [x0]\n add x3, x3, #1\n add x0, x0, #8\n sub x1, x1, #1\n cbnz x1, 1b\n"
        : : "r"(p) : "x0", "x1", "x2", "x3", "memory", "cc");
}

int main(void) {
    for (uint32_t n = 3; n < 3000; n = n * 3 + 1) {
        uint64_t got[4], w0 = 0, w3 = 0;
        for (uint32_t c = 1; c <= n; ++c) { w0 = c; w3 = (uint32_t)(w3 + c); }
        counter_x0(got, n);
        CHECK(got[0] == w0 && got[1] == w3, "counter_x0 n=%u: x0=%llu x3=%llu, expected %llu %llu", n,
              (unsigned long long)got[0], (unsigned long long)got[1], (unsigned long long)w0, (unsigned long long)w3);

        uint64_t a = 1, b = 7, c64 = 0x1234, d = 3;
        for (uint64_t i = 0; i < n; ++i) { a += 5; b -= d; c64 ^= a; d |= 1; }
        forms64(got, n);
        CHECK(got[0] == a && got[1] == b && got[2] == c64 && got[3] == d, "forms64 n=%u: %llx %llx %llx %llx, expected %llx %llx %llx %llx",
              n, (unsigned long long)got[0], (unsigned long long)got[1], (unsigned long long)got[2], (unsigned long long)got[3],
              (unsigned long long)a, (unsigned long long)b, (unsigned long long)c64, (unsigned long long)d);

        uint32_t a32 = 0xfffffff0u, b32 = 7, c32 = 0x1234, d32 = 3;
        for (uint32_t i = 0; i < n; ++i) { a32 += 5; b32 -= 3; c32 ^= a32; d32 *= a32; }
        forms32(got, n);
        CHECK(got[0] == a32 && got[1] == b32 && got[2] == c32 && got[3] == d32,
              "forms32 n=%u: %llx %llx %llx %llx, expected %x %x %x %x", n, (unsigned long long)got[0],
              (unsigned long long)got[1], (unsigned long long)got[2], (unsigned long long)got[3], a32, b32, c32, d32);

        uint32_t x4 = 0, x5 = 3, x6 = 0;
        do { x6 = x4 + x5; x4 += 1; x5 += x6; } while (n > x4);
        old_and_new(got, n);
        CHECK(got[0] == x4 && got[1] == x5 && got[2] == x6, "old_and_new n=%u: %llu %llu %llu, expected %u %u %u", n,
              (unsigned long long)got[0], (unsigned long long)got[1], (unsigned long long)got[2], x4, x5, x6);

        uint64_t s3 = 5, s9 = 0;
        do { s9 += 1; s3 += s9 * 3; } while (n > (uint32_t)s9);
        madd_shape(got, n);
        CHECK(got[0] == s3 && got[1] == s9, "madd_shape n=%u: %llu %llu, expected %llu %llu", n, (unsigned long long)got[0],
              (unsigned long long)got[1], (unsigned long long)s3, (unsigned long long)s9);
    }

    const long page = sysconf(_SC_PAGESIZE);
    uint8_t* map = mmap(0, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED || mprotect(map + page, page, PROT_NONE) != 0) {
        printf("jit_pin_inplace: mmap/mprotect failed\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    const uint64_t guard = (uint64_t)(map + page);
    for (int r = 0; r < 8; ++r) {
        const int update_first = r & 1;
        const uint64_t k = 40 + 11 * r;                    // iterations that succeed
        const uint64_t start = guard - 8 * k;
        if (sigsetjmp(env, 1) == 0) {
            if (update_first) walk_update_first(start); else walk_load_first(start);
            ++failures; printf("FAIL D %s: no fault\n", update_first ? "update first" : "load first");
            continue;
        }
        // The load of iteration k + 1 faults. x3 counts the updates that completed: the one of that iteration too when it
        // comes first. x0 is the address, updated k times (by the 8-byte adds before it).
        const uint64_t want_x3 = update_first ? k + 1 : k;
        CHECK(fault_addr == guard && fault_x0 == guard && fault_x3 == want_x3,
              "D %s k=%llu: fault %+lld, x0 %+lld, x3=%llu (expected +0, +0, %llu)", update_first ? "update first" : "load first",
              (unsigned long long)k, (long long)(fault_addr - guard), (long long)(fault_x0 - guard),
              (unsigned long long)fault_x3, (unsigned long long)want_x3);
    }
    if (failures) {
        printf("jit_pin_inplace: %d failures\n", failures);
        return 1;
    }
    printf("jit_pin_inplace: ALL PASS\n");
    return 0;
}
