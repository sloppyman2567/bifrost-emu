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

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d/%d checks passed\n", checks, checks);
    return 0;
}
