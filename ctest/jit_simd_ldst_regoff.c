// jit_simd_ldst_regoff.c — SIMD&FP LDR/STR (register offset) index scaling.
//
// With S=1 the index is shifted by log2(access size): B=0, H=1, S=2, D=3,
// and Q=4. A Q access encodes size=00 (opc=1x), so taking the shift from
// `size` left the index unscaled: `str q0, [x23, x9, lsl #4]` wrote to
// base + i instead of base + 16 * i. ART's class-linker hash set stored
// 16-byte records 4 bytes off and later read `ptr << 32` as a method.
//
// Runs under both JIT and --no-jit; "checks passed" is the pass marker.
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

static uint8_t buf[256] __attribute__((aligned(16)));

static void fill(void) {
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(i * 7 + 3);
}

static void test_q(void) {
    fill();
    uint64_t lo = 0, hi = 0;
    uint64_t index = 3;
    __asm__ volatile (
        "ldr q0, [%2, %3, lsl #4]\n"
        "fmov %0, d0\n"
        "mov %1, v0.d[1]\n"
        : "=r"(lo), "=r"(hi) : "r"(buf), "r"(index) : "v0", "memory");
    uint64_t want_lo, want_hi;
    memcpy(&want_lo, buf + 48, 8);
    memcpy(&want_hi, buf + 56, 8);
    CHECK(lo == want_lo && hi == want_hi, "ldr q, [xn, xm, lsl #4] reads base + 16*i");

    uint8_t before[256];
    memcpy(before, buf, sizeof(buf));
    index = 5;
    __asm__ volatile (
        "movi v1.16b, #0xab\n"
        "str q1, [%0, %1, lsl #4]\n"
        : : "r"(buf), "r"(index) : "v1", "memory");
    int ok = 1;
    for (int i = 0; i < 256; i++) {
        uint8_t want = (i >= 80 && i < 96) ? 0xab : before[i];
        if (buf[i] != want) ok = 0;
    }
    CHECK(ok, "str q, [xn, xm, lsl #4] writes only base + 16*i");

    // Unshifted (S=0) Q form: byte index.
    fill();
    index = 4;
    __asm__ volatile (
        "ldr q2, [%2, %3]\n"
        "fmov %0, d2\n"
        "mov %1, v2.d[1]\n"
        : "=r"(lo), "=r"(hi) : "r"(buf), "r"(index) : "v2", "memory");
    memcpy(&want_lo, buf + 4, 8);
    memcpy(&want_hi, buf + 12, 8);
    CHECK(lo == want_lo && hi == want_hi, "ldr q, [xn, xm] is unscaled");

    // Extended W index (SXTW #4), negative.
    fill();
    int32_t neg = -2;
    __asm__ volatile (
        "ldr q3, [%2, %w3, sxtw #4]\n"
        "fmov %0, d3\n"
        "mov %1, v3.d[1]\n"
        : "=r"(lo), "=r"(hi) : "r"(buf + 128), "r"(neg) : "v3", "memory");
    memcpy(&want_lo, buf + 96, 8);
    memcpy(&want_hi, buf + 104, 8);
    CHECK(lo == want_lo && hi == want_hi, "ldr q, [xn, wm, sxtw #4] scales a negative index");
}

static void test_bhsd(void) {
    fill();
    uint64_t index = 3, d = 0;
    uint32_t s = 0, h = 0, b = 0;
    __asm__ volatile ("ldr d4, [%1, %2, lsl #3]\n fmov %0, d4\n"
                      : "=r"(d) : "r"(buf), "r"(index) : "v4", "memory");
    uint64_t want_d; memcpy(&want_d, buf + 24, 8);
    CHECK(d == want_d, "ldr d, [xn, xm, lsl #3] reads base + 8*i");
    __asm__ volatile ("ldr s5, [%1, %2, lsl #2]\n fmov %w0, s5\n"
                      : "=r"(s) : "r"(buf), "r"(index) : "v5", "memory");
    uint32_t want_s; memcpy(&want_s, buf + 12, 4);
    CHECK(s == want_s, "ldr s, [xn, xm, lsl #2] reads base + 4*i");
    __asm__ volatile ("ldr h6, [%1, %2, lsl #1]\n umov %w0, v6.h[0]\n"
                      : "=r"(h) : "r"(buf), "r"(index) : "v6", "memory");
    uint16_t want_h; memcpy(&want_h, buf + 6, 2);
    CHECK(h == want_h, "ldr h, [xn, xm, lsl #1] reads base + 2*i");
    __asm__ volatile ("ldr b7, [%1, %2]\n umov %w0, v7.b[0]\n"
                      : "=r"(b) : "r"(buf), "r"(index) : "v7", "memory");
    CHECK(b == buf[3], "ldr b, [xn, xm] reads base + i");
}

// The ART shape: a 16-byte record hash table written by index.
static void test_record_table(void) {
    struct rec { uint64_t ptr; uint32_t index; uint32_t state; };
    static struct rec table[8] __attribute__((aligned(16)));
    memset(table, 0, sizeof(table));
    for (uint64_t i = 0; i < 8; i++) {
        struct rec r = { 0x7298a40328ULL + i * 0x100, (uint32_t)(0x1a00 + i), (uint32_t)(i % 6) };
        __asm__ volatile ("ldr q0, [%0]\n str q0, [%1, %2, lsl #4]\n"
                          : : "r"(&r), "r"(table), "r"(i) : "v0", "memory");
    }
    int ok = 1;
    for (uint64_t i = 0; i < 8; i++)
        if (table[i].ptr != 0x7298a40328ULL + i * 0x100 ||
            table[i].index != 0x1a00 + i || table[i].state != i % 6) ok = 0;
    CHECK(ok, "16-byte records stored by index stay aligned");
}

int main(void) {
    test_q();
    test_bhsd();
    test_record_table();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
