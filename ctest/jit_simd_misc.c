// jit_simd_misc.c — SIMD 2-REG (CNT/NOT/RBIT/ABS/NEG), CVTF, ADDP,
// TBL/TBX, INS native-JIT coverage. Each test compares against the
// expected value computed in C (the JIT and interp must both match).
#include <stdio.h>
#include <stdlib.h>
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

// ── CNT ─────────────────────────────────────────────────────────────
static void test_cnt_16b(void) {
    uint8_t src[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(0x01 * (i + 1)); exp[i] = 0; }
    src[0] = 0x00; src[1] = 0xFF; src[2] = 0x80; src[3] = 0x01;
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 8; b++) exp[i] += (src[i] >> b) & 1;
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "cnt v1.16b, v0.16b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "cnt v.16b, v.16b");
}

static void test_cnt_8b(void) {
    uint8_t src[8] = {0x00, 0xFF, 0x0F, 0xF0, 0x11, 0x88, 0xAA, 0x55};
    uint8_t out[16], exp[16];
    memset(out, 0xEE, sizeof(out));
    memset(exp, 0, sizeof(exp));
    for (int i = 0; i < 8; i++) {
        int c = 0;
        for (int b = 0; b < 8; b++) c += (src[i] >> b) & 1;
        exp[i] = (uint8_t)c;
    }
    __asm__ volatile (
        "ldr d0, [%[s]]\n"
        "cnt v1.8b, v0.8b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "cnt v.8b, v.8b (Q=0 zeroes v_hi)");
}

// ── NOT (MVN) ────────────────────────────────────────────────────────
static void test_mvn_16b(void) {
    uint8_t src[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(0x40 + i); exp[i] = ~src[i]; }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "mvn v1.16b, v0.16b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "mvn v.16b, v.16b");
}

static void test_mvn_8b(void) {
    uint8_t src[8] = {0x00, 0xFF, 0x0F, 0xF0, 0x12, 0x34, 0x56, 0x78};
    uint8_t out[16], exp[16];
    memset(out, 0xEE, sizeof(out));
    memset(exp, 0, sizeof(exp));
    for (int i = 0; i < 8; i++) exp[i] = ~src[i];
    __asm__ volatile (
        "ldr d0, [%[s]]\n"
        "mvn v1.8b, v0.8b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "mvn v.8b, v.8b (Q=0 zeroes v_hi)");
}

// ── RBIT ─────────────────────────────────────────────────────────────
static void test_rbit_16b(void) {
    uint8_t src[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(0x01 * (i + 1)); exp[i] = 0; }
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 8; b++) exp[i] |= ((src[i] >> b) & 1) << (7 - b);
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "rbit v1.16b, v0.16b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "rbit v.16b, v.16b");
}

static void test_rbit_8b(void) {
    uint8_t src[8] = {0x01, 0x80, 0x0F, 0xF0, 0xAA, 0x55, 0xFF, 0x00};
    uint8_t out[16], exp[16];
    memset(out, 0xEE, sizeof(out));
    memset(exp, 0, sizeof(exp));
    for (int i = 0; i < 8; i++) {
        uint8_t x = src[i]; uint8_t r = 0;
        for (int b = 0; b < 8; b++) r |= ((x >> b) & 1) << (7 - b);
        exp[i] = r;
    }
    __asm__ volatile (
        "ldr d0, [%[s]]\n"
        "rbit v1.8b, v0.8b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "rbit v.8b, v.8b (Q=0 zeroes v_hi)");
}

// ── ABS ──────────────────────────────────────────────────────────────
// Regression note: exp used to be computed in a SEPARATE loop from src
// filling because the scalar JIT's general-case SBFM sign-extension
// (sxtb w) shifted by width-field_width and 64-bit SAR'd — a negative
// byte came back zero-extended (0xf8 = 248 instead of 0xfffffff8),
// miscompiling the shift/xor abs when the self-loop exposed it. Fixed
// 2026-08-15 (shift by 64-field_width so bit 63 holds the sign); a single
// loop now exercises the fix and passes in both modes.
static void test_abs_16b(void) {
    int8_t src[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { int8_t v = (int8_t)(-8 + i); src[i] = v; exp[i] = (int8_t)((v + (v >> 7)) ^ (v >> 7)); }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "abs v1.16b, v0.16b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "abs v.16b, v.16b");
}

static void test_abs_8h(void) {
    int16_t src[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { int16_t v = (int16_t)(-3000 + i * 1000); src[i] = v; exp[i] = (int16_t)((v + (v >> 15)) ^ (v >> 15)); }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "abs v1.8h, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "abs v.8h, v.8h");
}

static void test_abs_4s(void) {
    int32_t src[4], out[4], exp[4];
    for (int i = 0; i < 4; i++) src[i] = (int32_t)(-200000000 + i * 100000000);
    for (int i = 0; i < 4; i++) { int32_t v = src[i]; exp[i] = (v + (v >> 31)) ^ (v >> 31); }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "abs v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "abs v.4s, v.4s");
}

static void test_abs_2d(void) {
    int64_t src[2], out[2], exp[2];
    src[0] = -0x123456789ABCDEF0LL; src[1] = 0x123456789ABCDEF0LL;
    exp[0] = (src[0] < 0) ? -src[0] : src[0];
    exp[1] = (src[1] < 0) ? -src[1] : src[1];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "abs v1.2d, v0.2d\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "abs v.2d, v.2d");
}

// ── NEG ──────────────────────────────────────────────────────────────
static void test_neg_16b(void) {
    int8_t src[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { src[i] = (int8_t)(-8 + i); exp[i] = (int8_t)(0 - src[i]); }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "neg v1.16b, v0.16b\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "neg v.16b, v.16b");
}

static void test_neg_8h(void) {
    int16_t src[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { src[i] = (int16_t)(-3000 + i * 1000); exp[i] = (int16_t)(0 - src[i]); }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "neg v1.8h, v0.8h\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "neg v.8h, v.8h");
}

static void test_neg_4s(void) {
    int32_t src[4], out[4], exp[4];
    for (int i = 0; i < 4; i++) { src[i] = (int32_t)(-200000000 + i * 100000000); exp[i] = 0 - src[i]; }
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "neg v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "neg v.4s, v.4s");
}

static void test_neg_2d(void) {
    int64_t src[2], out[2], exp[2];
    src[0] = 0x123456789ABCDEF0LL; src[1] = -1;
    exp[0] = -src[0]; exp[1] = 1;
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "neg v1.2d, v0.2d\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "neg v.2d, v.2d");
}

// ── ADDP ─────────────────────────────────────────────────────────────
static void test_addp_16b(void) {
    uint8_t a[16], b[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)(0x10 + i); }
    for (int i = 0; i < 8; i++) {
        exp[i] = (uint8_t)(a[2*i] + a[2*i+1]);
        exp[8+i] = (uint8_t)(b[2*i] + b[2*i+1]);
    }
    __asm__ volatile (
        "ldr q0, [%[a]]\n"
        "ldr q1, [%[b]]\n"
        "addp v2.16b, v0.16b, v1.16b\n"
        "str q2, [%[o]]\n"
        :: [a]"r"(a), [b]"r"(b), [o]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "addp v.16b (Vn pairs low, Vm pairs high)");
}

static void test_addp_8b(void) {
    uint8_t a[8], b[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { a[i] = (uint8_t)(0x20 + i); b[i] = (uint8_t)(0x40 + i); }
    for (int i = 0; i < 4; i++) {
        exp[i] = (uint8_t)(a[2*i] + a[2*i+1]);
        exp[4+i] = (uint8_t)(b[2*i] + b[2*i+1]);
    }
    __asm__ volatile (
        "ldr d0, [%[a]]\n"
        "ldr d1, [%[b]]\n"
        "addp v2.8b, v0.8b, v1.8b\n"
        "str d2, [%[o]]\n"
        :: [a]"r"(a), [b]"r"(b), [o]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 8) == 0, "addp v.8b (Vn pairs low, Vm pairs high)");
}

// ── SCVTF / UCVTF ────────────────────────────────────────────────────
static void test_scvtf(void) {
    int32_t src[4] = {0, -1, 0x7FFFFFFF, 0x80000000};
    float out[4], exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (float)src[i];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "scvtf v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "scvtf v.4s, v.4s");
}

static void test_ucvtf(void) {
    uint32_t src[4] = {0, 0xFFFFFFFF, 0x80000000, 0x12345678};
    float out[4], exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (float)src[i];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "ucvtf v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ucvtf v.4s, v.4s (0xFFFFFFFF, 0x80000000)");
}

// ── FCVTZS / FCVTZU ──────────────────────────────────────────────────
static void test_fcvtzs(void) {
    float src[4] = {12.75f, -12.75f, 0.5f, -0.5f};
    int32_t out[4], exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (int32_t)src[i];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzs v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "fcvtzs v.4s (truncate toward zero)");
}

static void test_fcvtzs_special(void) {
    // NaN/±inf → 0 (matches interp). NAN/INFINITY via bit patterns.
    uint32_t src_bits[4] = {0x7FC00000u /*NaN*/, 0x7F800000u /*+inf*/,
                            0xFF800000u /*-inf*/, 0x3F800000u /*1.0f*/};
    uint32_t out[4];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzs v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src_bits), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 1,
          "fcvtzs NaN/±inf → 0");
}

static void test_fcvtzu(void) {
    float src[4] = {12.75f, -12.75f, 0.0f, 3.0f};
    uint32_t out[4], exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (src[i] < 0.0f) ? 0 : (uint32_t)src[i];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzu v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "fcvtzu v.4s (negatives → 0)");
}

static void test_fcvtzu_hi(void) {
    // Values above 2^31 exercise the hi-path (x - 2^31 + 0x80000000).
    // All inputs stay below 2^32: for f >= 2^32 the C (uint32_t) cast is
    // UB (interp gives 0, JIT gives 0x80000000) so we don't test it.
    float src[4] = {2147483648.0f, 3000000000.0f, 4000000000.0f, 1073741824.0f};
    uint32_t out[4], exp[4];
    for (int i = 0; i < 4; i++) exp[i] = (uint32_t)src[i];
    __asm__ volatile (
        "ldr q0, [%[s]]\n"
        "fcvtzu v1.4s, v0.4s\n"
        "str q1, [%[o]]\n"
        :: [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "fcvtzu v.4s (values >= 2^31)");
}

// ── TBL / TBX ────────────────────────────────────────────────────────
static void test_tbl_1reg_16b(void) {
    uint8_t table[16], idx[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { table[i] = (uint8_t)(0xA0 + i); idx[i] = (uint8_t)(31 - i); }
    // idx 0x10-0x1F (16-31) are out of range for a 16-byte table → 0.
    for (int i = 0; i < 16; i++) exp[i] = (idx[i] < 16) ? table[idx[i]] : 0;
    __asm__ volatile (
        "ldr q0, [%[t]]\n"
        "ldr q1, [%[i]]\n"
        "tbl v2.16b, {v0.16b}, v1.16b\n"
        "str q2, [%[o]]\n"
        :: [t]"r"(table), [i]"r"(idx), [o]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "tbl v.16b, {v0.16b}, v1.16b (OOR → 0)");
}

static void test_tbl_1reg_8b(void) {
    uint8_t table[8], idx[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { table[i] = (uint8_t)(0xB0 + i); idx[i] = (uint8_t)(0x00 + i * 2); }
    for (int i = 0; i < 8; i++) exp[i] = (idx[i] < 8) ? table[idx[i]] : 0;
    __asm__ volatile (
        "ldr d0, [%[t]]\n"
        "ldr d1, [%[i]]\n"
        "tbl v2.8b, {v0.16b}, v1.8b\n"
        "str d2, [%[o]]\n"
        :: [t]"r"(table), [i]"r"(idx), [o]"r"(out) : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 8) == 0, "tbl v.8b, {v0.8b}, v1.8b (8-byte table)");
}

static void test_tbl_2reg_16b(void) {
    uint8_t t0[16], t1[16], idx[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { t0[i] = (uint8_t)(0x10 + i); t1[i] = (uint8_t)(0x30 + i); }
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(0x02 * i);  // spread across 0..30
        idx[i] = v;
        exp[i] = (v < 32) ? ((v < 16) ? t0[v] : t1[v - 16]) : 0;
    }
    __asm__ volatile (
        "ldr q0, [%[t0]]\n"
        "ldr q1, [%[t1]]\n"
        "ldr q3, [%[i]]\n"
        "tbl v2.16b, {v0.16b, v1.16b}, v3.16b\n"
        "str q2, [%[o]]\n"
        :: [t0]"r"(t0), [t1]"r"(t1), [i]"r"(idx), [o]"r"(out)
         : "v0","v1","v2","v3","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "tbl v.16b, {v0.16b,v1.16b}, v3.16b");
}

static void test_tbx_1reg_16b(void) {
    uint8_t table[16], idx[16], dst[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { table[i] = (uint8_t)(0xC0 + i); idx[i] = (uint8_t)(0xF0 + i); }
    for (int i = 0; i < 16; i++) dst[i] = (uint8_t)(0x07 + i);
    for (int i = 0; i < 16; i++) exp[i] = (idx[i] < 16) ? table[idx[i]] : dst[i];
    __asm__ volatile (
        "ldr q0, [%[t]]\n"
        "ldr q1, [%[i]]\n"
        "ldr q2, [%[d]]\n"
        "tbx v2.16b, {v0.16b}, v1.16b\n"
        "str q2, [%[o]]\n"
        :: [t]"r"(table), [i]"r"(idx), [d]"r"(dst), [o]"r"(out)
         : "v0","v1","v2","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "tbx v.16b, {v0.16b}, v1.16b (OOR keeps dest)");
}

// ── INS (element, vector) ────────────────────────────────────────────
static void test_ins_b(void) {
    uint8_t src[16], dst[16], out[16], exp[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(0x50 + i); dst[i] = (uint8_t)(0x90 + i); }
    memcpy(exp, dst, 16);
    exp[3] = src[11];  // ins v0.b[3], v1.b[11] → v0 byte 3 = v1 byte 11
    __asm__ volatile (
        "ldr q0, [%[d]]\n"
        "ldr q1, [%[s]]\n"
        "ins v0.b[3], v1.b[11]\n"
        "str q0, [%[o]]\n"
        :: [d]"r"(dst), [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ins v0.b[3], v1.b[11]");
}

static void test_ins_h(void) {
    uint16_t src[8], dst[8], out[8], exp[8];
    for (int i = 0; i < 8; i++) { src[i] = (uint16_t)(0x1000 + i); dst[i] = (uint16_t)(0x9000 + i); }
    memcpy(exp, dst, 16);
    exp[5] = src[2];  // ins v0.h[5], v1.h[2] → v0 halfword 5 = v1 halfword 2
    __asm__ volatile (
        "ldr q0, [%[d]]\n"
        "ldr q1, [%[s]]\n"
        "ins v0.h[5], v1.h[2]\n"
        "str q0, [%[o]]\n"
        :: [d]"r"(dst), [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ins v0.h[5], v1.h[2]");
}

static void test_ins_s(void) {
    uint32_t src[4], dst[4], out[4], exp[4];
    for (int i = 0; i < 4; i++) { src[i] = 0x10000000u + i; dst[i] = 0x90000000u + i; }
    memcpy(exp, dst, 16);
    exp[1] = src[3];  // ins v0.s[1], v1.s[3]
    __asm__ volatile (
        "ldr q0, [%[d]]\n"
        "ldr q1, [%[s]]\n"
        "ins v0.s[1], v1.s[3]\n"
        "str q0, [%[o]]\n"
        :: [d]"r"(dst), [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ins v0.s[1], v1.s[3]");
}

static void test_ins_d(void) {
    uint64_t src[2], dst[2], out[2], exp[2];
    src[0] = 0x123456789ABCDEF0ull; src[1] = 0x0ull;
    dst[0] = 0xAAAAAAAAAAAAAAAAull; dst[1] = 0xBBBBBBBBBBBBBBBBull;
    memcpy(exp, dst, 16);
    exp[1] = src[0];  // ins v0.d[1], v1.d[0] → qword 1 = src qword 0
    __asm__ volatile (
        "ldr q0, [%[d]]\n"
        "ldr q1, [%[s]]\n"
        "ins v0.d[1], v1.d[0]\n"
        "str q0, [%[o]]\n"
        :: [d]"r"(dst), [s]"r"(src), [o]"r"(out) : "v0","v1","memory"
    );
    CHECK(memcmp(out, exp, 16) == 0, "ins v0.d[1], v1.d[0]");
}

int main(void) {
    printf("=== jit_simd_misc ===\n");
    test_cnt_16b();
    test_cnt_8b();
    test_mvn_16b();
    test_mvn_8b();
    test_rbit_16b();
    test_rbit_8b();
    test_abs_16b();
    test_abs_8h();
    test_abs_4s();
    test_abs_2d();
    test_neg_16b();
    test_neg_8h();
    test_neg_4s();
    test_neg_2d();
    test_addp_16b();
    test_addp_8b();
    test_scvtf();
    test_ucvtf();
    test_fcvtzs();
    test_fcvtzs_special();
    test_fcvtzu();
    test_fcvtzu_hi();
    test_tbl_1reg_16b();
    test_tbl_1reg_8b();
    test_tbl_2reg_16b();
    test_tbx_1reg_16b();
    test_ins_b();
    test_ins_h();
    test_ins_s();
    test_ins_d();
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}