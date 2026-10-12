/*
 * jit_simd_fp_lanes.c — FP special values and FPCR.DN for native FP lanes.
 *
 * Covers FP pairwise, compare-with-zero, vector FRINT, half-precision
 * FCVTL/FCVTN and byte shift-by-immediate forms (which used to step the
 * interpreter), vector FP arithmetic, and scalar 2-source, FABD, FSQRT, FRINT,
 * FCVT and FMADD-family operations (whose native results used x86 NaN rules).
 * Each runs over special inputs (quiet/signalling NaNs, signed zeros,
 * infinities, ties, subnormals, values outside the half range) with FPCR.DN
 * clear and set. Vd (v2) is preloaded; it is also the FMADD addend. Results
 * are hashed per encoding and compared with a table produced by QEMU 11.1
 * (`jit_simd_fp_lanes gen` prints it).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FP(X) \
    X(0x2e21f402, 4) X(0x6e21f402, 4) X(0x6e61f402, 8) /* FMAXP */   \
    X(0x2ea1f402, 4) X(0x6ea1f402, 4) X(0x6ee1f402, 8) /* FMINP */   \
    X(0x2e21c402, 4) X(0x6e21c402, 4) X(0x6e61c402, 8) /* FMAXNMP */ \
    X(0x2ea1c402, 4) X(0x6ea1c402, 4) X(0x6ee1c402, 8) /* FMINNMP */ \
    X(0x0ea0c802, 4) X(0x4ea0c802, 4) X(0x4ee0c802, 8) /* FCMGT #0 */ \
    X(0x0ea0d802, 4) X(0x4ea0d802, 4) X(0x4ee0d802, 8) /* FCMEQ #0 */ \
    X(0x0ea0e802, 4) X(0x4ea0e802, 4) X(0x4ee0e802, 8) /* FCMLT #0 */ \
    X(0x2ea0c802, 4) X(0x6ea0c802, 4) X(0x6ee0c802, 8) /* FCMGE #0 */ \
    X(0x2ea0d802, 4) X(0x6ea0d802, 4) X(0x6ee0d802, 8) /* FCMLE #0 */ \
    X(0x0e218802, 4) X(0x4e218802, 4) X(0x4e618802, 8) /* FRINTN */  \
    X(0x2e218802, 4) X(0x6e218802, 4) X(0x6e618802, 8) /* FRINTA */  \
    X(0x0e219802, 4) X(0x4e219802, 4) X(0x4e619802, 8) /* FRINTM */  \
    X(0x2e219802, 4) X(0x6e219802, 4) X(0x6e619802, 8) /* FRINTX */  \
    X(0x0ea19802, 4) X(0x4ea19802, 4) X(0x4ee19802, 8) /* FRINTZ */  \
    X(0x2ea19802, 4) X(0x6ea19802, 4) X(0x6ee19802, 8) /* FRINTI */  \
    X(0x0ea18802, 4) X(0x4ea18802, 4) X(0x4ee18802, 8) /* FRINTP */  \
    X(0x0e217802, 2) X(0x4e217802, 2) /* FCVTL(2) from half */        \
    X(0x0e216802, 4) X(0x4e216802, 4) /* FCVTN(2) to half */
/* Byte shifts: SHL USHR SSHR USRA SSRA SLI SRI URSRA SRSRA, four amounts, Q. */
#define AMOUNTS(X, b) X((b) | 0x00080000, 1) X((b) | 0x00090000, 1) \
    X((b) | 0x000c0000, 1) X((b) | 0x000f0000, 1) X((b) | 0x40080000, 1) \
    X((b) | 0x40090000, 1) X((b) | 0x400c0000, 1) X((b) | 0x400f0000, 1)
#define BYTES(X) AMOUNTS(X, 0x0f005402) AMOUNTS(X, 0x2f000402) AMOUNTS(X, 0x0f000402) \
    AMOUNTS(X, 0x2f001402) AMOUNTS(X, 0x0f001402) AMOUNTS(X, 0x2f005402) \
    AMOUNTS(X, 0x2f004402) AMOUNTS(X, 0x2f003402) AMOUNTS(X, 0x0f003402)
/* Vector arithmetic (.4s, .2d): FADD FSUB FMUL FDIV FMAX FMIN FMAXNM FMINNM
   FMLA FMLS FABD FSQRT. */
#define VEC(X) \
    X(0x4e21d402, 4) X(0x4e61d402, 8) X(0x4ea1d402, 4) X(0x4ee1d402, 8) \
    X(0x6e21dc02, 4) X(0x6e61dc02, 8) X(0x6e21fc02, 4) X(0x6e61fc02, 8) \
    X(0x4e21f402, 4) X(0x4e61f402, 8) X(0x4ea1f402, 4) X(0x4ee1f402, 8) \
    X(0x4e21c402, 4) X(0x4e61c402, 8) X(0x4ea1c402, 4) X(0x4ee1c402, 8) \
    X(0x4e21cc02, 4) X(0x4e61cc02, 8) X(0x4ea1cc02, 4) X(0x4ee1cc02, 8) \
    X(0x6ea1d402, 4) X(0x6ee1d402, 8) X(0x6ea1f802, 4) X(0x6ee1f802, 8)
/* Scalar S and D: FMUL FDIV FADD FSUB FMAX FMIN FMAXNM FMINNM FNMUL, FABD,
   FSQRT, FRINTN/P/M/Z/A/X/I, FMADD FMSUB FNMADD FNMSUB (Ra = v2), FCVT,
   plus vector FCVTXN(2). */
#define SCALAR_SD(X, d, t) \
    X(0x1e210802 | (d), t) X(0x1e211802 | (d), t) X(0x1e212802 | (d), t) \
    X(0x1e213802 | (d), t) X(0x1e214802 | (d), t) X(0x1e215802 | (d), t) \
    X(0x1e216802 | (d), t) X(0x1e217802 | (d), t) X(0x1e218802 | (d), t) \
    X(0x7ea1d402 | (d), t) X(0x1e21c002 | (d), t) \
    X(0x1e244002 | (d), t) X(0x1e24c002 | (d), t) X(0x1e254002 | (d), t) \
    X(0x1e25c002 | (d), t) X(0x1e264002 | (d), t) X(0x1e274002 | (d), t) \
    X(0x1e27c002 | (d), t) \
    X(0x1f010802 | (d), t) X(0x1f018802 | (d), t) X(0x1f210802 | (d), t) \
    X(0x1f218802 | (d), t)
#define SCALAR(X) SCALAR_SD(X, 0, 4) SCALAR_SD(X, 0x00400000, 8) \
    X(0x1e624002, 8) X(0x1e22c002, 4) \
    X(0x2e616802, 8) X(0x6e616802, 8) /* FCVTXN(2): round to odd */
#define ALL(X) FP(X) BYTES(X) VEC(X) SCALAR(X)

typedef struct { uint8_t b[16]; } V;
#define RUN(word) __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%3]\n" \
    ".inst " #word "\n str q2, [%0]" :: "r"(out), "r"(n), "r"(m), "r"(d) \
    : "v0", "v1", "v2", "memory")
static void run(uint32_t word, const V* n, const V* m, const V* d, V* out) {
    switch (word) {
#define CASE(w, t) case (w): RUN(w); break;
        ALL(CASE)
#undef CASE
    }
}
static const struct { uint32_t word; unsigned type; } forms[] = {
#define ENTRY(w, t) {(w), (t)},
    ALL(ENTRY)
#undef ENTRY
};
#define NFORMS (sizeof(forms) / sizeof(forms[0]))

static const uint32_t f32s[] = {
    0x00000000, 0x80000000, 0x3fc00000, 0x40200000, 0xc0200000, 0x3f000000,
    0xbf000000, 0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001, 0xffc00002,
    0x7f7fffff, 0x00000001, 0x501502f9, 0xc0700000, 0x477fe000, 0x477ff000,
    0x33800000, 0x387fc000, 0x4b000001, 0xcb7fffff,
};
static const uint64_t f64s[] = {
    0, 0x8000000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
    0xc004000000000000ull, 0x3fe0000000000000ull, 0xbfe0000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000001ull,
    0x7ff0000000000001ull, 0xfff8000000000002ull, 0x7fefffffffffffffull,
    0x0000000000000001ull, 0x4330000000000001ull, 0xc00e000000000000ull,
    0x433fffffffffffffull, 0xc3e0000000000000ull,
};
static const uint16_t f16s[] = {
    0x0000, 0x8000, 0x3c00, 0x3e00, 0xc100, 0x7c00, 0xfc00, 0x7e01, 0x7c01,
    0xfe02, 0x7bff, 0x0001, 0x83ff, 0x0400, 0x3555, 0xd2a3,
};

static void fill(V* v, unsigned type, unsigned k) {
    for (unsigned lane = 0; lane < 16 / (type ? type : 1); ++lane) {
        const unsigned sel = k * 7 + lane * (k % 5 + 1) + k / 3;
        if (type == 4) memcpy(v->b + 4 * lane, &f32s[sel % (sizeof(f32s) / 4)], 4);
        else if (type == 8) memcpy(v->b + 8 * lane, &f64s[sel % (sizeof(f64s) / 8)], 8);
        else if (type == 2) memcpy(v->b + 2 * lane, &f16s[sel % (sizeof(f16s) / 2)], 2);
        else v->b[lane] = (uint8_t)(sel * 37 + k * 11 + (k & 1 ? 0x80 : 0));
    }
}
static void set_fpcr(uint64_t value) { __asm__ volatile("msr fpcr, %0" :: "r"(value)); }

static const uint32_t expected[] = {
    0xacdf1a49, 0xeadaf0a3, 0x5282dec5, 0x8c08a5ed, 0x58852183, 0x1a060649,
    0x93488d3c, 0xc9ff8fd0, 0xcfab6fac, 0x264afa14, 0xd7ff4b84, 0xb2c69de0,
    0xc4eb62b5, 0x0d178525, 0x7ccee875, 0x37cc6a4d, 0x91ef81ad, 0x843d4a25,
    0xabb9da25, 0x0f30d00d, 0x8f2986d5, 0x249a273d, 0xe0d86f0d, 0xa4bbaad5,
    0x9c8eeead, 0x696361f5, 0x61404d35, 0x9d114d68, 0xb4f5734e, 0xb3060e69,
    0x7e4217a4, 0xe76d9606, 0x82ce63c9, 0x72c656ac, 0xc4f12432, 0x8400e31d,
    0x9d114d68, 0xb4f5734e, 0xb3060e69, 0x0a51752c, 0x0c1bb352, 0x7b727e0d,
    0x9d114d68, 0xb4f5734e, 0xb3060e69, 0xa0842a74, 0x85665ca6, 0xb45a2491,
    0x4a3ffd01, 0xbad7236d, 0xf07bbca9, 0xfb3df179, 0x50d4abc5, 0xf790e705,
    0x445d4dc5, 0x119f85c5, 0xca567ac5, 0x2b47eec5, 0xf13357c5, 0x32bc4dc5,
    0x99fb0dc5, 0xd4b8d5c5, 0xd2dc7d91, 0x6151eb25, 0x99fb0dc5, 0x3cbd25c5,
    0xfa378165, 0x3cce2345, 0xd6ed2fc5, 0xd6ed2fc5, 0xa51f9551, 0xb5a55225,
    0xe63b29c5, 0xe63b29c5, 0xb7a0abc5, 0x77918145, 0x8f2743c5, 0x3c8e78ed,
    0x7de8f181, 0x5c9b74c5, 0x75de05c5, 0x8a768911, 0x0825c1f1, 0x79f6bae5,
    0xe16cdd1d, 0xe16cdd1d, 0xba47c301, 0x5b04e8c5, 0xeb1a3539, 0xeb1a3539,
    0xc205f711, 0x5d3929e5, 0x50d4abc5, 0x5dd68965, 0xc7615285, 0x4be3d5c5,
    0xca567ac5, 0x422a2ec5, 0x3ad73f05, 0x4751e4c5, 0x8f2743c5, 0x0d7feaad,
    0x58473531, 0x300f8425, 0x75de05c5, 0xbc20c295, 0x87f56525, 0x6f565f45,
    0x3c8e78ed, 0xb4ceea49, 0x292af215, 0xed696965, 0x8a768911, 0x9a4d827d,
    0xa4114129, 0x99d87645, 0x8f2743c5, 0x77b6cc45, 0x69206235, 0x190b4a65,
    0x75de05c5, 0x29401dfd, 0xd4307ca9, 0xb239b145, 0xbcb046d3, 0x2f6ae13c,
    0x4a601723, 0x47e22f04, 0xed42b95f, 0xc63a2418, 0x11cef60b, 0xb1a54710,
    0x1d721067, 0x29506b8c, 0x78f3e8cb, 0x989289c4, 0x8e1c7ac0, 0x85603b78,
    0x03073124, 0x9e286284, 0x04a71eb7, 0x9257177c, 0x6c25352b, 0xa4977ad0,
    0xc4d4d0a3, 0x14e41004, 0x40d6dff6, 0xd242eb85, 0x5c99bdc5, 0xf9e515c5,
    0x99811cc5, 0x2da11cc5, 0x99811cc5, 0xcc820cc5, 0x99811cc5, 0xcc820cc5,
    0xa84e7dc5, 0x99811cc5, 0xb946abc5, 0x8d4209c5, 0x8d4209c5, 0x8d4209c5,
    0x8d4209c5, 0x8d4209c5, 0x8d4209c5, 0x8d4209c5, 0x9f8da14c, 0x185c5d4c,
    0xa32e17cc, 0xc8b67fcc, 0xd0389c64, 0x692238b0, 0x4bc8ccdc, 0xbfd1ff44,
    0x0006e37c, 0xbd827e58, 0x8ccb6805, 0x550c3a51, 0xafb20f64, 0x4275c944,
    0xcdbb3dc4, 0xfa9b7054, 0x849f4a44, 0xe032cf74, 0x7600d814, 0x32ace7cc,
    0xfa9b7054, 0xfa9b7054, 0x5deca8cc, 0x941e87b8, 0x90113a4c, 0x01193f38,
    0x257cf405, 0xc489f445, 0xc1cd06ad, 0x3f6d0f7d,
};

int main(int argc, char** argv) {
    const int gen = argc > 1 && !strcmp(argv[1], "gen");
    int fails = 0;
    for (unsigned f = 0; f < NFORMS; ++f) {
        uint32_t h = 2166136261u;
        for (unsigned dn = 0; dn < 2; ++dn) {
            set_fpcr(dn ? 1u << 25 : 0);
            for (unsigned k = 0; k < 96; ++k) {
                V n, m, d, out;
                fill(&n, forms[f].type, k);
                fill(&m, forms[f].type, k + 17);
                for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0xa5 ^ (i * 29 + k));
                memset(&out, 0, sizeof(out));
                run(forms[f].word, &n, &m, &d, &out);
                for (int i = 0; i < 16; ++i) h = (h ^ out.b[i]) * 16777619u;
            }
        }
        set_fpcr(0);
        if (gen) printf("0x%08x,%s", h, f % 6 == 5 ? "\n" : " ");
        else if (sizeof(expected) / 4 != NFORMS || h != expected[f]) {
            printf("FAIL %08x hash=%08x\n", forms[f].word, h);
            ++fails;
        }
    }
    if (gen) { printf("\n"); return 0; }
    if (fails) { printf("jit_simd_fp_lanes: %d of %u forms FAILED\n", fails, (unsigned)NFORMS); return 1; }
    printf("jit_simd_fp_lanes: %u forms passed\n", (unsigned)NFORMS);
    return 0;
}
