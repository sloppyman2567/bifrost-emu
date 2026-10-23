/*
 * jit_simd_pixel.c - integer vector forms used by pixel and media code, against a lane model.
 *
 * SHADD/UHADD, SRHADD/URHADD, SHSUB/UHSUB, CMTST, MUL (bytes), MLA/MLS and SABA/UABA run for
 * every valid element size and both widths, with Rd distinct from, equal to Rn and equal to
 * Rm. Byte lanes see all 65,536 operand pairs; wider lanes see edge values and random data.
 * The model works on sign- or zero-extended 128-bit lanes (the Arm ARM pseudocode computes
 * these in full precision), so it shares no wrap-around shortcut with the JIT's sequences.
 * A Q=0 result must clear the upper half.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint8_t b[16]; } V;
typedef void (*stub_fn)(const V*, const V*, const V*, V*);

static uint32_t* stub;

/* ldr q0,[x0]; ldr q1,[x1]; ldr q2,[x2]; <insn>; str q<rd>,[x3]; ret */
static stub_fn install(uint32_t insn, unsigned rd) {
    stub[0] = 0x3dc00000; stub[1] = 0x3dc00021; stub[2] = 0x3dc00042;
    stub[3] = insn;
    stub[4] = 0x3d800060 | rd;
    stub[5] = 0xd65f03c0;
    __builtin___clear_cache((char*)stub, (char*)(stub + 6));
    return (stub_fn)stub;
}

static uint64_t seed = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed;
}

static uint64_t lane_get(const V* v, int i, int bytes) {
    uint64_t x = 0; memcpy(&x, v->b + i * bytes, bytes); return x;
}
static void lane_put(V* v, int i, int bytes, uint64_t x) { memcpy(v->b + i * bytes, &x, bytes); }
static int64_t sext(uint64_t x, int bytes) {
    const int s = 64 - 8 * bytes; return (int64_t)(x << s) >> s;
}

enum { HADD, RHADD, HSUB, CMTST, MUL, MLA, MLS, ABA };
typedef struct { const char* name; int op, u, opcode, max_size; } Op;
static const Op ops[] = {
    {"shadd", HADD, 0, 0x00, 2}, {"uhadd", HADD, 1, 0x00, 2},
    {"srhadd", RHADD, 0, 0x02, 2}, {"urhadd", RHADD, 1, 0x02, 2},
    {"shsub", HSUB, 0, 0x04, 2}, {"uhsub", HSUB, 1, 0x04, 2},
    {"cmtst", CMTST, 0, 0x11, 3},
    {"mul.b", MUL, 0, 0x13, 0},                  /* wider lanes are SIMD_ARITH */
    {"mla", MLA, 0, 0x12, 2}, {"mls", MLS, 1, 0x12, 2},
    {"saba", ABA, 0, 0x0f, 2}, {"uaba", ABA, 1, 0x0f, 2},
};

static void model(const Op* o, int size, int q, const V* n, const V* m, const V* d, V* out) {
    const int bytes = 1 << size;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < (q ? 16 : 8) / bytes; ++i) {
        const uint64_t ua = lane_get(n, i, bytes), ub = lane_get(m, i, bytes), ud = lane_get(d, i, bytes);
        const __int128 a = o->u ? (__int128)ua : (__int128)sext(ua, bytes);
        const __int128 b = o->u ? (__int128)ub : (__int128)sext(ub, bytes);
        uint64_t r = 0;
        switch (o->op) {
            case HADD:  r = (uint64_t)((a + b) >> 1); break;      /* floor */
            case RHADD: r = (uint64_t)((a + b + 1) >> 1); break;
            case HSUB:  r = (uint64_t)((a - b) >> 1); break;
            case CMTST: r = (ua & ub) ? ~0ull : 0; break;
            case MUL:   r = ua * ub; break;
            case MLA:   r = ud + ua * ub; break;
            case MLS:   r = ud - ua * ub; break;
            case ABA:   r = ud + (uint64_t)(a > b ? a - b : b - a); break;
        }
        lane_put(out, i, bytes, r);
    }
}

static void edge(V* v, int bytes, unsigned k) {
    static const uint64_t pool[] = {0, 1, 2, 3, 0x7e, 0x7f, 0x80, 0x81, 0xfe, 0xff};
    const uint64_t top = 1ull << (8 * bytes - 1), mask = bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
    for (int i = 0; i < 16 / bytes; ++i) {
        uint64_t x;
        switch ((k + i) % 7) {
            case 0: x = pool[(k / 7 + i) % 10]; break;
            case 1: x = top; break;
            case 2: x = top - 1; break;
            case 3: x = mask; break;
            case 4: x = mask - (k % 3); break;
            case 5: x = top + ((k / 7 + i) % 5) - 2; break;
            default: x = 1 + (k % 4); break;
        }
        lane_put(v, i, bytes, x & mask);
    }
}

/* Operand pair k: all byte pairs first, then edge pairs, then random lanes. */
static void inputs(int size, unsigned k, unsigned edges, V* n, V* m) {
    if (size == 0 && k < 4096) {
        for (int j = 0; j < 16; ++j) {
            const unsigned v = k * 16 + j;
            n->b[j] = (uint8_t)(v >> 8); m->b[j] = (uint8_t)v;
        }
    } else if (k < (size == 0 ? 4096u : 0u) + edges) {
        edge(n, 1 << size, k);
        edge(m, 1 << size, k / 5 + 3 * (k % 13));
    } else {
        for (int i = 0; i < 16; ++i) { n->b[i] = (uint8_t)rnd(); m->b[i] = (uint8_t)rnd(); }
    }
}

static void dump(const char* label, const V* v) {
    for (int i = 0; i < 16; ++i) printf("%02x", v->b[i]);
    printf(" %s\n", label);
}


/* ---- hot-loop kernels: the same forms chained on live vector registers --------------------------
 * Inside one block these values stay in the JIT's vector cache and accumulators are carried
 * across iterations, so a wrong read/write set or scratch clash shows up as a mismatch against
 * the model applied step by step. */
#define ITER 2500
static unsigned checks_k;
static uint8_t in_a[ITER][16], in_b[ITER][16], out_1[ITER][16], out_2[ITER][16];

static const Op* find(const char* name) {
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; ++i)
        if (!strcmp(ops[i].name, name)) return &ops[i];
    return 0;
}
static void step(const char* name, int size, V* dst, const V* n, const V* m, const V* d) {
    model(find(name), size, 1, n, m, d, dst);
}
static void eor(V* dst, const V* a, const V* b) { for (int i = 0; i < 16; ++i) dst->b[i] = a->b[i] ^ b->b[i]; }

#define KERNEL(NAME, S) \
    static void NAME(uint8_t acc[3][16]) { \
        const uint8_t *a = &in_a[0][0], *b = &in_b[0][0]; uint8_t *o1 = &out_1[0][0], *o2 = &out_2[0][0]; \
        long n = ITER; \
        __asm__ volatile( \
            "movi v27.16b, #0x11\n\tmovi v28.16b, #0x22\n\tmovi v29.16b, #0x33\n\t" \
            "movi v5.16b, #0xa5\n\tmovi v6.16b, #0x3c\n\tmovi v7.16b, #0xc3\n" \
            "1:\n\t" \
            "ldr q16, [%0], #16\n\tldr q17, [%1], #16\n\t" \
            "urhadd v18." S ", v16." S ", v17." S "\n\t" \
            "uhadd v19." S ", v16." S ", v17." S "\n\t" \
            "shadd v20." S ", v18." S ", v19." S "\n\t" \
            "srhadd v21." S ", v16." S ", v19." S "\n\t" \
            "uhsub v22." S ", v17." S ", v18." S "\n\t" \
            "shsub v23." S ", v20." S ", v21." S "\n\t" \
            "cmtst v24." S ", v22." S ", v23." S "\n\t" \
            MULINSN \
            "uaba v27." S ", v22." S ", v23." S "\n\t" \
            "saba v28." S ", v20." S ", v21." S "\n\t" \
            "mls v29." S ", v22." S ", v23." S "\n\t" \
            "mla v29." S ", v19." S ", v21." S "\n\t" \
            "cmtst v1." S ", v16." S ", v5." S "\n\t"      /* v5-v7 are live into the loop */ \
            "urhadd v3." S ", v16." S ", v6." S "\n\t" \
            "uaba v27." S ", v16." S ", v7." S "\n\t" \
            "eor v26.16b, v25.16b, v20.16b\n\t" \
            "eor v26.16b, v26.16b, v1.16b\n\t" \
            "eor v26.16b, v26.16b, v3.16b\n\t" \
            "str q26, [%2], #16\n\tstr q29, [%3], #16\n\t" \
            "movi v5.16b, #0x5a\n\tmovi v6.16b, #0x69\n\tmovi v7.16b, #0x96\n\t" /* written, not read */ \
            "subs %4, %4, #1\n\tb.ne 1b\n\t" \
            "str q27, [%5]\n\tstr q28, [%5, #16]\n\tstr q29, [%5, #32]" \
            : "+r"(a), "+r"(b), "+r"(o1), "+r"(o2), "+r"(n) : "r"(acc) \
            : "memory", "cc", "v1", "v3", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", \
              "v25", "v26", "v27", "v28", "v29"); \
    }
#define MULINSN "mul v25.16b, v24.16b, v18.16b\n\t"
KERNEL(kernel_b, "16b")
#undef MULINSN
#define MULINSN "mul v25.8h, v24.8h, v18.8h\n\t"
KERNEL(kernel_h, "8h")
#undef MULINSN
#define MULINSN "mul v25.4s, v24.4s, v18.4s\n\t"
KERNEL(kernel_s, "4s")

static unsigned run_kernel(void (*fn)(uint8_t[3][16]), int size, const char* label) {
    for (int i = 0; i < ITER; ++i)
        for (int j = 0; j < 16; ++j) { in_a[i][j] = (uint8_t)rnd(); in_b[i][j] = (uint8_t)rnd(); }
    uint8_t acc[3][16];
    fn(acc);
    V u27, u28, u29, z, c5, c6, c7;
    memset(&u27, 0x11, 16); memset(&u28, 0x22, 16); memset(&u29, 0x33, 16); memset(&z, 0, 16);
    memset(&c5, 0xa5, 16); memset(&c6, 0x3c, 16); memset(&c7, 0xc3, 16);
    unsigned bad = 0;
    for (int i = 0; i < ITER; ++i) {
        V a, b, v18, v19, v20, v21, v22, v23, v24, v25, v26, t;
        memcpy(&a, in_a[i], 16); memcpy(&b, in_b[i], 16);
        step("urhadd", size, &v18, &a, &b, &z);
        step("uhadd", size, &v19, &a, &b, &z);
        step("shadd", size, &v20, &v18, &v19, &z);
        step("srhadd", size, &v21, &a, &v19, &z);
        step("uhsub", size, &v22, &b, &v18, &z);
        step("shsub", size, &v23, &v20, &v21, &z);
        step("cmtst", size, &v24, &v22, &v23, &z);
        step(size == 0 ? "mul.b" : "mla", size, &v25, &v24, &v18, &z);  /* mla onto zero = MUL */
        step("uaba", size, &t, &v22, &v23, &u27); u27 = t;
        step("saba", size, &t, &v20, &v21, &u28); u28 = t;
        step("mls", size, &t, &v22, &v23, &u29); u29 = t;
        step("mla", size, &t, &v19, &v21, &u29); u29 = t;
        V t1, t3;
        if (i == 1) { memset(&c5, 0x5a, 16); memset(&c6, 0x69, 16); memset(&c7, 0x96, 16); }
        step("cmtst", size, &t1, &a, &c5, &z);
        step("urhadd", size, &t3, &a, &c6, &z);
        step("uaba", size, &t, &a, &c7, &u27); u27 = t;
        eor(&v26, &v25, &v20);
        eor(&v26, &v26, &t1);
        eor(&v26, &v26, &t3);
        ++checks_k;
        if (memcmp(out_1[i], &v26, 16) || memcmp(out_2[i], &u29, 16)) {
            if (++bad <= 3) printf("FAIL %s iteration %d\n", label, i);
        }
    }
    if (memcmp(acc[0], &u27, 16) || memcmp(acc[1], &u28, 16) || memcmp(acc[2], &u29, 16)) {
        ++bad; printf("FAIL %s accumulators\n", label);
    }
    return bad;
}

int main(void) {
    stub = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    unsigned checks = 0, fails = 0;
    for (unsigned oi = 0; oi < sizeof ops / sizeof ops[0]; ++oi) {
        const Op* o = &ops[oi];
        for (int size = o->op == MUL ? 0 : 0; size <= o->max_size; ++size)
        for (int q = 0; q < 2; ++q) {
            if (size == 3 && !q) continue;               /* .1d is reserved */
            for (int alias = 0; alias < 3; ++alias) {
                const unsigned rd = alias == 0 ? 2 : alias == 1 ? 0 : 1;
                const uint32_t insn = (uint32_t)q << 30 | (uint32_t)o->u << 29 | 0x0e000000u |
                    (uint32_t)size << 22 | 1u << 21 | 1u << 16 | (uint32_t)o->opcode << 11 | 1u << 10 | rd;
                stub_fn fn = install(insn, rd);
                const unsigned edges = alias == 0 ? 1500 : 500;
                const unsigned total = (size == 0 ? (alias == 0 ? 4096u : 600u) : 0u) + edges + (alias == 0 ? 600 : 200);
                for (unsigned k = 0; k < total; ++k) {
                    V n, m, d, got, want, d_eff;
                    inputs(size, size == 0 && alias ? k * 7 : k, edges, &n, &m);
                    for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0x5a ^ (i * 29 + k));
                    d_eff = alias == 0 ? d : alias == 1 ? n : m;
                    fn(&n, &m, &d, &got);
                    model(o, size, q, &n, &m, &d_eff, &want);
                    ++checks;
                    if (memcmp(&got, &want, sizeof got) && ++fails <= 8) {
                        printf("FAIL %s size=%d q=%d alias=%d k=%u\n", o->name, size, q, alias, k);
                        dump("n", &n); dump("m", &m); dump("d", &d_eff);
                        dump("got", &got); dump("want", &want);
                    }
                }
            }
        }
    }
    fails += run_kernel(kernel_b, 0, "kernel bytes");
    fails += run_kernel(kernel_h, 1, "kernel halfwords");
    fails += run_kernel(kernel_s, 2, "kernel words");
    checks += checks_k;
    if (fails) { printf("jit_simd_pixel: %u of %u checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_pixel: %u checks passed\n", checks);
    return 0;
}
