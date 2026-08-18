// jit_simd_pairmin.c — SMAXP/SMINP/UMAXP/UMINP (pairwise max/min) coverage.
// Native in the JIT since 1.5.3-alpha. Expected values computed in C from
// the same semantics as interp_fp.cpp's pairwise block: Q=1 -> Vd =
// pairwise(Vn) ++ pairwise(Vm) (16 bytes); Q=0 -> Vd = pairwise(Vn) ++
// pairwise(Vm) packed into the low 8 bytes (both sources ALWAYS
// contribute; the second half is NOT zeroed). High-bit-heavy operand
// patterns catch signed/unsigned mixups; negative-heavy patterns catch
// sign-extension bugs.
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

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

// Q=1 pairwise: exp[i] = op(a[2i], a[2i+1]) for i < N/2, then the same for b.
#define TEST_PAIR(name, INS, ETYPE, N, AM, BM, OP)              \
static void name(void) {                                        \
    ETYPE a[N], b[N], out[N], exp[N];                           \
    for (int i = 0; i < N; i++) { a[i] = (ETYPE)(AM); b[i] = (ETYPE)(BM); } \
    for (int i = 0; i < N/2; i++) {                             \
        exp[i] = OP(a[2*i], a[2*i+1]);                          \
        exp[N/2+i] = OP(b[2*i], b[2*i+1]);                      \
    }                                                           \
    __asm__ volatile (                                          \
        "ldr q0, [%[a]]\n"                                      \
        "ldr q1, [%[b]]\n"                                      \
        INS "\n"                                                \
        "str q2, [%[out]]\n"                                    \
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"); \
    CHECK(memcmp(out, exp, sizeof(exp)) == 0, #name);           \
}

// Q=0 pairwise: N/2 results from a then N/2 from b, all packed into the
// low 8 bytes of Vd (both sources contribute; real-ARM semantics).
#define TEST_PAIR_Q0(name, INS, ETYPE, N, AM, BM, OP)           \
static void name(void) {                                        \
    ETYPE a[N], b[N], out[16], exp[N];                          \
    memset(out, 0, sizeof(out));                                \
    for (int i = 0; i < N; i++) { a[i] = (ETYPE)(AM); b[i] = (ETYPE)(BM); } \
    for (int i = 0; i < N/2; i++) {                             \
        exp[i] = OP(a[2*i], a[2*i+1]);                          \
        exp[N/2+i] = OP(b[2*i], b[2*i+1]);                      \
    }                                                           \
    __asm__ volatile (                                          \
        "ldr d0, [%[a]]\n"                                      \
        "ldr d1, [%[b]]\n"                                      \
        INS "\n"                                                \
        "str q2, [%[out]]\n"                                    \
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"); \
    CHECK(memcmp(out, exp, sizeof(exp)) == 0, #name);           \
}

// ── Signed (SMAXP/SMINP) — negative-heavy operands ──────────────────────
TEST_PAIR(test_smaxp_16b, "smaxp v2.16b, v0.16b, v1.16b", int8_t, 16,
          (int)(i*17 - 120), (int)(-i*23 + 100), MAX)
TEST_PAIR(test_sminp_16b, "sminp v2.16b, v0.16b, v1.16b", int8_t, 16,
          (int)(i*17 - 120), (int)(-i*23 + 100), MIN)
TEST_PAIR(test_smaxp_8h,  "smaxp v2.8h, v0.8h, v1.8h",    int16_t, 8,
          (int)(i*300 - 2000), (int)(-i*700 + 3000), MAX)
TEST_PAIR(test_sminp_8h,  "sminp v2.8h, v0.8h, v1.8h",    int16_t, 8,
          (int)(i*300 - 2000), (int)(-i*700 + 3000), MIN)
TEST_PAIR(test_smaxp_4s,  "smaxp v2.4s, v0.4s, v1.4s",    int32_t, 4,
          (int)(i*1000000 - 20000000), (int)(-i*3000000 + 5000000), MAX)
TEST_PAIR(test_sminp_4s,  "sminp v2.4s, v0.4s, v1.4s",    int32_t, 4,
          (int)(i*1000000 - 20000000), (int)(-i*3000000 + 5000000), MIN)

// ── Unsigned (UMAXP/UMINP) — high-bit-heavy operands ────────────────────
TEST_PAIR(test_umaxp_16b, "umaxp v2.16b, v0.16b, v1.16b", uint8_t, 16,
          (int)(255 - i*13), (int)(i*29 + 200), MAX)
TEST_PAIR(test_uminp_16b, "uminp v2.16b, v0.16b, v1.16b", uint8_t, 16,
          (int)(255 - i*13), (int)(i*29 + 200), MIN)
TEST_PAIR(test_umaxp_8h,  "umaxp v2.8h, v0.8h, v1.8h",    uint16_t, 8,
          (int)(0xFF00 + i*257), (int)(0xE000 - i*997), MAX)
TEST_PAIR(test_uminp_8h,  "uminp v2.8h, v0.8h, v1.8h",    uint16_t, 8,
          (int)(0xFF00 + i*257), (int)(0xE000 - i*997), MIN)
TEST_PAIR(test_umaxp_4s,  "umaxp v2.4s, v0.4s, v1.4s",    uint32_t, 4,
          (int)(0xFFFF0000u + i*0x10001), (int)(0xC0000000u - i*0x100000), MAX)
TEST_PAIR(test_uminp_4s,  "uminp v2.4s, v0.4s, v1.4s",    uint32_t, 4,
          (int)(0xFFFF0000u + i*0x10001), (int)(0xC0000000u - i*0x100000), MIN)

// ── Q=0 (64-bit operand, 4 result bytes, v_hi zeroed) ────────────────────
TEST_PAIR_Q0(test_smaxp_8b_q0, "smaxp v2.8b, v0.8b, v1.8b", int8_t, 8,
             (int)(i*17 - 120), (int)(-i*23 + 100), MAX)
TEST_PAIR_Q0(test_sminp_8b_q0, "sminp v2.8b, v0.8b, v1.8b", int8_t, 8,
             (int)(i*17 - 120), (int)(-i*23 + 100), MIN)
TEST_PAIR_Q0(test_smaxp_4h_q0, "smaxp v2.4h, v0.4h, v1.4h", int16_t, 4,
             (int)(i*300 - 2000), (int)(-i*700 + 3000), MAX)
TEST_PAIR_Q0(test_sminp_4h_q0, "sminp v2.4h, v0.4h, v1.4h", int16_t, 4,
             (int)(i*300 - 2000), (int)(-i*700 + 3000), MIN)
TEST_PAIR_Q0(test_umaxp_2s_q0, "umaxp v2.2s, v0.2s, v1.2s", uint32_t, 2,
             (int)(0xFFFF0000u + i*0x10001), (int)(0xC0000000u - i*0x100000), MAX)
TEST_PAIR_Q0(test_uminp_2s_q0, "uminp v2.2s, v0.2s, v1.2s", uint32_t, 2,
             (int)(0xFFFF0000u + i*0x10001), (int)(0xC0000000u - i*0x100000), MIN)

// The teeworlds hot form: uminp v0.16b, v0.16b, v0.16b (same register for
// both sources — exercises the Vn==Vm path in the JIT).
static void test_uminp_self(void) {
    uint8_t a[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) a[i] = (uint8_t)(255 - i*13);
    for (int i = 0; i < 8; i++) {
        exp[i] = a[2*i] < a[2*i+1] ? a[2*i] : a[2*i+1];
        exp[8+i] = exp[i];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "uminp v0.16b, v0.16b, v0.16b\n"
        "str q0, [%[out]]\n"
        :: [a]"r"(a), [out]"r"(out) : "v0","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "uminp v0.16b, v0.16b, v0.16b (self)");
}

int main(void) {
    printf("=== jit_simd_pairmin ===\n");
    test_smaxp_16b();
    test_sminp_16b();
    test_smaxp_8h();
    test_sminp_8h();
    test_smaxp_4s();
    test_sminp_4s();
    test_umaxp_16b();
    test_uminp_16b();
    test_umaxp_8h();
    test_uminp_8h();
    test_umaxp_4s();
    test_uminp_4s();
    test_smaxp_8b_q0();
    test_sminp_8b_q0();
    test_smaxp_4h_q0();
    test_sminp_4h_q0();
    test_umaxp_2s_q0();
    test_uminp_2s_q0();
    test_uminp_self();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}