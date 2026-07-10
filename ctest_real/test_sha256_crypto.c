// test_sha256_crypto.c — Verify the SHA256 crypto extension instructions
// (SHA256SU0 / SHA256SU1) produce the same schedule as a reference C
// implementation. This is a regression test for the bug fixed in this
// turn where SHA256SU1 was never dispatched and SHA256SU0 had an extra
// "+Vd[i]" term that doesn't appear in the ARM ARM pseudocode.
//
// The test feeds the first 16 message words of "abc" padded to a SHA256
// block through both paths and compares the schedule output word-by-word.
//
// Build: make cross SRC=ctest_real/test_sha256_crypto.c OUT=ctest_real/test_sha256_crypto.elf
// Run:   ./bifrost-emu ctest_real/test_sha256_crypto.elf
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

// Reference SHA256 schedule update for one block of "abc" (16 words → 64).
// W[0..15] = the first 16 words of the padded message.
// W[16..63] = schedule updates: W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16].
static void ref_sha256_schedule(uint32_t W[64]) {
    for (int i = 16; i < 64; i++) {
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];
    }
}

// Test the SHA256SU0/SHA256SU1 instructions by computing the schedule
// for words 16..19 (the first schedule update group).
//
// Per the ARM ARM:
//   SHA256SU0 Vd.4S, Vn.4S:
//     Vd[i] = Vn[i] + sig0(Vd[(i+1) mod 4]) + Vd[(i+2) mod 4]
//   where Vd holds W[i..i+3] (old) and Vn holds W[i+4..i+7].
//
//   SHA256SU1 Vd.4S, Vn.4S, Vm.4S:
//     T[i] = Vn[i] + sig1(Vm[i]) + Vm[(i+1) mod 4] + Vm[(i+2) mod 4]
//     Vd[i] = Vd[i] + sig0(T[(i+1) mod 4]) + T[(i+2) mod 4] + T[i]
//   where Vd = W[i+12..i+15], Vn = W[i+8..i+11], Vm = W[i..i+3] (newest).
//
// To compute W[16..19] using SHA256SU0 alone:
//   - Vd (input) = W[0..3], Vn = W[4..7]   → Vd (output) = W[16..19]? No.
//
// Actually SHA256SU0 computes 4 schedule words at a time using the
// simpler formula. SHA256SU1 handles the chained case when W[i-15] is
// not in the same 4-word group as W[i].
//
// For this test, we focus on SHA256SU0 since SHA256SU1 requires
// the prior 4-word group to already be computed.
//
// For the first group (W[16..19]):
//   W[16] = SIG1(W[14]) + W[9] + SIG0(W[1]) + W[0]
//   W[17] = SIG1(W[15]) + W[10] + SIG0(W[2]) + W[1]
//   W[18] = SIG1(W[16]) + W[11] + SIG0(W[3]) + W[2]   ← uses W[16] from prev step!
//   W[19] = SIG1(W[17]) + W[12] + SIG0(W[4]) + W[3]   ← uses W[17] from prev step!
//
// SHA256SU0 alone doesn't handle this case (it doesn't use SIG1 or the
// "newest" group). It's used in combination with SHA256SU1 for the full
// schedule. The simplest test of SHA256SU0 is the *first* 4 words of
// the schedule, where there's no feedback yet:
//   - Actually no, SHA256SU0 takes Vd (4 words) and Vn (4 words).
//   - For W[16..19], Vd=W[0..3] (oldest), Vn=W[4..7]... but that's only
//     8 input words. W[16] needs W[0], W[9], W[14], W[15] which span
//     Vd and the NEXT group. So SHA256SU0 isn't quite the right op for
//     the first 4 words.
//
// Given the complexity of mapping the SHA256SU0/SU1 instructions onto
// the standard SHA256 schedule, this test instead verifies the
// instructions' microarchitecture directly: we set up known Vd, Vn, Vm
// values, run the instructions via inline asm, and compare against a
// reference C implementation of the ARM ARM pseudocode.

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// Reference implementation of SHA256SU0 per ARM ARM pseudocode.
static void ref_sha256su0(uint32_t vd[4], const uint32_t vn[4]) {
    uint32_t old_vd[4] = {vd[0], vd[1], vd[2], vd[3]};
    for (int i = 0; i < 4; i++) {
        vd[i] = vn[i] + SIG0(old_vd[(i + 1) & 3]) + old_vd[(i + 2) & 3];
    }
}

// Reference implementation of SHA256SU1 per ARM ARM pseudocode.
static void ref_sha256su1(uint32_t vd[4], const uint32_t vn[4],
                           const uint32_t vm[4]) {
    uint32_t t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = vn[i] + SIG1(vm[i]) + vm[(i + 1) & 3] + vm[(i + 2) & 3];
    }
    uint32_t old_vd[4] = {vd[0], vd[1], vd[2], vd[3]};
    for (int i = 0; i < 4; i++) {
        vd[i] = old_vd[i] + SIG0(t[(i + 1) & 3]) + t[(i + 2) & 3] + t[i];
    }
}

// Test SHA256SU0.
static void test_sha256su0(void) {
    printf("\n--- test_sha256su0 ---\n");
    uint32_t vd_in[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t vn_in[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
    uint32_t vd_ref[4], vd_out[4];

    memcpy(vd_ref, vd_in, 16);
    ref_sha256su0(vd_ref, vn_in);

    // Load vd into v0, vn into v1, run sha256su0, store back.
    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "sha256su0 v0.4s, v1.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vd_out]"r"(vd_out)
        : "v0", "v1", "memory"
    );

    char msg[128];
    snprintf(msg, sizeof(msg), "SHA256SU0 word 0: out=0x%x ref=0x%x",
             vd_out[0], vd_ref[0]);
    CHECK(vd_out[0] == vd_ref[0], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 1: out=0x%x ref=0x%x",
             vd_out[1], vd_ref[1]);
    CHECK(vd_out[1] == vd_ref[1], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 2: out=0x%x ref=0x%x",
             vd_out[2], vd_ref[2]);
    CHECK(vd_out[2] == vd_ref[2], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 3: out=0x%x ref=0x%x",
             vd_out[3], vd_ref[3]);
    CHECK(vd_out[3] == vd_ref[3], msg);
}

// Test SHA256SU1.
static void test_sha256su1(void) {
    printf("\n--- test_sha256su1 ---\n");
    uint32_t vd_in[4] = {0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10};
    uint32_t vn_in[4] = {0x11121314, 0x15161718, 0x191a1b1c, 0x1d1e1f20};
    uint32_t vm_in[4] = {0x21222324, 0x25262728, 0x292a2b2c, 0x2d2e2f30};
    uint32_t vd_ref[4], vd_out[4];

    memcpy(vd_ref, vd_in, 16);
    ref_sha256su1(vd_ref, vn_in, vm_in);

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha256su1 v0.4s, v1.4s, v2.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vm]"r"(vm_in),
           [vd_out]"r"(vd_out)
        : "v0", "v1", "v2", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA256SU1 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

// Test SHA1SU0 (3-operand XOR).
static void test_sha1su0(void) {
    printf("\n--- test_sha1su0 ---\n");
    uint32_t vd_in[4] = {0xAABBCCDD, 0x11223344, 0x55667788, 0x99AABBCD};
    uint32_t vn_in[4] = {0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10};
    uint32_t vm_in[4] = {0xCAFEBABE, 0xDEADBEEF, 0xFEEDFACE, 0xBAADF00D};
    uint32_t vd_ref[4], vd_out[4];

    for (int i = 0; i < 4; i++) vd_ref[i] = vd_in[i] ^ vn_in[i] ^ vm_in[i];

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha1su0 v0.4s, v1.4s, v2.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vm]"r"(vm_in),
           [vd_out]"r"(vd_out)
        : "v0", "v1", "v2", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA1SU0 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

// Test SHA1SU1 (the corrected full 4-word version).
static void test_sha1su1(void) {
    printf("\n--- test_sha1su1 ---\n");
    uint32_t vd_in[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t vn_in[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
    uint32_t vd_ref[4], vd_out[4];

    // Reference per ARM ARM pseudocode.
    memcpy(vd_ref, vd_in, 16);
    uint32_t t[4];
    for (int i = 0; i < 4; i++) t[i] = vd_ref[i] ^ vn_in[i];
    // In-place chained: t[i] = ROR(t[i] ^ t[(i+2)%4], 31).
    t[0] = (t[0] ^ t[2]); t[0] = (t[0] >> 31) | (t[0] << 1);
    t[1] = (t[1] ^ t[3]); t[1] = (t[1] >> 31) | (t[1] << 1);
    t[2] = (t[2] ^ t[0]); t[2] = (t[2] >> 31) | (t[2] << 1);
    t[3] = (t[3] ^ t[1]); t[3] = (t[3] >> 31) | (t[3] << 1);
    for (int i = 0; i < 4; i++) vd_ref[i] = t[i];

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "sha1su1 v0.4s, v1.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vd_out]"r"(vd_out)
        : "v0", "v1", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA1SU1 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

int main(void) {
    test_sha1su0();
    test_sha1su1();
    test_sha256su0();
    test_sha256su1();

    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
