// jit_mvni_softfloat.c — MVNI/MOVI/ORR/BIC modified-immediate + soft-float.
//
// Regression for the AdvSIMD modified-immediate rewrite: correct MVNI
// inversion (all-ones masks), LSL amounts, MSL, ORR/BIC, and 64-bit MOVI
// (cmode=0xE,op=1) must coexist with musl soft-float (__muldf3) paths.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

static void test_mvni_4s_zero(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "mvni v0.4s, #0\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    CHECK(out[0] == 0xFFFFFFFFu && out[1] == 0xFFFFFFFFu
          && out[2] == 0xFFFFFFFFu && out[3] == 0xFFFFFFFFu,
          "mvni v.4s #0 → all-ones");
}

static void test_mvni_4s_lsl8(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "mvni v0.4s, #0, lsl #8\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    // ~ (0 << 8) = 0xFFFFFFFF still (imm8 was 0)
    CHECK(out[0] == 0xFFFFFFFFu, "mvni v.4s #0 lsl#8 → all-ones");
}

static void test_mvni_4s_imm(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "mvni v0.4s, #0xab\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    CHECK(out[0] == ~0xabu && out[1] == ~0xabu, "mvni v.4s #0xab");
}

static void test_movi_16b(void) {
    uint8_t out[16] = {0};
    __asm__ volatile (
        "movi v0.16b, #0xff\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    int ok = 1;
    for (int i = 0; i < 16; i++) if (out[i] != 0xff) ok = 0;
    CHECK(ok, "movi v.16b #0xff");
}

static void test_movi_2d_zero(void) {
    // cmode=0xE op=1 — must NOT invert; produces zeros
    uint64_t out[2] = {0xdeadbeefULL, 0xdeadbeefULL};
    __asm__ volatile (
        "movi v0.2d, #0\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    CHECK(out[0] == 0 && out[1] == 0, "movi v.2d #0 (64-bit form)");
}

static void test_movi_msl(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "movi v0.4s, #0x12, msl #8\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    // 0x12 << 8 | 0xFF = 0x12FF
    CHECK(out[0] == 0x12FFu && out[1] == 0x12FFu, "movi v.4s #0x12 msl#8");
}

static void test_mvni_msl(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "mvni v0.4s, #0, msl #8\n"
        "str q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    // ~(0 << 8 | 0xFF) = ~0xFF = 0xFFFFFF00
    CHECK(out[0] == 0xFFFFFF00u, "mvni v.4s #0 msl#8");
}

static void test_orr_bic(void) {
    uint32_t out[4] = {0};
    __asm__ volatile (
        "movi v0.4s, #0\n"
        "orr  v0.4s, #0xff\n"
        "bic  v0.4s, #0x0f\n"
        "str  q0, [%[out]]\n"
        :: [out]"r"(out) : "v0", "memory"
    );
    CHECK(out[0] == 0xF0u && out[1] == 0xF0u, "orr then bic v.4s");
}

static void test_softfloat_mul(void) {
    // Force soft-float-style multiplies via volatile doubles so libc
    // printf/math paths exercise __muldf3 under the emulator.
    volatile double a = 3.141592653589793;
    volatile double b = 2.718281828459045;
    volatile double c = a * b;
    volatile double d = a * a * b * b;
    CHECK(c > 8.53 && c < 8.54, "soft-float a*b in range");
    CHECK(d > 72.8 && d < 73.0, "soft-float chained mul in range");

    // Mask idiom used by SIMD soft-float helpers: mvni → and
    uint64_t mask[2] = {0};
    __asm__ volatile (
        "mvni v1.4s, #0\n"
        "str  q1, [%[m]]\n"
        :: [m]"r"(mask) : "v1", "memory"
    );
    CHECK(mask[0] == ~0ULL && mask[1] == ~0ULL, "mvni mask for soft-float");

    volatile double e = 1.0;
    for (int i = 0; i < 64; i++) e = e * 1.01;
    CHECK(e > 1.8 && e < 2.0, "soft-float iterative mul converges");
}

int main(void) {
    printf("=== jit_mvni_softfloat ===\n");
    test_mvni_4s_zero();
    test_mvni_4s_lsl8();
    test_mvni_4s_imm();
    test_movi_16b();
    test_movi_2d_zero();
    test_movi_msl();
    test_mvni_msl();
    test_orr_bic();
    test_softfloat_mul();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
