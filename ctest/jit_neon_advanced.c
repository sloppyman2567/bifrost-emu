// jit_neon_advanced.c — Advanced NEON SIMD tests for v1.4.5-alpha.
// Covers SIMD instructions not in jit_neon.elf:
//   - SHL (vector shift left by immediate) — 16/32/64-bit
//   - USHR (unsigned shift right by immediate) — 16/32/64-bit
//   - SSHR (signed shift right by immediate) — 16/32-bit
//   - Combined shift+arithmetic patterns
//
// Each test runs the operation via inline assembly, then checks the
// result against a scalar reference. All tests must pass under both
// JIT and interpreter.
//
// NOTE: All buffers are 16 bytes (128 bits) because ldr/str q0 are
// 128-bit operations. Using smaller buffers causes stack corruption.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// ── SHL (vector shift left by immediate) ──
static void test_shl_16(void) {
    printf("\n--- test_shl_16 ---\n");
    uint16_t v[8] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008};
    uint16_t out[8] = {0};
    uint16_t exp[8];
    for (int i = 0; i < 8; i++) exp[i] = v[i] << 2;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "shl v0.8h, v0.8h, #2\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SHL v.8h #2");
}

static void test_shl_32(void) {
    printf("\n--- test_shl_32 ---\n");
    uint32_t v[4] = {0x00000001, 0x00000002, 0x00000003, 0x00000004};
    uint32_t out[4] = {0};
    uint32_t exp[4];
    for (int i = 0; i < 4; i++) exp[i] = v[i] << 4;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "shl v0.4s, v0.4s, #4\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SHL v.4s #4");
}

static void test_shl_64(void) {
    printf("\n--- test_shl_64 ---\n");
    uint64_t v[2] = {0x0000000000000001ULL, 0x0000000000000002ULL};
    uint64_t out[2] = {0};
    uint64_t exp[2];
    for (int i = 0; i < 2; i++) exp[i] = v[i] << 8;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "shl v0.2d, v0.2d, #8\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SHL v.2d #8");
}

// ── USHR (unsigned shift right by immediate) ──
static void test_ushr_16(void) {
    printf("\n--- test_ushr_16 ---\n");
    uint16_t v[8] = {0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100};
    uint16_t out[8] = {0};
    uint16_t exp[8];
    for (int i = 0; i < 8; i++) exp[i] = v[i] >> 2;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "ushr v0.8h, v0.8h, #2\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "USHR v.8h #2");
}

static void test_ushr_32(void) {
    printf("\n--- test_ushr_32 ---\n");
    uint32_t v[4] = {0x80000000, 0x40000000, 0x20000000, 0x10000000};
    uint32_t out[4] = {0};
    uint32_t exp[4];
    for (int i = 0; i < 4; i++) exp[i] = v[i] >> 4;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "ushr v0.4s, v0.4s, #4\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "USHR v.4s #4");
}

static void test_ushr_64(void) {
    printf("\n--- test_ushr_64 ---\n");
    uint64_t v[2] = {0x8000000000000000ULL, 0x4000000000000000ULL};
    uint64_t out[2] = {0};
    uint64_t exp[2];
    for (int i = 0; i < 2; i++) exp[i] = v[i] >> 8;
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "ushr v0.2d, v0.2d, #8\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "USHR v.2d #8");
}

// ── SSHR (signed shift right by immediate) ──
static void test_sshr_16(void) {
    printf("\n--- test_sshr_16 ---\n");
    int16_t v[8] = {-8, 16, -32, 64, -128, 256, -512, 1024};
    int16_t out[8] = {0};
    int16_t exp[8];
    for (int i = 0; i < 8; i++) exp[i] = v[i] >> 2;  // arithmetic
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "sshr v0.8h, v0.8h, #2\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SSHR v.8h #2 (sign-extends)");
}

static void test_sshr_32(void) {
    printf("\n--- test_sshr_32 ---\n");
    int32_t v[4] = {-16, 32, -64, 128};
    int32_t out[4] = {0};
    int32_t exp[4];
    for (int i = 0; i < 4; i++) exp[i] = v[i] >> 4;  // arithmetic
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "sshr v0.4s, v0.4s, #4\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SSHR v.4s #4 (sign-extends)");
}

// ── Shift-by-zero (no-op) ──
static void test_shl_zero(void) {
    printf("\n--- test_shl_zero ---\n");
    uint32_t v[4] = {0x12345678, 0xDEADBEEF, 0xCAFEBABE, 0x0BADF00D};
    uint32_t out[4] = {0};
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "shl v0.4s, v0.4s, #0\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, v, 16) == 0, "SHL v.4s #0 is a no-op");
}

// ── Shift by max (moves all bits out) ──
static void test_ushr_max(void) {
    printf("\n--- test_ushr_max ---\n");
    uint32_t v[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    uint32_t out[4] = {0};
    uint32_t exp[4] = {0, 0, 0, 0};  // >>32 = 0
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "ushr v0.4s, v0.4s, #32\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "USHR v.4s #32 clears all");
}

// ── Combined pattern: shift then add ──
static void test_shift_then_add(void) {
    printf("\n--- test_shift_then_add ---\n");
    uint32_t v[4] = {1, 2, 3, 4};
    uint32_t out[4] = {0};
    uint32_t exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (v[i] << 4) + v[i];  // v*17
    __asm__ volatile (
        "ldr q0, [%[v]]\n"
        "shl v1.4s, v0.4s, #4\n"
        "add v0.4s, v0.4s, v1.4s\n"
        "str q0, [%[out]]\n"
        :: [v]"r"(v), [out]"r"(out) : "v0", "v1", "memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "SHL #4 then ADD (v*17)");
}

int main(void) {
    printf("=== v1.4.5-alpha Advanced NEON SIMD Tests ===\n");
    test_shl_16();
    test_shl_32();
    test_shl_64();
    test_ushr_16();
    test_ushr_32();
    test_ushr_64();
    test_sshr_16();
    test_sshr_32();
    test_shl_zero();
    test_ushr_max();
    test_shift_then_add();
    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
