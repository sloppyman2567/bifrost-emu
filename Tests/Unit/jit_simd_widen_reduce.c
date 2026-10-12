/*
 * jit_simd_widen_reduce.c — across-lane reductions and widening integer ops.
 *
 * SpongeBob's gameplay fallback profile ran UADDLV, UMINV, UMLAL, UADDW,
 * UADDLP and UADALP through the interpreter. These run through specialized
 * native helpers. Every element size and Q form is checked against a C
 * reference (signed and unsigned, upper-half "2" forms, accumulation), with
 * Vd preloaded so results must fully overwrite or accumulate into it. The
 * expected results also pass under QEMU.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t b[16]; } V;
static int checks, fails;
static uint64_t lane(const V* v, unsigned i, unsigned bytes) {
    uint64_t x = 0; memcpy(&x, v->b + i * bytes, bytes); return x;
}
static int64_t slane(const V* v, unsigned i, unsigned bytes) {
    uint64_t x = lane(v, i, bytes); unsigned sh = 64 - 8 * bytes;
    return sh ? (int64_t)(x << sh) >> sh : (int64_t)x;
}
static void put(V* v, unsigned i, unsigned bytes, uint64_t x) { memcpy(v->b + i * bytes, &x, bytes); }
static void expect(const char* name, unsigned form, const V* got, const V* want) {
    ++checks;
    if (memcmp(got, want, 16)) {
        printf("FAIL %s form=%u got=", name, form);
        for (int i = 15; i >= 0; --i) printf("%02x", got->b[i]);
        printf(" want=");
        for (int i = 15; i >= 0; --i) printf("%02x", want->b[i]);
        printf("\n");
        ++fails;
    }
}
/* Run one encoding with v0=n, v1=m, v2=d (preloaded) and return v2. */
#define RUN(word) do { __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%3]\n" \
    ".inst " #word "\n str q2, [%0]" :: "r"(out), "r"(n), "r"(m), "r"(d) \
    : "v0", "v1", "v2", "memory"); } while (0)

/* Across lanes: Vd = op over Vn's lanes (rd=2, rn=0). */
static void across(unsigned form, const V* n, const V* m, const V* d, V* out) {
    switch (form) {
#define A(i, w) case i: RUN(w); break;
        A(0, 0x0e303802) A(1, 0x4e303802) A(2, 0x0e703802) A(3, 0x4e703802) A(4, 0x4eb03802)  /* SADDLV */
        A(5, 0x2e303802) A(6, 0x6e303802) A(7, 0x2e703802) A(8, 0x6e703802) A(9, 0x6eb03802)  /* UADDLV */
        A(10, 0x0e30a802) A(11, 0x4e30a802) A(12, 0x0e70a802) A(13, 0x4e70a802) A(14, 0x4eb0a802) /* SMAXV */
        A(15, 0x2e30a802) A(16, 0x6e30a802) A(17, 0x2e70a802) A(18, 0x6e70a802) A(19, 0x6eb0a802) /* UMAXV */
        A(20, 0x0e31a802) A(21, 0x4e31a802) A(22, 0x0e71a802) A(23, 0x4e71a802) A(24, 0x4eb1a802) /* SMINV */
        A(25, 0x2e31a802) A(26, 0x6e31a802) A(27, 0x2e71a802) A(28, 0x6e71a802) A(29, 0x6eb1a802) /* UMINV */
#undef A
    }
}
static V across_ref(unsigned form, const V* n) {
    static const unsigned sz[5] = {1, 1, 2, 2, 4}, q[5] = {0, 1, 0, 1, 1};
    const unsigned kind = form / 5, bytes = sz[form % 5], count = (q[form % 5] ? 16 : 8) / bytes;
    const int sign = kind == 0 || kind == 2 || kind == 4;
    V r; memset(&r, 0, sizeof(r));
    int64_t acc = sign ? slane(n, 0, bytes) : (int64_t)lane(n, 0, bytes);
    for (unsigned i = 1; i < count; ++i) {
        int64_t x = sign ? slane(n, i, bytes) : (int64_t)lane(n, i, bytes);
        if (kind < 2) acc += x;
        else if (kind < 4) acc = (sign ? x > acc : (uint64_t)x > (uint64_t)acc) ? x : acc;
        else acc = (sign ? x < acc : (uint64_t)x < (uint64_t)acc) ? x : acc;
    }
    put(&r, 0, kind < 2 ? bytes * 2 : bytes, (uint64_t)acc);
    return r;
}

/* Three different / pairwise widening ops: rd=2, rn=0, rm=1. */
enum { ADDL, SUBL, ADDW, SUBW, MLAL, MLSL, MULL, ADDLP, ADALP };
static const struct { uint32_t base; int op; } wide_ops[] = {
    {0x0e210002, ADDL}, {0x0e212002, SUBL}, {0x0e211002, ADDW}, {0x0e213002, SUBW},
    {0x0e218002, MLAL}, {0x0e21a002, MLSL}, {0x0e21c002, MULL},
    {0x0e202802, ADDLP}, {0x0e206802, ADALP},
};
static void wide(uint32_t word, const V* n, const V* m, const V* d, V* out) {
    switch (word) {
#define W(w) case w: RUN(w); break;
#define SIZES(b) W(b) W(b | 0x00400000) W(b | 0x00800000) \
                 W(b | 0x40000000) W(b | 0x40400000) W(b | 0x40800000) \
                 W(b | 0x20000000) W(b | 0x20400000) W(b | 0x20800000) \
                 W(b | 0x60000000) W(b | 0x60400000) W(b | 0x60800000)
        SIZES(0x0e210002) SIZES(0x0e212002) SIZES(0x0e211002) SIZES(0x0e213002)
        SIZES(0x0e218002) SIZES(0x0e21a002) SIZES(0x0e21c002)
        SIZES(0x0e202802) SIZES(0x0e206802)
#undef SIZES
#undef W
    }
}
static V wide_ref(int op, uint32_t word, const V* n, const V* m, const V* d) {
    const int q = (word >> 30) & 1, u = (word >> 29) & 1;
    const unsigned bytes = 1u << ((word >> 22) & 3), wbytes = bytes * 2;
    V r;
    if (op == ADDLP || op == ADALP) {
        const unsigned count = (q ? 16 : 8) / wbytes;
        memset(&r, 0, sizeof(r));
        for (unsigned i = 0; i < count; ++i) {
            int64_t a = u ? (int64_t)lane(n, 2 * i, bytes) : slane(n, 2 * i, bytes);
            int64_t b = u ? (int64_t)lane(n, 2 * i + 1, bytes) : slane(n, 2 * i + 1, bytes);
            uint64_t s = (uint64_t)(a + b) + (op == ADALP ? lane(d, i, wbytes) : 0);
            put(&r, i, wbytes, s);
        }
        return r;
    }
    const unsigned count = 8 / bytes, half = q ? count : 0;
    for (unsigned i = 0; i < count; ++i) {
        int64_t nb = u ? (int64_t)lane(n, half + i, bytes) : slane(n, half + i, bytes);
        int64_t mb = u ? (int64_t)lane(m, half + i, bytes) : slane(m, half + i, bytes);
        uint64_t nw = lane(n, i, wbytes), dw = lane(d, i, wbytes), x;
        switch (op) {
            case ADDL: x = (uint64_t)(nb + mb); break;
            case SUBL: x = (uint64_t)(nb - mb); break;
            case ADDW: x = nw + (uint64_t)mb; break;
            case SUBW: x = nw - (uint64_t)mb; break;
            case MLAL: x = dw + (uint64_t)nb * (uint64_t)mb; break;
            case MLSL: x = dw - (uint64_t)nb * (uint64_t)mb; break;
            default:   x = (uint64_t)nb * (uint64_t)mb; break;
        }
        put(&r, i, wbytes, x);
    }
    return r;
}

int main(void) {
    for (int pattern = 0; pattern < 24; ++pattern) {
        V n, m, d, out, want;
        for (int i = 0; i < 16; ++i) {
            n.b[i] = (uint8_t)(pattern < 4 ? (pattern == 0 ? 0 : pattern == 1 ? 0xff : pattern == 2 ? 0x80 : 0x7f)
                               : i * 37 + pattern * 101 + (i * i) * 13);
            m.b[i] = (uint8_t)(pattern < 4 ? (pattern == 3 ? 0x80 : 0xff) : i * 59 + pattern * 17 + 0x35);
            d.b[i] = (uint8_t)(0xa5 ^ (i * 29 + pattern));
        }
        for (unsigned form = 0; form < 30; ++form) {
            memset(&out, 0, sizeof(out));
            across(form, &n, &m, &d, &out);
            want = across_ref(form, &n);
            expect("across", form, &out, &want);
        }
        for (unsigned o = 0; o < sizeof(wide_ops) / sizeof(wide_ops[0]); ++o)
            for (unsigned variant = 0; variant < 12; ++variant) {
                static const uint32_t bits[12] = {0, 0x00400000, 0x00800000, 0x40000000, 0x40400000,
                    0x40800000, 0x20000000, 0x20400000, 0x20800000, 0x60000000, 0x60400000, 0x60800000};
                const uint32_t word = wide_ops[o].base | bits[variant];
                memset(&out, 0, sizeof(out));
                wide(word, &n, &m, &d, &out);
                want = wide_ref(wide_ops[o].op, word, &n, &m, &d);
                expect("wide", word, &out, &want);
            }
    }
    if (fails) { printf("jit_simd_widen_reduce: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_widen_reduce: %d checks passed\n", checks);
    return 0;
}
