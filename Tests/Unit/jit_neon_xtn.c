// jit_neon_xtn.c — XTN/SQXTN/UQXTN/SQXTUN narrowing coverage (base + *2).
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

static void test_xtn_8b(void) {
    uint16_t src[8] = {0xFF00, 0x7FFF, 0x8001, 0x1234, 0xABCD, 0x0010, 0xFF01, 0x4321};
    uint8_t out[16];
    uint8_t exp[16] = {0x00, 0xFF, 0x01, 0x34, 0xCD, 0x10, 0x01, 0x21, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "xtn v1.8b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "xtn v.8b, v.8h");
}

static void test_xtn_4h(void) {
    uint32_t src[4] = {0x00001234, 0xABCD5678, 0xFFFFFFFF, 0x7FFF8000};
    uint8_t out[16];
    uint8_t exp[16] = {0x34,0x12, 0x78,0x56, 0xFF,0xFF, 0x00,0x80, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "xtn v1.4h, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "xtn v.4h, v.4s");
}

static void test_xtn2_8b(void) {
    uint16_t src[8] = {0xFF00, 0x7FFF, 0x8001, 0x1234, 0xABCD, 0x0010, 0xFF01, 0x4321};
    uint8_t sentinel[16];
    uint8_t out[16];
    uint8_t exp[16] = {0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,
                       0x00,0xFF,0x01,0x34,0xCD,0x10,0x01,0x21};
    memset(sentinel, 0xAA, sizeof(sentinel));
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q1, [%[st]]\n"
        "ldr q0, [%[s]]\n"
        "xtn2 v1.16b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [st]"r"(sentinel), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "xtn2 v.8b, v.8h (low half preserved)");
}

static void test_sqxtn_8b(void) {
    int16_t src[8] = {-32768, 32767, 255, -1, 128, -128, 291, 0};
    uint8_t out[16];
    uint8_t exp[16] = {0x80, 0x7F, 0x7F, 0xFF, 0x7F, 0x80, 0x7F, 0x00, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "sqxtn v1.8b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtn v.8b, v.8h (signed sat)");
}

static void test_sqxtn_4h(void) {
    int32_t src[4] = {0x7FFFFFFF, 0x80000000, 0x00001234, 0x0000FEDC};
    uint8_t out[16];
    uint8_t exp[16] = {0xFF,0x7F, 0x00,0x80, 0x34,0x12, 0xFF,0x7F, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "sqxtn v1.4h, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtn v.4h, v.4s");
}

static void test_sqxtn2_8b(void) {
    int16_t src[8] = {-32768, 32767, 255, -1, 128, -128, 291, 0};
    uint8_t sentinel[16];
    uint8_t out[16];
    uint8_t exp[16] = {0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,
                       0x80,0x7F,0x7F,0xFF,0x7F,0x80,0x7F,0x00};
    memset(sentinel, 0x55, sizeof(sentinel));
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q1, [%[st]]\n"
        "ldr q0, [%[s]]\n"
        "sqxtn2 v1.16b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [st]"r"(sentinel), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtn2 v.8b, v.8h");
}

static void test_uqxtn_8b(void) {
    uint16_t src[8] = {0xFFFF, 0x0000, 0x00FF, 0x0100, 0x007F, 0x0080, 0x0023, 0x1234};
    uint8_t out[16];
    uint8_t exp[16] = {0xFF, 0x00, 0xFF, 0xFF, 0x7F, 0x80, 0x23, 0xFF, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "uqxtn v1.8b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "uqxtn v.8b, v.8h (unsigned sat)");
}

static void test_uqxtn_2s(void) {
    uint64_t src[2] = {0xFFFFFFFFFFFFFFFF, 0x0000000012345678};
    uint8_t out[16];
    uint8_t exp[16] = {0xFF,0xFF,0xFF,0xFF, 0x78,0x56,0x34,0x12, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "uqxtn v1.2s, v0.2d\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "uqxtn v.2s, v.2d");
}

static void test_uqxtn2_8b(void) {
    uint16_t src[8] = {0xFFFF, 0x0000, 0x00FF, 0x0100, 0x007F, 0x0080, 0x0023, 0x1234};
    uint8_t sentinel[16];
    uint8_t out[16];
    uint8_t exp[16] = {0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,
                       0xFF,0x00,0xFF,0xFF,0x7F,0x80,0x23,0xFF};
    memset(sentinel, 0x33, sizeof(sentinel));
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q1, [%[st]]\n"
        "ldr q0, [%[s]]\n"
        "uqxtn2 v1.16b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [st]"r"(sentinel), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "uqxtn2 v.8b, v.8h");
}

static void test_sqxtun_8b(void) {
    int16_t src[8] = {-32768, 32767, 256, -1, 128, -128, 127, 0x0023};
    uint8_t out[16];
    uint8_t exp[16] = {0x00, 0xFF, 0xFF, 0x00, 0x80, 0x00, 0x7F, 0x23, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "sqxtun v1.8b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtun v.8b, v.8h (signed->unsigned sat)");
}

static void test_sqxtun_4h(void) {
    int32_t src[4] = {0x80000000, 0x7FFFFFFF, 0x00001234, 0x80010000};
    uint8_t out[16];
    uint8_t exp[16] = {0x00,0x00, 0xFF,0xFF, 0x34,0x12, 0x00,0x00, 0,0,0,0,0,0,0,0};
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "sqxtun v1.4h, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtun v.4h, v.4s");
}

static void test_sqxtun2_8b(void) {
    int16_t src[8] = {-32768, 32767, 256, -1, 128, -128, 127, 0x0023};
    uint8_t sentinel[16];
    uint8_t out[16];
    uint8_t exp[16] = {0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77,
                       0x00,0xFF,0xFF,0x00,0x80,0x00,0x7F,0x23};
    memset(sentinel, 0x77, sizeof(sentinel));
    memset(out, 0xEE, sizeof(out));
    __asm__ volatile (
        "ldr q1, [%[st]]\n"
        "ldr q0, [%[s]]\n"
        "sqxtun2 v1.16b, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [st]"r"(sentinel), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "sqxtun2 v.8b, v.8h");
}

int main(void) {
    test_xtn_8b();
    test_xtn_4h();
    test_xtn2_8b();
    test_sqxtn_8b();
    test_sqxtn_4h();
    test_sqxtn2_8b();
    test_uqxtn_8b();
    test_uqxtn_2s();
    test_uqxtn2_8b();
    test_sqxtun_8b();
    test_sqxtun_4h();
    test_sqxtun2_8b();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
