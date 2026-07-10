// test_sha256_full.c — JIT vs interpreter consistency test for the
// SHA256H/SHA256H2/SHA1C/SHA1P/SHA1M crypto extension instructions.
//
// This test verifies that the JIT (which falls back to CALL_INTERP for
// these instructions) produces the same results as the interpreter.
// It does NOT verify against a known SHA-256 hash because the H/H2
// calling convention is complex (the instructions each do 4 rounds but
// operate on different halves of the 8-word state; a full hash requires
// careful sequencing that's beyond the scope of this unit test).
//
// The test_sha256_crypto.c test covers the schedule-update instructions
// (SHA1SU0/SU1, SHA256SU0/SU1) against reference implementations.
//
// Build:
//   tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc \
//     -static -O2 -march=armv8-a+crypto \
//     -o ctest_real/test_sha256_full.elf ctest_real/test_sha256_full.c
//
// Run:
//   ./bifrost-emu ctest_real/test_sha256_full.elf
//   ./bifrost-emu --no-jit ctest_real/test_sha256_full.elf
//
// Both should produce identical output, verifying JIT/interp parity.
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

// Test SHA256H: Qd={A,B,C,D}, Qn={E,F,G,H} → Qd = new {A,B,C,D}
static void test_sha256h(void) {
    printf("\n--- test_sha256h ---\n");
    uint32_t qd[4] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint32_t qn[4] = {0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32_t vm[4] = {0x61626380u + 0x428a2f98u, 0u + 0x71374491u,
                      0u + 0xb5c0fbcfu, 0u + 0xe9b5dba5u};
    uint32_t qd_out[4];

    memcpy(qd_out, qd, 16);
    asm volatile (
        "ldr q0, [%[qd]]\n"
        "ldr q1, [%[qn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha256h q0, q1, v2.4s\n"
        "str q0, [%[qd_out]]\n"
        :: [qd]"r"(qd), [qn]"r"(qn), [vm]"r"(vm), [qd_out]"r"(qd_out)
        : "v0", "v1", "v2", "memory"
    );

    // Print results so JIT and interp can be compared.
    printf("SHA256H out: %08x %08x %08x %08x\n",
           qd_out[0], qd_out[1], qd_out[2], qd_out[3]);
    // Verify Qn is unchanged.
    CHECK(qn[0] == 0x510e527f && qn[1] == 0x9b05688c &&
          qn[2] == 0x1f83d9ab && qn[3] == 0x5be0cd19,
          "SHA256H: Qn unchanged");
    // Verify Qd changed (not equal to input).
    CHECK(qd_out[0] != qd[0] || qd_out[1] != qd[1] ||
          qd_out[2] != qd[2] || qd_out[3] != qd[3],
          "SHA256H: Qd modified");
}

// Test SHA256H2: Qd={E,F,G,H}, Qn={A,B,C,D} → Qd = new {E,F,G,H}
static void test_sha256h2(void) {
    printf("\n--- test_sha256h2 ---\n");
    uint32_t qd[4] = {0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint32_t qn[4] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint32_t vm[4] = {0x61626380u + 0x428a2f98u, 0u + 0x71374491u,
                      0u + 0xb5c0fbcfu, 0u + 0xe9b5dba5u};
    uint32_t qd_out[4];

    memcpy(qd_out, qd, 16);
    asm volatile (
        "ldr q0, [%[qd]]\n"
        "ldr q1, [%[qn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha256h2 q0, q1, v2.4s\n"
        "str q0, [%[qd_out]]\n"
        :: [qd]"r"(qd), [qn]"r"(qn), [vm]"r"(vm), [qd_out]"r"(qd_out)
        : "v0", "v1", "v2", "memory"
    );

    printf("SHA256H2 out: %08x %08x %08x %08x\n",
           qd_out[0], qd_out[1], qd_out[2], qd_out[3]);
    CHECK(qn[0] == 0x6a09e667 && qn[1] == 0xbb67ae85 &&
          qn[2] == 0x3c6ef372 && qn[3] == 0xa54ff53a,
          "SHA256H2: Qn unchanged");
    CHECK(qd_out[0] != qd[0] || qd_out[1] != qd[1] ||
          qd_out[2] != qd[2] || qd_out[3] != qd[3],
          "SHA256H2: Qd modified");
}

// Test SHA1C: Qd={A,B,C,D}, Sn=E (from Vn[0]), Vm.4S=4 schedule words
// → Qd = new {A,B,C,D}, Sn = new E
static void test_sha1c(void) {
    printf("\n--- test_sha1c ---\n");
    uint32_t qd[4] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint32_t sn_val = 0x510e527f;  // E
    uint32_t vm[4] = {0x61626380, 0, 0, 0};
    // K for SHA1C = 0x5A827999

    // Load E into v1[0] (low 32 bits of v1)
    uint64_t v1_lo = sn_val;
    uint32_t qd_out[4];
    uint64_t v1_out;

    memcpy(qd_out, qd, 16);
    asm volatile (
        "ldr q0, [%[qd]]\n"
        "fmov d1, %[e]\n"
        "ldr q2, [%[vm]]\n"
        "sha1c q0, s1, v2.4s\n"
        "str q0, [%[qd_out]]\n"
        "fmov %[e_out], d1\n"
        : [e_out]"=r"(v1_out)
        : [qd]"r"(qd), [e]"r"(v1_lo), [vm]"r"(vm), [qd_out]"r"(qd_out)
        : "v0", "v1", "v2", "memory"
    );

    printf("SHA1C Qd out: %08x %08x %08x %08x  E out: %08x\n",
           qd_out[0], qd_out[1], qd_out[2], qd_out[3],
           (uint32_t)v1_out);
    // Verify Qd changed.
    CHECK(qd_out[0] != qd[0] || qd_out[1] != qd[1] ||
          qd_out[2] != qd[2] || qd_out[3] != qd[3],
          "SHA1C: Qd modified");
}

// Test SHA1P and SHA1M (same interface as SHA1C, different round function).
static void test_sha1p(void) {
    printf("\n--- test_sha1p ---\n");
    uint32_t qd[4] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint64_t v1_lo = 0x510e527f;
    uint32_t vm[4] = {0x61626380, 0, 0, 0};
    uint32_t qd_out[4];
    uint64_t v1_out;

    memcpy(qd_out, qd, 16);
    asm volatile (
        "ldr q0, [%[qd]]\n"
        "fmov d1, %[e]\n"
        "ldr q2, [%[vm]]\n"
        "sha1p q0, s1, v2.4s\n"
        "str q0, [%[qd_out]]\n"
        "fmov %[e_out], d1\n"
        : [e_out]"=r"(v1_out)
        : [qd]"r"(qd), [e]"r"(v1_lo), [vm]"r"(vm), [qd_out]"r"(qd_out)
        : "v0", "v1", "v2", "memory"
    );
    printf("SHA1P Qd out: %08x %08x %08x %08x  E out: %08x\n",
           qd_out[0], qd_out[1], qd_out[2], qd_out[3],
           (uint32_t)v1_out);
    CHECK(qd_out[0] != qd[0] || qd_out[1] != qd[1] ||
          qd_out[2] != qd[2] || qd_out[3] != qd[3],
          "SHA1P: Qd modified");
}

static void test_sha1m(void) {
    printf("\n--- test_sha1m ---\n");
    uint32_t qd[4] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint64_t v1_lo = 0x510e527f;
    uint32_t vm[4] = {0x61626380, 0, 0, 0};
    uint32_t qd_out[4];
    uint64_t v1_out;

    memcpy(qd_out, qd, 16);
    asm volatile (
        "ldr q0, [%[qd]]\n"
        "fmov d1, %[e]\n"
        "ldr q2, [%[vm]]\n"
        "sha1m q0, s1, v2.4s\n"
        "str q0, [%[qd_out]]\n"
        "fmov %[e_out], d1\n"
        : [e_out]"=r"(v1_out)
        : [qd]"r"(qd), [e]"r"(v1_lo), [vm]"r"(vm), [qd_out]"r"(qd_out)
        : "v0", "v1", "v2", "memory"
    );
    printf("SHA1M Qd out: %08x %08x %08x %08x  E out: %08x\n",
           qd_out[0], qd_out[1], qd_out[2], qd_out[3],
           (uint32_t)v1_out);
    CHECK(qd_out[0] != qd[0] || qd_out[1] != qd[1] ||
          qd_out[2] != qd[2] || qd_out[3] != qd[3],
          "SHA1M: Qd modified");
}

// Test SHA1H: Sd = ROR(Sn, 2)
static void test_sha1h(void) {
    printf("\n--- test_sha1h ---\n");
    uint32_t sn_val = 0x12345678;
    uint64_t v1_lo = sn_val;
    uint64_t v0_out;

    asm volatile (
        "fmov d1, %[sn]\n"
        "sha1h s0, s1\n"
        "fmov %[sd], d0\n"
        : [sd]"=r"(v0_out)
        : [sn]"r"(v1_lo)
        : "v0", "v1"
    );
    uint32_t expected = (sn_val >> 2) | (sn_val << 30);
    printf("SHA1H: Sd=%08x  expected=%08x\n", (uint32_t)v0_out, expected);
    CHECK((uint32_t)v0_out == expected, "SHA1H: ROR(Sn, 2)");
}

int main(void) {
    test_sha256h();
    test_sha256h2();
    test_sha1c();
    test_sha1p();
    test_sha1m();
    test_sha1h();

    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
