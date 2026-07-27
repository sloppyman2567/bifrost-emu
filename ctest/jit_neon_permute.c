// jit_neon_permute.c — ZIP/UZP/TRN + SSHLL/USHLL/SHRN coverage.
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

static void test_zip1_16b(void) {
    uint8_t a[16], b[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(0x80 + i); }
    for (int i = 0; i < 8; i++) {
        exp[2*i] = a[i];
        exp[2*i+1] = b[i];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "zip1 v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "zip1 v.16b");
}

static void test_zip2_16b(void) {
    uint8_t a[16], b[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(0x80 + i); }
    for (int i = 0; i < 8; i++) {
        exp[2*i] = a[8+i];
        exp[2*i+1] = b[8+i];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "zip2 v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "zip2 v.16b");
}

static void test_uzp1_8h(void) {
    uint16_t a[8], b[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { a[i] = (uint16_t)(0x100 + i); b[i] = (uint16_t)(0x200 + i); }
    exp[0]=a[0]; exp[1]=a[2]; exp[2]=a[4]; exp[3]=a[6];
    exp[4]=b[0]; exp[5]=b[2]; exp[6]=b[4]; exp[7]=b[6];
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "uzp1 v2.8h, v0.8h, v1.8h\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "uzp1 v.8h");
}

static void test_trn2_8h(void) {
    uint16_t a[8], b[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { a[i] = (uint16_t)(0x10 + i); b[i] = (uint16_t)(0x20 + i); }
    for (int i = 0; i < 4; i++) {
        exp[2*i] = a[2*i+1];
        exp[2*i+1] = b[2*i+1];
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "trn2 v2.8h, v0.8h, v1.8h\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "trn2 v.8h");
}

static void test_sshll(void) {
    int8_t src[8] = {-8, -1, 0, 1, 2, 3, 4, 5};
    int16_t out[8] = {0};
    int16_t exp[8];
    for (int i = 0; i < 8; i++) exp[i] = (int16_t)((int16_t)src[i] << 3);
    __asm__ volatile (
        "ldr d0, [%[s]]\n"
        "sshll v1.8h, v0.8b, #3\n"
        "str q1, [%[out]]\n"
        :: [s]"r"(src), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sshll v.8h, v.8b, #3");
}

static void test_ushll(void) {
    uint16_t src[4] = {0x0001, 0x0010, 0x0100, 0x1000};
    uint32_t out[4] = {0};
    uint32_t exp[4] = {0x0001, 0x0010, 0x0100, 0x1000};
    __asm__ volatile (
        "ldr d0, [%[s]]\n"
        "ushll v1.4s, v0.4h, #0\n"
        "str q1, [%[out]]\n"
        :: [s]"r"(src), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ushll v.4s, v.4h, #0");
}

static void test_shrn(void) {
    uint16_t src[8] = {0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800};
    uint8_t out[16] = {0};
    uint8_t exp_lo[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "shrn v1.8b, v0.8h, #4\n"
        "str q1, [%[out]]\n"
        :: [s]"r"(src), [out]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp_lo, 8) == 0, "shrn v.8b, v.8h, #4");
}

static void test_ext(void) {
    uint8_t a[16], b[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(0xA0 + i); }
    // ext #4: concatenation is [Vm:Vn] = [b:a] = {0xA0..0xAF, 0..15}.
    // Extracting 16 bytes at offset 4: {0xA4..0xAF, 0,1,2,3}.
    for (int i = 0; i < 12; i++) exp[i] = b[4+i];
    for (int i = 0; i < 4; i++) exp[12+i] = a[i];
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "ext v2.16b, v0.16b, v1.16b, #4\n"
        "str q2, [%[out]]\n"
        :: [a]"r"(a), [b]"r"(b), [out]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ext v.16b #4");
}

int main(void) {
    printf("=== jit_neon_permute ===\n");
    test_zip1_16b();
    test_zip2_16b();
    test_uzp1_8h();
    test_trn2_8h();
    test_sshll();
    test_ushll();
    test_shrn();
    test_ext();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
