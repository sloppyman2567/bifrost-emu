// jit_simd_faddp.c — vector FP pairwise ops (FADDP/FMAXP/FMINP/FMAXNMP/
// FMINNMP, three-same U=1) in both float sizes.
// ARM ARM: the source is Vm:Vn and Vd[e] = op(concat[2e], concat[2e+1]),
// so Vn's pairs fill the low half and Vm's the high half; Q=0 zeroes the
// top 64 bits. Unity's libunity constructor runs
// `faddp v17.2s, v17.2s, v18.2s`, an aliased form (Vd == Vn), which used
// to raise SIGILL. Operands are exactly representable so sums are exact.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while (0)

// Run INS with v0 = a, v1 = b and v2 prefilled with junk; store v2.
#define RUN(INS, a, b, out) \
    __asm__ volatile ( \
        "ldr q0, [%[pa]]\n" \
        "ldr q1, [%[pb]]\n" \
        "movi v2.16b, #0xa5\n" \
        INS "\n" \
        "str q2, [%[po]]\n" \
        :: [pa]"r"(a), [pb]"r"(b), [po]"r"(out) : "v0", "v1", "v2", "memory")

int main(void) {
    const float a[4] = {1.5f, 2.25f, -8.0f, 0.125f};
    const float b[4] = {100.0f, -3.5f, 7.0f, 7.0f};
    float out[4];

    RUN("faddp v2.2s, v0.2s, v1.2s", a, b, out);
    {
        const float exp[4] = {3.75f, 96.5f, 0.0f, 0.0f};
        uint64_t hi;
        memcpy(&hi, &out[2], 8);
        CHECK(out[0] == exp[0] && out[1] == exp[1], "faddp .2s pairs from Vn then Vm");
        CHECK(hi == 0, "faddp .2s zeroes the top 64 bits");
    }

    RUN("faddp v2.4s, v0.4s, v1.4s", a, b, out);
    {
        const float exp[4] = {3.75f, -7.875f, 96.5f, 14.0f};
        CHECK(memcmp(out, exp, sizeof(exp)) == 0, "faddp .4s");
    }

    RUN("fmaxp v2.4s, v0.4s, v1.4s", a, b, out);
    {
        const float exp[4] = {2.25f, 0.125f, 100.0f, 7.0f};
        CHECK(memcmp(out, exp, sizeof(exp)) == 0, "fmaxp .4s");
    }
    RUN("fminp v2.4s, v0.4s, v1.4s", a, b, out);
    {
        const float exp[4] = {1.5f, -8.0f, -3.5f, 7.0f};
        CHECK(memcmp(out, exp, sizeof(exp)) == 0, "fminp .4s");
    }

    // NaN in one pair: FMAXP/FMINP propagate it, the NM forms return the number.
    const float an[4] = {NAN, 4.0f, 1.0f, 2.0f};
    RUN("fmaxp v2.4s, v0.4s, v1.4s", an, b, out);
    CHECK(isnan(out[0]) && out[1] == 2.0f, "fmaxp propagates NaN");
    RUN("fminp v2.4s, v0.4s, v1.4s", an, b, out);
    CHECK(isnan(out[0]) && out[1] == 1.0f, "fminp propagates NaN");
    RUN("fmaxnmp v2.4s, v0.4s, v1.4s", an, b, out);
    CHECK(out[0] == 4.0f && out[1] == 2.0f && out[2] == 100.0f, "fmaxnmp ignores quiet NaN");
    RUN("fminnmp v2.4s, v0.4s, v1.4s", an, b, out);
    CHECK(out[0] == 4.0f && out[1] == 1.0f && out[3] == 7.0f, "fminnmp ignores quiet NaN");

    const double da[2] = {1.5, -0.25};
    const double db[2] = {1e10, 2.0};
    double dout[2];
    RUN("faddp v2.2d, v0.2d, v1.2d", da, db, dout);
    CHECK(dout[0] == 1.25 && dout[1] == 1e10 + 2.0, "faddp .2d");
    RUN("fmaxp v2.2d, v0.2d, v1.2d", da, db, dout);
    CHECK(dout[0] == 1.5 && dout[1] == 1e10, "fmaxp .2d");
    RUN("fminnmp v2.2d, v0.2d, v1.2d", da, db, dout);
    CHECK(dout[0] == -0.25 && dout[1] == 2.0, "fminnmp .2d");

    // Aliased destinations: Vd == Vn (the Unity form) and Vd == Vm.
    __asm__ volatile (
        "ldr q17, [%[pa]]\n"
        "ldr q18, [%[pb]]\n"
        "faddp v17.2s, v17.2s, v18.2s\n"
        "str q17, [%[po]]\n"
        :: [pa]"r"(a), [pb]"r"(b), [po]"r"(out) : "v17", "v18", "memory");
    {
        uint64_t hi;
        memcpy(&hi, &out[2], 8);
        CHECK(out[0] == 3.75f && out[1] == 96.5f && hi == 0, "faddp v17.2s, v17.2s, v18.2s");
    }
    __asm__ volatile (
        "ldr q0, [%[pa]]\n"
        "ldr q1, [%[pb]]\n"
        "faddp v1.4s, v0.4s, v1.4s\n"
        "str q1, [%[po]]\n"
        :: [pa]"r"(a), [pb]"r"(b), [po]"r"(out) : "v0", "v1", "memory");
    {
        const float exp[4] = {3.75f, -7.875f, 96.5f, 14.0f};
        CHECK(memcmp(out, exp, sizeof(exp)) == 0, "faddp with Vd == Vm");
    }

    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
