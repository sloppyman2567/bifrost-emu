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

// ── Every narrowing form inside a block of other vector ops ──────────────────
// The narrowed register feeds an EOR and an ADD in the same block, so the JIT keeps it in a host register
// (the vector cache) instead of writing it to the guest state first. Reference: the Arm ARM pseudocode of
// XTN/SQXTN/UQXTN/SQXTUN, lane by lane, including FPSR.QC and the preserved low half of the "2" forms.
typedef void (*xtn_run)(const uint8_t *src, const uint8_t *old, uint8_t *dv, uint8_t *sum, uint64_t *fpsr);
struct xtn_form { const char *name; int op, esize, q, alias; xtn_run run; };

#define XTN_FORM(TAG, OP, ES, Q, ALIAS, DV, INS) \
static void run_##TAG(const uint8_t *src, const uint8_t *old, uint8_t *dv, uint8_t *sum, uint64_t *fpsr) { \
    uint64_t f; \
    __asm__ volatile ( \
        "msr fpsr, xzr\n" \
        "ldr q2, [%[s]]\n" \
        "ldr q3, [%[d]]\n" \
        INS "\n" \
        "eor v5.16b, v" #DV ".16b, v2.16b\n" \
        "add v6.4s, v" #DV ".4s, v5.4s\n" \
        "str q" #DV ", [%[o1]]\n" \
        "str q6, [%[o2]]\n" \
        "mrs %[f], fpsr\n" \
        : [f]"=r"(f) \
        : [s]"r"(src), [d]"r"(old), [o1]"r"(dv), [o2]"r"(sum) \
        : "v2","v3","v5","v6","memory"); \
    *fpsr = f; \
}
#define XTN_LIST(XTN_FORM_ENTRY) \
    XTN_FORM_ENTRY(xtn_8b, 0, 2, 0, 0, 3, "xtn v3.8b, v2.8h") \
    XTN_FORM_ENTRY(xtn_8b_alias, 0, 2, 0, 1, 2, "xtn v2.8b, v2.8h") \
    XTN_FORM_ENTRY(xtn2_16b, 0, 2, 1, 0, 3, "xtn2 v3.16b, v2.8h") \
    XTN_FORM_ENTRY(xtn2_16b_alias, 0, 2, 1, 1, 2, "xtn2 v2.16b, v2.8h") \
    XTN_FORM_ENTRY(xtn_4h, 0, 4, 0, 0, 3, "xtn v3.4h, v2.4s") \
    XTN_FORM_ENTRY(xtn_4h_alias, 0, 4, 0, 1, 2, "xtn v2.4h, v2.4s") \
    XTN_FORM_ENTRY(xtn2_8h, 0, 4, 1, 0, 3, "xtn2 v3.8h, v2.4s") \
    XTN_FORM_ENTRY(xtn2_8h_alias, 0, 4, 1, 1, 2, "xtn2 v2.8h, v2.4s") \
    XTN_FORM_ENTRY(xtn_2s, 0, 8, 0, 0, 3, "xtn v3.2s, v2.2d") \
    XTN_FORM_ENTRY(xtn_2s_alias, 0, 8, 0, 1, 2, "xtn v2.2s, v2.2d") \
    XTN_FORM_ENTRY(xtn2_4s, 0, 8, 1, 0, 3, "xtn2 v3.4s, v2.2d") \
    XTN_FORM_ENTRY(xtn2_4s_alias, 0, 8, 1, 1, 2, "xtn2 v2.4s, v2.2d") \
    XTN_FORM_ENTRY(sqxtun_8b, 1, 2, 0, 0, 3, "sqxtun v3.8b, v2.8h") \
    XTN_FORM_ENTRY(sqxtun_8b_alias, 1, 2, 0, 1, 2, "sqxtun v2.8b, v2.8h") \
    XTN_FORM_ENTRY(sqxtun2_16b, 1, 2, 1, 0, 3, "sqxtun2 v3.16b, v2.8h") \
    XTN_FORM_ENTRY(sqxtun2_16b_alias, 1, 2, 1, 1, 2, "sqxtun2 v2.16b, v2.8h") \
    XTN_FORM_ENTRY(sqxtun_4h, 1, 4, 0, 0, 3, "sqxtun v3.4h, v2.4s") \
    XTN_FORM_ENTRY(sqxtun_4h_alias, 1, 4, 0, 1, 2, "sqxtun v2.4h, v2.4s") \
    XTN_FORM_ENTRY(sqxtun2_8h, 1, 4, 1, 0, 3, "sqxtun2 v3.8h, v2.4s") \
    XTN_FORM_ENTRY(sqxtun2_8h_alias, 1, 4, 1, 1, 2, "sqxtun2 v2.8h, v2.4s") \
    XTN_FORM_ENTRY(sqxtun_2s, 1, 8, 0, 0, 3, "sqxtun v3.2s, v2.2d") \
    XTN_FORM_ENTRY(sqxtun_2s_alias, 1, 8, 0, 1, 2, "sqxtun v2.2s, v2.2d") \
    XTN_FORM_ENTRY(sqxtun2_4s, 1, 8, 1, 0, 3, "sqxtun2 v3.4s, v2.2d") \
    XTN_FORM_ENTRY(sqxtun2_4s_alias, 1, 8, 1, 1, 2, "sqxtun2 v2.4s, v2.2d") \
    XTN_FORM_ENTRY(sqxtn_8b, 2, 2, 0, 0, 3, "sqxtn v3.8b, v2.8h") \
    XTN_FORM_ENTRY(sqxtn_8b_alias, 2, 2, 0, 1, 2, "sqxtn v2.8b, v2.8h") \
    XTN_FORM_ENTRY(sqxtn2_16b, 2, 2, 1, 0, 3, "sqxtn2 v3.16b, v2.8h") \
    XTN_FORM_ENTRY(sqxtn2_16b_alias, 2, 2, 1, 1, 2, "sqxtn2 v2.16b, v2.8h") \
    XTN_FORM_ENTRY(sqxtn_4h, 2, 4, 0, 0, 3, "sqxtn v3.4h, v2.4s") \
    XTN_FORM_ENTRY(sqxtn_4h_alias, 2, 4, 0, 1, 2, "sqxtn v2.4h, v2.4s") \
    XTN_FORM_ENTRY(sqxtn2_8h, 2, 4, 1, 0, 3, "sqxtn2 v3.8h, v2.4s") \
    XTN_FORM_ENTRY(sqxtn2_8h_alias, 2, 4, 1, 1, 2, "sqxtn2 v2.8h, v2.4s") \
    XTN_FORM_ENTRY(sqxtn_2s, 2, 8, 0, 0, 3, "sqxtn v3.2s, v2.2d") \
    XTN_FORM_ENTRY(sqxtn_2s_alias, 2, 8, 0, 1, 2, "sqxtn v2.2s, v2.2d") \
    XTN_FORM_ENTRY(sqxtn2_4s, 2, 8, 1, 0, 3, "sqxtn2 v3.4s, v2.2d") \
    XTN_FORM_ENTRY(sqxtn2_4s_alias, 2, 8, 1, 1, 2, "sqxtn2 v2.4s, v2.2d") \
    XTN_FORM_ENTRY(uqxtn_8b, 3, 2, 0, 0, 3, "uqxtn v3.8b, v2.8h") \
    XTN_FORM_ENTRY(uqxtn_8b_alias, 3, 2, 0, 1, 2, "uqxtn v2.8b, v2.8h") \
    XTN_FORM_ENTRY(uqxtn2_16b, 3, 2, 1, 0, 3, "uqxtn2 v3.16b, v2.8h") \
    XTN_FORM_ENTRY(uqxtn2_16b_alias, 3, 2, 1, 1, 2, "uqxtn2 v2.16b, v2.8h") \
    XTN_FORM_ENTRY(uqxtn_4h, 3, 4, 0, 0, 3, "uqxtn v3.4h, v2.4s") \
    XTN_FORM_ENTRY(uqxtn_4h_alias, 3, 4, 0, 1, 2, "uqxtn v2.4h, v2.4s") \
    XTN_FORM_ENTRY(uqxtn2_8h, 3, 4, 1, 0, 3, "uqxtn2 v3.8h, v2.4s") \
    XTN_FORM_ENTRY(uqxtn2_8h_alias, 3, 4, 1, 1, 2, "uqxtn2 v2.8h, v2.4s") \
    XTN_FORM_ENTRY(uqxtn_2s, 3, 8, 0, 0, 3, "uqxtn v3.2s, v2.2d") \
    XTN_FORM_ENTRY(uqxtn_2s_alias, 3, 8, 0, 1, 2, "uqxtn v2.2s, v2.2d") \
    XTN_FORM_ENTRY(uqxtn2_4s, 3, 8, 1, 0, 3, "uqxtn2 v3.4s, v2.2d") \
    XTN_FORM_ENTRY(uqxtn2_4s_alias, 3, 8, 1, 1, 2, "uqxtn2 v2.4s, v2.2d")

XTN_LIST(XTN_FORM)
#undef XTN_FORM
#define XTN_FORM(TAG, OP, ES, Q, ALIAS, DV, INS) { #TAG, OP, ES, Q, ALIAS, run_##TAG },
static const struct xtn_form xtn_forms[] = { XTN_LIST(XTN_FORM) };

// Narrow the 128-bit source: op 0 XTN, 1 SQXTUN, 2 SQXTN, 3 UQXTN; esize is the source lane size in bytes.
static int xtn_reference(int op, int esize, const uint8_t *src, uint8_t *res) {
    const int lanes = 16 / esize, bits = esize * 4;
    int saturated = 0;
    for (int i = 0; i < lanes; i++) {
        uint64_t u = 0;
        for (int b = 0; b < esize; b++) u |= (uint64_t)src[i * esize + b] << (8 * b);
        const int64_t s = esize == 8 ? (int64_t)u : esize == 4 ? (int32_t)u : (int16_t)u;
        const uint64_t umax = (1ull << bits) - 1;
        const int64_t smax = (int64_t)(umax >> 1), smin = -smax - 1;
        uint64_t r;
        if (op == 0) r = u & umax;
        else if (op == 1) { if (s < 0) { r = 0; saturated = 1; } else if ((uint64_t)s > umax) { r = umax; saturated = 1; } else r = (uint64_t)s; }
        else if (op == 2) { int64_t v = s; if (v > smax) { v = smax; saturated = 1; } else if (v < smin) { v = smin; saturated = 1; } r = (uint64_t)v & umax; }
        else { if (u > umax) { r = umax; saturated = 1; } else r = u; }
        for (int b = 0; b < esize / 2; b++) res[i * (esize / 2) + b] = (uint8_t)(r >> (8 * b));
    }
    return saturated;
}

static uint64_t xtn_state = 0x2545F4914F6CDD1Dull;
static uint64_t xtn_next(void) { xtn_state ^= xtn_state << 13; xtn_state ^= xtn_state >> 7; xtn_state ^= xtn_state << 17; return xtn_state; }

static void test_all_forms_in_a_vector_block(void) {
    int bad = 0;
    for (unsigned f = 0; f < sizeof(xtn_forms) / sizeof(xtn_forms[0]); f++) {
        const struct xtn_form *form = &xtn_forms[f];
        for (int round = 0; round < 40; round++) {
            uint8_t src[16], old[16], dv[16], sum[16], res[8], want[16], want_sum[16];
            uint64_t fpsr;
            for (int i = 0; i < 16; i += 8) { uint64_t a = xtn_next(), b = xtn_next(); memcpy(src + i, &a, 8); memcpy(old + i, &b, 8); }
            // Directed lanes: both limits, zero, minus one and values just past the narrow range.
            const uint64_t edge[] = {0, ~0ull, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull};
            if (round < 14) { uint64_t e = edge[round]; for (int i = 0; i < 16; i += 8) memcpy(src + i, &e, 8); src[0] ^= (uint8_t)round; }
            const int sat = xtn_reference(form->op, form->esize, src, res);
            const uint8_t *keep = form->alias ? src : old;
            memset(want, 0, sizeof(want));
            if (form->q) { memcpy(want, keep, 8); memcpy(want + 8, res, 8); } else memcpy(want, res, 8);
            for (int i = 0; i < 16; i += 4) {
                uint32_t w, s32, x, r;
                memcpy(&w, want + i, 4); memcpy(&s32, (form->alias ? want : src) + i, 4);  // aliased: v2 is the result itself
                x = w ^ s32; r = w + x; memcpy(want_sum + i, &r, 4);
            }
            form->run(src, old, dv, sum, &fpsr);
            const int qc = (fpsr >> 27) & 1;
            if (memcmp(dv, want, 16) || memcmp(sum, want_sum, 16) || qc != (form->op ? sat : 0)) {
                if (bad++ < 4) printf("  %s round %d: dv %s sum %s qc %d want %d\n", form->name, round,
                                      memcmp(dv, want, 16) ? "WRONG" : "ok", memcmp(sum, want_sum, 16) ? "WRONG" : "ok", qc, form->op ? sat : 0);
            }
        }
    }
    CHECK(bad == 0, "all 48 narrowing forms, base and *2, aliased and not, inside a vector block");
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
    test_all_forms_in_a_vector_block();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
