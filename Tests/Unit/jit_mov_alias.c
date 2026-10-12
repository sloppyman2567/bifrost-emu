/*
 * jit_mov_alias.c — register moves written as logical ops with XZR/WZR.
 *
 * MOV (register) is ORR Xd, XZR, Xm: the zero constant is the FIRST
 * operand. The optimizer folds commutative identities with the constant in
 * either position. W forms must still clear bits 63:32; shifted and
 * inverted sources must not be dropped by the fold; AND with XZR is zero.
 */
#include <stdint.h>
#include <stdio.h>

static int checks, fails;
static void expect(const char* name, uint64_t got, uint64_t want) {
    ++checks;
    if (got != want) {
        printf("FAIL %s got=%016llx want=%016llx\n", name,
               (unsigned long long)got, (unsigned long long)want);
        ++fails;
    }
}
#define ONE(name, ins, src, want) do { uint64_t d = 0x5a5a5a5a5a5a5a5aull, s = (src); \
    __asm__ volatile(ins : "+r"(d) : "r"(s)); expect(name, d, (want)); } while (0)

int main(void) {
    const uint64_t v[] = {0, 1, 0x80000000ull, 0xffffffffull, 0x123456789abcdef0ull,
                          0xfedcba9876543210ull, ~0ull};
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        const uint64_t x = v[i];
        ONE("mov x", "mov %0, %1", x, x);
        ONE("orr x xzr", "orr %0, xzr, %1", x, x);
        ONE("mov w", "mov %w0, %w1", x, x & 0xffffffffull);
        ONE("orr x lsl", "orr %0, xzr, %1, lsl #4", x, x << 4);
        ONE("orr w lsr", "orr %w0, wzr, %w1, lsr #3", x, (x & 0xffffffffull) >> 3);
        ONE("orn x", "orn %0, xzr, %1", x, ~x);
        ONE("eor x xzr", "eor %0, xzr, %1", x, x);
        ONE("eor w wzr", "eor %w0, wzr, %w1", x, x & 0xffffffffull);
        ONE("and x xzr", "and %0, xzr, %1", x, 0);
        /* Destination aliases the source. */
        uint64_t a = x;
        __asm__ volatile("mov %w0, %w0" : "+r"(a));
        expect("mov w alias", a, x & 0xffffffffull);
        a = x;
        __asm__ volatile("orr %0, xzr, %0, ror #8" : "+r"(a));
        expect("orr ror alias", a, (x >> 8) | (x << 56));
    }
    if (fails) { printf("jit_mov_alias: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_mov_alias: %d checks passed\n", checks);
    return 0;
}
