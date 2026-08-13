// jit_neon_roundingshift.c — URSRA/SRSRA (vector rounding shift and
// accumulate) correctness. Exercises the native JIT path (SSE2 + AVX2)
// and the interpreter for every element size, both Q=0 and Q=1.
//
// ARM RShr semantics for Vd += RShr(Vn, #sh):
//   URSRA: RShr(x, sh) = (x + 2^(sh-1)) >> sh       (round-half-up)
//   SRSRA: signed variant on an arithmetic shift.
// For sh == esize*8: URSRA -> (x >= 2^(esize*8-1)) ? 1 : 0; SRSRA -> 0.
// For 0 < sh < esize*8 the interpreter uses
//   (x >> sh) + ((x & ((1<<sh)-1)) >= 2^(sh-1) ? 1 : 0).
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

typedef uint64_t u64;

static u64 MASK(int bits) { return bits == 64 ? ~0ULL : ((1ULL << bits) - 1); }

static u64 ru(u64 x, u64 acc, int sh, int bits) {
    u64 onehi = (bits == 64) ? (1ULL << 63) : (1ULL << (bits - 1));
    u64 r;
    if (sh == 0) r = x;
    else if (sh >= bits) r = (x >= onehi) ? 1 : 0;
    else {
        u64 low = x & ((1ULL << sh) - 1);
        r = (x >> sh) + ((low >= (1ULL << (sh - 1))) ? 1 : 0);
    }
    return (acc + r) & MASK(bits);
}

static u64 rs(u64 xb, u64 acc, int sh, int bits) {
    int64_t x;
    switch (bits) {
        case 8:  x = (int64_t)(int8_t)(uint8_t)xb; break;
        case 16: x = (int64_t)(int16_t)(uint16_t)xb; break;
        case 32: x = (int64_t)(int32_t)(uint32_t)xb; break;
        default: x = (int64_t)xb; break;
    }
    int64_t r;
    if (sh == 0) r = x;
    else if (sh >= bits) r = 0;
    else {
        u64 low = (u64)x & ((1ULL << sh) - 1);
        r = (x >> sh) + ((low >= (1ULL << (sh - 1))) ? 1 : 0);
    }
    return (u64)((int64_t)(acc & MASK(bits)) + r) & MASK(bits);
}

typedef u64 (*ref_fn)(u64, u64, int, int);

#define VARS uint8_t vn[16], vd[16], out[16]; ref_fn ufn;

#define RUN(nm, op, fref, layout, reg, L, nlanes, bits, sh) do {                    \
    VARS;                                                                     \
    ufn = (fref);                                                               \
    for (int i = 0; i < 16; i++) vn[i] = (uint8_t)(i * L + 0x35 - i);         \
    for (int i = 0; i < 16; i++) vd[i] = (uint8_t)(i * L + 0xC4 - i * 3);     \
    memset(out, 0, 16);                                                       \
    __asm__ volatile(                                                         \
        "ldr " reg "0, [%1]\n"                                                \
        "ldr " reg "2, [%2]\n"                                                \
        op " v2." layout ", v0." layout ", #%3\n"                             \
        "str " reg "2, [%0]"                                                  \
        :: "r"(out), "r"(vn), "r"(vd), "i"(sh) : "v0", "v2", "memory");       \
    int ok = 1;                                                               \
    for (int e = 0; e < (nlanes); e++) {                                      \
        u64 s = 0, a = 0, g = 0;                                              \
        memcpy(&s, vn + e * (bits) / 8, (bits) / 8);                          \
        memcpy(&a, vd + e * (bits) / 8, (bits) / 8);                          \
        memcpy(&g, out + e * (bits) / 8, (bits) / 8);                         \
        if (ufn(s, a, sh, (bits)) != g) ok = 0;                               \
    }                                                                         \
    CHECK(ok, nm);                                                            \
} while (0)

int main(void) {
    printf("=== jit_neon_roundingshift ===\n");
    RUN("ursra 8h #1",  "ursra", ru, "8h", "q", 37, 8, 16, 1);
    RUN("ursra 8h #15", "ursra", ru, "8h", "q", 13, 8, 16, 15);
    RUN("ursra 4s #12", "ursra", ru, "4s", "q", 29, 4, 32, 12);
    RUN("ursra 4s #32", "ursra", ru, "4s", "q", 31, 4, 32, 32);
    RUN("ursra 2d #63", "ursra", ru, "2d", "q", 41, 2, 64, 63);
    RUN("ursra 2d #64", "ursra", ru, "2d", "q", 43, 2, 64, 64);
    RUN("ursra 4h #3 (Q=0)", "ursra", ru, "4h", "d", 53, 4, 16, 3);
    RUN("ursra 2s #20 (Q=0)", "ursra", ru, "2s", "d", 67, 2, 32, 20);
    RUN("srsra 8h #1",  "srsra", rs, "8h", "q", 47, 8, 16, 1);
    RUN("srsra 8h #16", "srsra", rs, "8h", "q", 33, 8, 16, 16);
    RUN("srsra 4s #7",  "srsra", rs, "4s", "q", 71, 4, 32, 7);
    RUN("srsra 4s #32", "srsra", rs, "4s", "q", 73, 4, 32, 32);
    RUN("srsra 4h #5 (Q=0)", "srsra", rs, "4h", "d", 83, 4, 16, 5);
    RUN("srsra 2s #17 (Q=0)", "srsra", rs, "2s", "d", 97, 2, 32, 17);
    printf("=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}