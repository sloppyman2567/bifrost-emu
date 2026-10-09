// jit_simd_fixconv.c — vector fixed-point conversions
// SCVTF/UCVTF/FCVTZS/FCVTZU (vector, fixed-point): 0 Q U 011110 immh:immb
// 11100 1 / 11111 1, fbits = 2*esize - immh:immb (QEMU a64.decode
// *_vf). Teeworlds' map editor runs `fcvtzs v31.2s, v30.2s, #10` in
// CEditor::DoQuadPoint; it used to raise SIGILL.
//
// Expected values follow the ARM ARM (FCVTZ* truncates and saturates,
// NaN -> 0) and were checked against qemu-aarch64.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check_u64(const char* what, const uint64_t* got,
                      const uint64_t* want) {
    checks++;
    if (got[0] != want[0] || got[1] != want[1]) {
        printf("FAIL %s: got %016llx:%016llx want %016llx:%016llx\n", what,
               (unsigned long long)got[1], (unsigned long long)got[0],
               (unsigned long long)want[1], (unsigned long long)want[0]);
        failures++;
    }
}

// Pack lanes so `want` matches the register layout (v_lo = [0], v_hi = [1]).
static void pack32(uint64_t* out, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    out[0] = a | (uint64_t)b << 32;
    out[1] = c | (uint64_t)d << 32;
}

static uint32_t fbits32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t fbits64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// Run `insn v1, v0, #imm` with v1 pre-filled with 0xAA bytes so Q=0
// upper-half zeroing is observable.
#define RUN(insn, in, out)                                              \
    __asm__ volatile("ldr q0, [%0]\n\t"                                 \
                     "movi v1.16b, #0xaa\n\t"                           \
                     insn "\n\t"                                        \
                     "str q1, [%1]\n\t"                                 \
                     :: "r"(in), "r"(out) : "v0", "v1", "memory")

int main(void) {
    uint64_t in[2], out[2], want[2];
    float nanf_ = __builtin_nanf("");

    // Teeworlds' exact form: float x 2^10, truncated, Q=0.
    pack32(in, fbits32(1.5f), fbits32(-2.75f), 0, 0);
    RUN("fcvtzs v1.2s, v0.2s, #10", in, out);
    pack32(want, 1536, (uint32_t)-2816, 0, 0);
    check_u64("fcvtzs .2s #10", out, want);

    // Truncation toward zero, saturation and NaN, Q=1.
    pack32(in, fbits32(0.99f), fbits32(-0.99f), fbits32(1e10f), fbits32(nanf_));
    RUN("fcvtzs v1.4s, v0.4s, #1", in, out);
    pack32(want, 1, (uint32_t)-1, 0x7fffffff, 0);
    check_u64("fcvtzs .4s #1 trunc/sat/nan", out, want);

    pack32(in, fbits32(-1.0f), fbits32(3.25f), fbits32(1e10f), fbits32(4294967295.0f));
    RUN("fcvtzu v1.4s, v0.4s, #2", in, out);
    pack32(want, 0, 13, 0xffffffff, 0xffffffff);
    check_u64("fcvtzu .4s #2 neg/sat", out, want);

    pack32(in, fbits32(-1e10f), fbits32(0.5f), 0, 0);
    RUN("fcvtzs v1.2s, v0.2s, #32", in, out);
    pack32(want, 0x80000000u, 0x7fffffffu, 0, 0);
    check_u64("fcvtzs .2s #32 sat", out, want);

    in[0] = fbits64(-3.5); in[1] = fbits64(1e30);
    RUN("fcvtzs v1.2d, v0.2d, #4", in, out);
    want[0] = (uint64_t)-56; want[1] = 0x7fffffffffffffffull;
    check_u64("fcvtzs .2d #4", out, want);

    in[0] = fbits64(0.75); in[1] = fbits64(1.0);
    RUN("fcvtzu v1.2d, v0.2d, #64", in, out);
    want[0] = 0xc000000000000000ull; want[1] = 0xffffffffffffffffull;
    check_u64("fcvtzu .2d #64", out, want);

    // int -> FP divides by 2^fbits.
    pack32(in, 1536, (uint32_t)-2816, 7, 0x80000000u);
    RUN("scvtf v1.4s, v0.4s, #10", in, out);
    pack32(want, fbits32(1.5f), fbits32(-2.75f), fbits32(7.0f / 1024),
           fbits32(-2097152.0f));
    check_u64("scvtf .4s #10", out, want);

    pack32(in, 0x80000000u, 3, 0, 0);
    RUN("ucvtf v1.2s, v0.2s, #1", in, out);
    pack32(want, fbits32(1073741824.0f), fbits32(1.5f), 0, 0);
    check_u64("ucvtf .2s #1", out, want);

    in[0] = (uint64_t)-3; in[1] = 0xffffffffffffffffull;
    RUN("scvtf v1.2d, v0.2d, #1", in, out);
    want[0] = fbits64(-1.5); want[1] = fbits64(-0.5);
    check_u64("scvtf .2d #1", out, want);

    RUN("ucvtf v1.2d, v0.2d, #64", in, out);
    want[0] = fbits64(1.0); want[1] = fbits64(1.0);
    check_u64("ucvtf .2d #64 (rounds)", out, want);

    // Source == destination.
    pack32(in, fbits32(2.5f), fbits32(-0.25f), 0, 0);
    __asm__ volatile("ldr q0, [%0]\n\t"
                     "fcvtzs v0.2s, v0.2s, #3\n\t"
                     "str q0, [%1]\n\t"
                     :: "r"(in), "r"(out) : "v0", "memory");
    pack32(want, 20, (uint32_t)-2, 0, 0);
    check_u64("fcvtzs .2s #3 in place", out, want);

    // Android Skia's icon rasterizer: nearest-even conversion in place.
    pack32(in, fbits32(0.5f), fbits32(1.5f), fbits32(2.5f), fbits32(-0.5f));
    RUN("fcvtnu v1.4s, v0.4s", in, out);
    pack32(want, 0, 2, 2, 0);
    check_u64("fcvtnu .4s ties-even", out, want);
    RUN("mov v1.16b, v0.16b\n\tfcvtnu v1.4s, v1.4s", in, out);
    check_u64("fcvtnu .4s alias", out, want);
    RUN("fcvtnu v1.2s, v0.2s", in, out);
    pack32(want, 0, 2, 0, 0);
    check_u64("fcvtnu .2s clears high", out, want);
    pack32(in, fbits32(-1.0f), fbits32(1e30f), fbits32(nanf_), fbits32(3.25f));
    RUN("fcvtnu v1.4s, v0.4s", in, out);
    pack32(want, 0, 0xffffffff, 0, 3);
    check_u64("fcvtnu .4s saturation nan", out, want);
    in[0] = fbits64(2.5); in[1] = fbits64(3.5);
    RUN("fcvtnu v1.2d, v0.2d", in, out);
    want[0] = 2; want[1] = 4;
    check_u64("fcvtnu .2d", out, want);
    in[0] = fbits64(9223372036854775808.0); in[1] = fbits64(18446744073709551616.0);
    RUN("fcvtnu v1.2d, v0.2d", in, out);
    want[0] = 0x8000000000000000ull; want[1] = ~0ull;
    check_u64("fcvtnu .2d upper unsigned", out, want);

    uint64_t saved_fpcr, saved_fpsr, status;
    __asm__ volatile("mrs %0,fpcr; mrs %1,fpsr" : "=r"(saved_fpcr), "=r"(saved_fpsr));
    pack32(in, fbits32(0.5f), fbits32(1.5f), fbits32(2.5f), fbits32(-0.5f));
    pack32(want, 0, 2, 2, 0);
    for (unsigned mode = 0; mode < 4; ++mode) {
        uint64_t fpcr = (saved_fpcr & ~(3ull << 22)) | ((uint64_t)mode << 22);
        __asm__ volatile("msr fpcr,%0; msr fpsr,xzr" :: "r"(fpcr) : "memory");
        RUN("fcvtnu v1.4s, v0.4s", in, out);
        __asm__ volatile("mrs %0,fpsr" : "=r"(status));
        check_u64("fcvtnu independent of FPCR rounding", out, want);
        checks++; if (status != 16) { printf("FAIL fcvtnu inexact FPSR=%llx\n", (unsigned long long)status); failures++; }
    }
    pack32(in, fbits32(-1.f), fbits32(nanf_), fbits32(1e30f), fbits32(3.25f));
    __asm__ volatile("msr fpsr,xzr" ::: "memory");
    RUN("fcvtnu v1.4s, v0.4s", in, out);
    __asm__ volatile("mrs %0,fpsr; msr fpcr,%1; msr fpsr,%2" : "=r"(status) : "r"(saved_fpcr), "r"(saved_fpsr) : "memory");
    checks++; if (status != 17) { printf("FAIL fcvtnu invalid/inexact FPSR=%llx\n", (unsigned long long)status); failures++; }

    pack32(in, fbits32(0.25f), fbits32(-0.25f), fbits32(1.5f), fbits32(-1.5f));
    RUN("frintp v1.4s, v0.4s", in, out);
    pack32(want, fbits32(1.f), fbits32(-0.f), fbits32(2.f), fbits32(-1.f));
    check_u64("frintp .4s", out, want);
    RUN("frintp v1.2s, v0.2s", in, out);
    want[1] = 0;
    check_u64("frintp .2s", out, want);
    in[0] = fbits64(1.25); in[1] = fbits64(-1.25);
    RUN("mov v1.16b, v0.16b\n\tfrintp v1.2d, v1.2d", in, out);
    want[0] = fbits64(2.); want[1] = fbits64(-1.);
    check_u64("frintp .2d alias", out, want);

    // Scalar integer comparisons must clear the upper 64 bits and preserve
    // both inputs when Rd aliases Rn or Rm. Signed and unsigned differ here.
#define COMPARE(insn, a, b, expected) do {                              \
        in[0] = (a); in[1] = (b);                                       \
        __asm__ volatile("ldr d0,[%0]\n ldr d2,[%0,#8]\n"               \
                         "movi v1.16b,#0xaa\n" insn "\n str q1,[%1]"  \
                         :: "r"(in), "r"(out) : "v0","v1","v2","memory"); \
        want[0] = (expected) ? ~0ull : 0; want[1] = 0;                   \
        check_u64(insn, out, want);                                     \
    } while (0)
    COMPARE("cmgt d1,d0,d2", ~0ull, 1, 0);
    COMPARE("cmge d1,d0,d2", ~0ull, ~0ull, 1);
    COMPARE("cmhi d1,d0,d2", ~0ull, 1, 1);
    COMPARE("cmhs d1,d0,d2", 1, ~0ull, 0);
    COMPARE("mov v1.16b,v0.16b\n cmgt d1,d1,d2", 2, 1, 1);
    COMPARE("mov v1.16b,v2.16b\n cmgt d1,d0,d1", 1, 2, 0);
#undef COMPARE

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d/%d checks passed\n", checks, checks);
    return 0;
}
