/*
 * jit_simd_reserved_forms.c — reserved ASIMD element forms trap; valid ones run.
 *
 * DUP (element) and INS (general) with imm5 = x0000 divided by a zero element
 * size in the interpreter and killed the emulator with a host SIGFPE (both
 * engines; found by Scripts/Testing/simd_coverage.py). Each reserved form
 * below must raise SIGILL, as under QEMU. DUP (general) ignores imm5 bits
 * above the lowest set bit, and vector FCVTZS/FCVTZU saturate infinities
 * instead of returning 0.
 *
 * Forms of features Bifrost does not advertise must trap too (found by
 * Scripts/Testing/sequence_fuzz.py): scalar fixed-point converts with an
 * FP16 element (immh = 001x) ran as single precision with a wrong scale, and
 * FP8 BF1CVTL/F2CVTL ran as SQNEG. They are checked only when AT_HWCAP /
 * AT_HWCAP2 lack the feature, so the binary also passes under QEMU's max CPU.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

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
    TRAPS(0x5f08fc02);  /* scalar FCVTZS #fbits, immh = 0001 (reserved) */
    TRAPS(0x5f08e402);  /* scalar SCVTF #fbits, immh = 0001 (reserved) */
    if (!(getauxval(AT_HWCAP) & (1ul << 10))) {  /* HWCAP_ASIMDHP */
        TRAPS(0x5f1dfc02);  /* fcvtzs h2, h0, #3 */
        TRAPS(0x7f1dfc02);  /* fcvtzu h2, h0, #3 */
        TRAPS(0x5f10e402);  /* scvtf h2, h0, #16 */
    }
    if (!(getauxval(AT_HWCAP2) & (1ul << 51))) {  /* HWCAP2_F8CVT */
        TRAPS(0x2ea17802);  /* bf1cvtl v2.8h, v0.8b */
        TRAPS(0x6e617802);  /* f2cvtl2 v2.8h, v0.16b */
    }

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
