// jit_addsub_sp_operand.c — Rn=31 is SP in add/subtract (immediate) and (extended register), flag-setting forms too.
//
// Arm: ADDS <Xd>, <Xn|SP>, #imm and its aliases CMP/CMN <Xn|SP>, #imm read SP for Rn=31; only the destination of the
// flag-setting forms is XZR. The decoder, the interpreter and the JIT each read XZR there (found by
// Scripts/Testing/sequence_fuzz.py: adds x1, sp, #0x1ac gave 0x1ac). The results and NZCV are checked against Arm's
// AddWithCarry, with SP read inside the same asm statement, so the binary also runs under QEMU. Add/subtract
// immediate words with bit 23 set are other classes (ADDG/SUBG, FEAT_MTE): without HWCAP2_MTE they must raise SIGILL,
// not run as a plain ADD.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

static int checks, fails;
#define CHECK(ok, ...) do { ++checks; if (!(ok)) { ++fails; if (fails < 30) { printf("NG " __VA_ARGS__); printf("\n"); } } } while (0)

// Arm AddWithCarry for `width` bits: the result and NZCV (bits 31:28).
static uint64_t add_with_carry(uint64_t x, uint64_t y, int carry, int width, unsigned *nzcv) {
    const uint64_t mask = width == 64 ? ~0ull : 0xffffffffull;
    x &= mask; y &= mask;
    const uint64_t result = (x + y + (uint64_t)carry) & mask;
    const uint64_t sign = width == 64 ? 1ull << 63 : 1ull << 31;
    unsigned n = (result & sign) != 0, z = result == 0;
    unsigned c = width == 64 ? (result < x || (carry && result == x)) : ((x + y + (uint64_t)carry) >> 32) != 0;
    unsigned v = ((x & sign) == (y & sign)) && ((result & sign) != (x & sign));
    *nzcv = (n << 31) | (z << 30) | (c << 29) | (v << 28);
    return result;
}

#define FLAGS(n) ((unsigned)((n) & 0xf0000000u))

static sigjmp_buf trap;
static void on_sigill(int sig) { (void)sig; siglongjmp(trap, 1); }

int main(void) {
    uint64_t sp, r, nzcv;
    unsigned want;
    uint64_t expect;

    __asm__ volatile("mov %[sp], sp\n adds %[r], sp, #0x1ac\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [r] "=&r"(r), [f] "=r"(nzcv) :: "cc");
    expect = add_with_carry(sp, 0x1ac, 0, 64, &want);
    CHECK(r == expect && FLAGS(nzcv) == want, "adds x, sp, #0x1ac: %llx %x want %llx %x",
          (unsigned long long)r, FLAGS(nzcv), (unsigned long long)expect, want);

    __asm__ volatile("mov %[sp], sp\n adds %[r], sp, #0x7, lsl #12\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [r] "=&r"(r), [f] "=r"(nzcv) :: "cc");
    expect = add_with_carry(sp, 0x7000, 0, 64, &want);
    CHECK(r == expect && FLAGS(nzcv) == want, "adds x, sp, #7, lsl #12");

    __asm__ volatile("mov %[sp], sp\n subs %[r], sp, #0x10\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [r] "=&r"(r), [f] "=r"(nzcv) :: "cc");
    expect = add_with_carry(sp, ~0x10ull, 1, 64, &want);
    CHECK(r == expect && FLAGS(nzcv) == want, "subs x, sp, #0x10");

    // CMP/CMN <Xn|SP>, #imm: only the flags; a value just above SP and one far below it.
    const uint64_t probe = 0x20;
    __asm__ volatile("mov %[sp], sp\n cmp sp, #0x20\n mrs %[f], nzcv" : [sp] "=&r"(sp), [f] "=r"(nzcv) :: "cc");
    add_with_carry(sp, ~probe, 1, 64, &want);
    CHECK(FLAGS(nzcv) == want, "cmp sp, #0x20: %x want %x", FLAGS(nzcv), want);
    __asm__ volatile("mov %[sp], sp\n cmn sp, #0xfff\n mrs %[f], nzcv" : [sp] "=&r"(sp), [f] "=r"(nzcv) :: "cc");
    add_with_carry(sp, 0xfff, 0, 64, &want);
    CHECK(FLAGS(nzcv) == want, "cmn sp, #0xfff: %x want %x", FLAGS(nzcv), want);

    // 32-bit forms read WSP, the low half of SP.
    uint32_t w;
    __asm__ volatile("mov %[sp], sp\n adds %w[w], wsp, #0x30\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [w] "=&r"(w), [f] "=r"(nzcv) :: "cc");
    expect = add_with_carry(sp, 0x30, 0, 32, &want);
    CHECK(w == (uint32_t)expect && FLAGS(nzcv) == want, "adds w, wsp, #0x30");
    __asm__ volatile("mov %[sp], sp\n cmp wsp, #0x40\n mrs %[f], nzcv" : [sp] "=&r"(sp), [f] "=r"(nzcv) :: "cc");
    add_with_carry(sp, ~0x40ull, 1, 32, &want);
    CHECK(FLAGS(nzcv) == want, "cmp wsp, #0x40");

    // Extended register (already SP before the fix): controls for the same rule.
    uint64_t operand = 0x123;
    __asm__ volatile("mov %[sp], sp\n subs %[r], sp, %[x], uxtx\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [r] "=&r"(r), [f] "=r"(nzcv) : [x] "r"(operand) : "cc");
    expect = add_with_carry(sp, ~operand, 1, 64, &want);
    CHECK(r == expect && FLAGS(nzcv) == want, "subs x, sp, x, uxtx");
    __asm__ volatile("mov %[sp], sp\n cmp sp, %[x], uxtx\n mrs %[f], nzcv"
                     : [sp] "=&r"(sp), [f] "=r"(nzcv) : [x] "r"(operand) : "cc");
    add_with_carry(sp, ~operand, 1, 64, &want);
    CHECK(FLAGS(nzcv) == want, "cmp sp, x, uxtx");

    // ADDG x7, x4, #0x40, #6 without FEAT_MTE: UNDEFINED.
    if (!(getauxval(AT_HWCAP2) & (1ul << 18))) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_sigill;
        sigaction(SIGILL, &sa, 0);
        volatile int raised = 0;
        if (sigsetjmp(trap, 1)) raised = 1;
        else __asm__ volatile(".inst 0x91841887" ::: "x7");
        CHECK(raised, "addg without FEAT_MTE ran instead of raising SIGILL");
    }

    if (fails) { printf("jit_addsub_sp_operand: FAILED (%d of %d)\n", fails, checks); return 1; }
    printf("jit_addsub_sp_operand: ALL PASS (%d checks)\n", checks);
    return 0;
}
