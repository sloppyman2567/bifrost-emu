/*
 * jit_simd_fp_lanes.c — native FP pairwise, compare-with-zero, vector FRINT,
 * half-precision FCVTL/FCVTN and byte shift-by-immediate forms.
 *
 * These encodings used to step the interpreter. Each one runs over special
 * inputs (quiet/signalling NaNs, signed zeros, infinities, ties, subnormals,
 * values outside the half range) with FPCR.DN clear and set; Vd is preloaded
 * for the merging forms. Results are hashed per encoding and compared with a
 * table produced by QEMU 11.1 (`jit_simd_fp_lanes gen` prints it). FP
 * pairwise forms skip FPCR.DN=1, which neither engine models for arithmetic.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FP(X) \
    X(0x2e21f402, 0x14) X(0x6e21f402, 0x14) X(0x6e61f402, 0x18) /* FMAXP */   \
    X(0x2ea1f402, 0x14) X(0x6ea1f402, 0x14) X(0x6ee1f402, 0x18) /* FMINP */   \
    X(0x2e21c402, 0x14) X(0x6e21c402, 0x14) X(0x6e61c402, 0x18) /* FMAXNMP */ \
    X(0x2ea1c402, 0x14) X(0x6ea1c402, 0x14) X(0x6ee1c402, 0x18) /* FMINNMP */ \
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
#define ALL(X) FP(X) BYTES(X)

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
/* FP arithmetic ignores FPCR.DN in both engines (see Docs/JIT.md); 0x10
   types run with DN clear only. */
#define NO_DN 0x10u

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
    0x0bad369c, 0x929ddb81, 0x837d975a, 0x8f064e05, 0xe0b7bec6, 0xb73ade1b,
    0x651e4d61, 0x27a99d82, 0xceb4cb41, 0x2305e718, 0x3ebe3ad1, 0xdb0266b4,
    0xfe43be3d, 0x118c7275, 0xadc7fcf5, 0xbe547225, 0xa9b8ded5, 0xf0da54e5,
    0xf58561bd, 0x0a106355, 0x7c6f7a55, 0x3ed97c9d, 0x80c97985, 0xa9d54e15,
    0xf3ce081d, 0xaa9e4c65, 0xf7c7ad75, 0xea02ab8e, 0xd308cdca, 0x2432b0fd,
    0xa1593ab2, 0x92aa542e, 0x6401e0d5, 0x1b72831a, 0x4bbd8cf6, 0xb24591ad,
    0xea02ab8e, 0xd308cdca, 0x2432b0fd, 0x7b188b1a, 0x46a33286, 0x111ec1ed,
    0xea02ab8e, 0xd308cdca, 0x2432b0fd, 0x2d5833c6, 0x5c71201a, 0xc2cc04c5,
    0xd7120011, 0x7f904e95, 0xe1b0b7bd, 0x36aedeed, 0x2146d645, 0xc72961c5,
    0x19461fc5, 0x8f89a7c5, 0x8f2d8b45, 0x633bca45, 0x987ce8c5, 0xe5b735c5,
    0xf5ebd5c5, 0x151542a9, 0xdf2353a5, 0x66fba265, 0xf5ebd5c5, 0x01b7cc29,
    0xcc2ce879, 0x2b5b84a5, 0x6968b8e1, 0x6968b8e1, 0xb37626e5, 0x170ebd65,
    0x98a45761, 0x98a45761, 0x49f91599, 0xbf64d2a5, 0xef782bc5, 0xf7a74a5d,
    0x00323801, 0xcb814445, 0x1f1af8c5, 0x458f6e75, 0x4ff49c8d, 0x0628d1c5,
    0x18bec0a5, 0x18bec0a5, 0x79423fc1, 0xa5d14d45, 0xfab60f05, 0xfab60f05,
    0x15e23f2d, 0x0000d1c5, 0x2146d645, 0x2e7b83f5, 0x6618ca45, 0x422424c5,
    0x8f2d8b45, 0xf3c68065, 0xff6bc385, 0xe796f8c5, 0xef782bc5, 0xa7f09729,
    0x81f23d65, 0x5093ce65, 0x1f1af8c5, 0xa6e87539, 0x20077a59, 0x18ad43a5,
    0xf7a74a5d, 0x83b9f639, 0x3240f1b5, 0x7af93fd5, 0x458f6e75, 0x3f27cb35,
    0x7c4ff179, 0x48a5f8a5, 0xef782bc5, 0x5dc8a111, 0x876a0355, 0xc8dbc7d5,
    0x1f1af8c5, 0x3b46c4e5, 0x3f317fb9, 0x701c8aa5,
};

int main(int argc, char** argv) {
    const int gen = argc > 1 && !strcmp(argv[1], "gen");
    int fails = 0;
    for (unsigned f = 0; f < NFORMS; ++f) {
        uint32_t h = 2166136261u;
        for (unsigned dn = 0; dn < (forms[f].type & NO_DN ? 1u : 2u); ++dn) {
            set_fpcr(dn ? 1u << 25 : 0);
            for (unsigned k = 0; k < 48; ++k) {
                V n, m, d, out;
                fill(&n, forms[f].type & 15, k);
                fill(&m, forms[f].type & 15, k + 17);
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
