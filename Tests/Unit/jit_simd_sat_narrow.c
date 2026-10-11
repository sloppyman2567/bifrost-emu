// JPEG IDCT regression: saturating/rounded narrowing consumes a full 128-bit
// source. Check both destination halves, aliasing, rounding overflow and QC.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

typedef void (*shift_fn)(const uint8_t *, uint8_t *, uint64_t *, int, int, uint64_t);

#define STR1(x) #x
#define STR(x) STR1(x)
#define ASM_SHIFT(OP, DST, SRC, SHIFT, RD) \
    __asm__ volatile ( \
        "msr fpsr, %[seed]\n" \
        "ldr q0, [%[s]]\n" \
        "movi v1.16b, #0x5a\n" \
        OP " v" RD "." DST ", v0." SRC ", #" STR(SHIFT) "\n" \
        "str q" RD ", [%[o]]\n" \
        "mrs %[f], fpsr\n" \
        : [f] "=r"(*fpsr) \
        : [s] "r"(src), [o] "r"(out), [seed] "r"(seed) \
        : "v0", "v1", "memory")

#define DEFINE_FN(NAME, OP, LOW, HIGH, SRC, SHIFT) \
static void NAME(const uint8_t *src, uint8_t *out, uint64_t *fpsr, \
                 int q, int alias, uint64_t seed) { \
    if (q) { \
        if (alias) { ASM_SHIFT(OP "2", HIGH, SRC, SHIFT, "0"); } \
        else       { ASM_SHIFT(OP "2", HIGH, SRC, SHIFT, "1"); } \
    } else { \
        if (alias) { ASM_SHIFT(OP, LOW, SRC, SHIFT, "0"); } \
        else       { ASM_SHIFT(OP, LOW, SRC, SHIFT, "1"); } \
    } \
}

#define DEFINE_WIDTHS(OP) \
    DEFINE_FN(OP##_8_1,  #OP, "8b", "16b", "8h", 1) \
    DEFINE_FN(OP##_8_8,  #OP, "8b", "16b", "8h", 8) \
    DEFINE_FN(OP##_16_1, #OP, "4h", "8h",  "4s", 1) \
    DEFINE_FN(OP##_16_16,#OP, "4h", "8h",  "4s", 16) \
    DEFINE_FN(OP##_32_1, #OP, "2s", "4s",  "2d", 1) \
    DEFINE_FN(OP##_32_32,#OP, "2s", "4s",  "2d", 32)

DEFINE_WIDTHS(sqshrn)
DEFINE_WIDTHS(sqrshrn)
DEFINE_WIDTHS(uqshrn)
DEFINE_WIDTHS(uqrshrn)
DEFINE_WIDTHS(sqshrun)
DEFINE_WIDTHS(sqrshrun)
DEFINE_WIDTHS(rshrn)

static void run(const char *name, shift_fn fn, int db, int shift,
                int unsrc, int undst, int round, int saturating) {
    const int sb = 2 * db, lanes = 128 / sb;
    const uint64_t mask = sb == 64 ? UINT64_MAX : (UINT64_C(1) << sb) - 1;
    const uint64_t values[8] = {
        0, 1, mask, mask >> 1, UINT64_C(1) << (sb - 1),
        (UINT64_C(1) << db) - 1,
        ((UINT64_C(1) << (db - 1)) - 1) << shift, 3
    };
    for (int batch = 0; batch < 4; batch++) {
        uint8_t src[16];
        for (int i = 0; i < lanes; i++) {
            uint64_t v = values[(batch * lanes + i) % 8];
            memcpy(src + i * (sb / 8), &v, sb / 8);
        }
        for (int q = 0; q < 2; q++) for (int alias = 0; alias < 2; alias++) {
            uint8_t out[16], expected[16] = {0};
            uint64_t fpsr = 0, seed = (batch & 1) ? UINT64_C(1) << 27 : 0;
            int sat = 0;
            if (q) {
                if (alias) memcpy(expected, src, 8);
                else memset(expected, 0x5a, 8);
            }
            for (int i = 0; i < lanes; i++) {
                uint64_t raw = 0;
                memcpy(&raw, src + i * (sb / 8), sb / 8);
                __int128 v = raw;
                if (!unsrc && (raw & (UINT64_C(1) << (sb - 1))))
                    v -= (__int128)1 << sb;
                if (round) v += (__int128)1 << (shift - 1);
                v >>= shift;
                __int128 lo = undst ? 0 : -((__int128)1 << (db - 1));
                __int128 hi = ((__int128)1 << (undst ? db : db - 1)) - 1;
                if (saturating && v < lo) { v = lo; sat = 1; }
                if (saturating && v > hi) { v = hi; sat = 1; }
                uint64_t narrowed = (uint64_t)v;
                memcpy(expected + q * 8 + i * (db / 8), &narrowed, db / 8);
            }
            fn(src, out, &fpsr, q, alias, seed);
            checks++;
            if (memcmp(out, expected, 16) ||
                !!(fpsr & (UINT64_C(1) << 27)) != (!!seed || sat)) {
                printf("FAIL %s db=%d shift=%d q=%d alias=%d batch=%d\n",
                       name, db, shift, q, alias, batch);
                failures++;
            }
        }
    }
}

#define RUN_WIDTHS(OP, US, UD, R, SAT) do { \
    run(#OP, OP##_8_1, 8, 1, US, UD, R, SAT); \
    run(#OP, OP##_8_8, 8, 8, US, UD, R, SAT); \
    run(#OP, OP##_16_1, 16, 1, US, UD, R, SAT); \
    run(#OP, OP##_16_16, 16, 16, US, UD, R, SAT); \
    run(#OP, OP##_32_1, 32, 1, US, UD, R, SAT); \
    run(#OP, OP##_32_32, 32, 32, US, UD, R, SAT); \
} while (0)

int main(void) {
    RUN_WIDTHS(sqshrn,   0, 0, 0, 1);
    RUN_WIDTHS(sqrshrn,  0, 0, 1, 1);
    RUN_WIDTHS(uqshrn,   1, 1, 0, 1);
    RUN_WIDTHS(uqrshrn,  1, 1, 1, 1);
    RUN_WIDTHS(sqshrun,  0, 1, 0, 1);
    RUN_WIDTHS(sqrshrun, 0, 1, 1, 1);
    RUN_WIDTHS(rshrn,    1, 1, 1, 0);
    printf("%d checks passed, %d failures\n", checks - failures, failures);
    return failures ? 1 : 0;
}
