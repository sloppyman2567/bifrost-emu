/*
 * jit_simd_reserved_forms.c — reserved ASIMD element forms trap; valid ones run.
 *
 * DUP (element) and INS (general) with imm5 = x0000 divided by a zero element
 * size in the interpreter and killed the emulator with a host SIGFPE (both
 * engines; found by Scripts/Testing/simd_coverage.py). Each reserved form
 * below must raise SIGILL, as under QEMU. DUP (general) ignores imm5 bits
 * above the lowest set bit, and vector FCVTZS/FCVTZU saturate infinities
 * instead of returning 0.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf trap;
static void on_sigill(int sig) { (void)sig; siglongjmp(trap, 1); }
static int fails, checks;

#define TRAPS(word) do { \
        ++checks; \
        if (!sigsetjmp(trap, 1)) { \
            __asm__ volatile(".inst " #word ::: "v2", "x2", "memory"); \
            printf("FAIL %s executed instead of SIGILL\n", #word); ++fails; \
        } \
    } while (0)

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, 0);

    TRAPS(0x0e000402);  /* DUP v2, v0 element, imm5 = 0 */
    TRAPS(0x4e100402);  /* DUP element, imm5 = 16 */
    TRAPS(0x0e080402);  /* DUP v2.1d, v0.d[0] (Q = 0) */
    TRAPS(0x0e000c02);  /* DUP v2, w0, imm5 = 0 */
    TRAPS(0x0e080c02);  /* DUP v2.1d, x0 (Q = 0) */
    TRAPS(0x4e001c02);  /* INS v2, x0, imm5 = 0 */
    TRAPS(0x0e041c02);  /* INS general with Q = 0 */
    TRAPS(0x0ee1b802);  /* FCVTZS v2.1d */

    /* DUP v2.16b, w0 with imm5 = 0b00011: byte size, bit 1 ignored. */
    uint8_t out[16];
    uint64_t x0 = 0x1234567890abcdefull;
    ++checks;
    __asm__ volatile("mov x0, %1\n .inst 0x4e030c02\n str q2, [%0]"
                     :: "r"(out), "r"(x0) : "x0", "v2", "memory");
    for (int i = 0; i < 16; ++i)
        if (out[i] != 0xef) { printf("FAIL DUP imm5=3 lane %d = %02x\n", i, out[i]); ++fails; break; }

    /* FCVTZS/FCVTZU v.2d: +inf saturates, -inf saturates, NaN is 0. */
    static const uint64_t in[4] = {0x7ff0000000000000ull, 0xfff0000000000000ull,
                                   0x7ff8000000000000ull, 0xc000000000000000ull};
    int64_t s[4]; uint64_t u[4];
    __asm__ volatile("ldr q0, [%2]\n fcvtzs v1.2d, v0.2d\n str q1, [%0]\n"
                     "fcvtzu v1.2d, v0.2d\n str q1, [%1]\n"
                     "ldr q0, [%3]\n fcvtzs v1.2d, v0.2d\n str q1, [%0, 16]\n"
                     "fcvtzu v1.2d, v0.2d\n str q1, [%1, 16]"
                     :: "r"(s), "r"(u), "r"(in), "r"(in + 2) : "v0", "v1", "memory");
    const int64_t want_s[4] = {INT64_MAX, INT64_MIN, 0, -2};
    const uint64_t want_u[4] = {UINT64_MAX, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        ++checks;
        if (s[i] != want_s[i] || u[i] != want_u[i]) {
            printf("FAIL fcvtz lane %d s=%lld u=%llu\n", i, (long long)s[i], (unsigned long long)u[i]);
            ++fails;
        }
    }
    /* .4s forms take the JIT's native SIMD_CVTF path. */
    static const uint32_t in4[4] = {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x4f9502f9u};
    uint32_t s4[4], u4[4];
    __asm__ volatile("ldr q0, [%2]\n fcvtzs v1.4s, v0.4s\n str q1, [%0]\n"
                     "fcvtzu v1.4s, v0.4s\n str q1, [%1]"
                     :: "r"(s4), "r"(u4), "r"(in4) : "v0", "v1", "memory");
    const uint32_t want_s4[4] = {0x7fffffffu, 0x80000000u, 0, 0x7fffffffu};
    const uint32_t want_u4[4] = {0xffffffffu, 0, 0, 0xffffffffu};
    for (int i = 0; i < 4; ++i) {
        ++checks;
        if (s4[i] != want_s4[i] || u4[i] != want_u4[i]) {
            printf("FAIL fcvtz .4s lane %d s=%08x u=%08x\n", i, s4[i], u4[i]);
            ++fails;
        }
    }
    if (fails) { printf("jit_simd_reserved_forms: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_reserved_forms: %d checks passed\n", checks);
    return 0;
}
