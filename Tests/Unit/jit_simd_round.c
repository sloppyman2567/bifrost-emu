/*
 * jit_simd_round.c - rounding shifts, rounding narrowing, compares against zero and element
 * reversal, against a lane model written from the Arm ARM pseudocode.
 *
 *  - SRSHR/URSHR (all lane sizes, every shift 1..lane width, both widths, and the scalar D form):
 *    RShr(x, s) = (x + 2^(s-1)) >> s computed in 128 bits;
 *  - RSHRN/RSHRN2 (every destination size and shift): the unsigned source lane plus the rounding
 *    constant in 128 bits, shifted, then truncated; RSHRN2 keeps the low half of Vd;
 *  - CMGT/CMGE/CMEQ/CMLE/CMLT against zero, REV64/REV32/REV16.
 * Rd is distinct from, equal to Rn, and (for the forms that read Vd) the accumulator. Byte
 * lanes see all 256 values at every shift; wider lanes see edge values and random data.
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

static uint64_t seed = 0x6c8e9cf570932bd5ull;
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

/* Vector k for lane size `bytes`: all byte values, then edge lanes, then random. */
static void gen(V* v, int bytes, unsigned k) {
    static const uint64_t pool[] = {0, 1, 2, 3, 0x7e, 0x7f, 0x80, 0x81, 0xfe, 0xff};
    const uint64_t top = 1ull << (8 * bytes - 1), mask = bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
    if (bytes == 1 && k < 16) {
        for (int j = 0; j < 16; ++j) v->b[j] = (uint8_t)(k * 16 + j);
    } else if (k < 16 + 220) {
        for (int i = 0; i < 16 / bytes; ++i) {
            uint64_t x;
            switch ((k + i) % 8) {
                case 0: x = pool[(k / 8 + i) % 10]; break;
                case 1: x = top; break;
                case 2: x = top - 1; break;
                case 3: x = mask; break;
                case 4: x = mask - (k % 3); break;
                case 5: x = top + ((k / 8 + i) % 5) - 2; break;
                case 6: x = (1ull << ((k / 8 + i) % (8 * bytes))) - 1; break;
                default: x = 1ull << ((k + 3 * i) % (8 * bytes)); break;
            }
            lane_put(v, i, bytes, x & mask);
        }
    } else {
        for (int i = 0; i < 16; ++i) v->b[i] = (uint8_t)rnd();
    }
}

static void dump(const char* label, const V* v) {
    for (int i = 0; i < 16; ++i) printf("%02x", v->b[i]);
    printf(" %s\n", label);
}

static unsigned checks, fails;

static void compare(const char* name, int size, int q, int arg, int alias, unsigned k,
                    const V* n, const V* d, const V* got, const V* want) {
    ++checks;
    if (memcmp(got, want, sizeof *got) == 0 || ++fails > 8) return;
    printf("FAIL %s size=%d q=%d arg=%d alias=%d k=%u\n", name, size, q, arg, alias, k);
    dump("n", n); dump("d", d); dump("got", got); dump("want", want);
}

/* ---- SRSHR / URSHR ---------------------------------------------------------------------- */
static void round_shift_model(int u, int size, int q, int scalar, int s, const V* n, V* out) {
    const int bytes = 1 << size;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < (scalar ? 1 : (q ? 16 : 8) / bytes); ++i) {
        const uint64_t raw = lane_get(n, i, bytes);
        __int128 x = u ? (__int128)raw : (__int128)sext(raw, bytes);
        x += (__int128)1 << (s - 1);
        lane_put(out, i, bytes, (uint64_t)(x >> s));
    }
}

static void test_round_shift(void) {
    for (int u = 0; u < 2; ++u)
    for (int size = 0; size < 4; ++size)
    for (int q = 0; q < 2; ++q) {
        if (size == 3 && !q) continue;
        const int bits = 8 << size;
        for (int s = 1; s <= bits; ++s)
        for (int alias = 0; alias < 2; ++alias) {
            const unsigned rd = alias ? 0 : 2;
            const uint32_t insn = (uint32_t)q << 30 | (uint32_t)u << 29 | 0x0f000000u |
                (uint32_t)(2 * bits - s) << 16 | 0x04u << 11 | 1u << 10 | rd;
            stub_fn fn = install(insn, rd);
            const unsigned total = size == 0 ? 16 + 40 : 16 + 90;
            for (unsigned k = 0; k < total; ++k) {
                V n, d, got, want;
                gen(&n, 1 << size, size == 0 ? k : 16 + k * 2);
                for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0x33 + i);
                fn(&n, &n, &d, &got);
                round_shift_model(u, size, q, 0, s, &n, &want);
                compare(u ? "urshr" : "srshr", size, q, s, alias, k, &n, &d, &got, &want);
            }
        }
    }
    /* scalar D: srshr d, d, #s / urshr d, d, #s (immh = 1xxx) */
    for (int u = 0; u < 2; ++u)
    for (int s = 1; s <= 64; ++s)
    for (int alias = 0; alias < 2; ++alias) {
        const unsigned rd = alias ? 0 : 2;
        const uint32_t insn = 1u << 30 | (uint32_t)u << 29 | 0x1f000000u |
            (uint32_t)(128 - s) << 16 | 0x04u << 11 | 1u << 10 | rd;
        stub_fn fn = install(insn, rd);
        for (unsigned k = 0; k < 120; ++k) {
            V n, d, got, want;
            gen(&n, 8, 16 + k);
            for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0x33 + i);
            fn(&n, &n, &d, &got);
            round_shift_model(u, 3, 0, 1, s, &n, &want);
            compare(u ? "urshr d" : "srshr d", 3, 0, s, alias, k, &n, &d, &got, &want);
        }
    }
}

/* ---- RSHRN / RSHRN2 --------------------------------------------------------------------- */
static void rshrn_model(int size, int q, int s, const V* n, const V* d, V* out) {
    const int dest = 1 << size, src = 2 * dest;          /* bytes */
    memset(out, 0, sizeof *out);
    if (q) memcpy(out, d, 8);
    for (int i = 0; i < 8 / dest; ++i) {
        unsigned __int128 x = lane_get(n, i, src);
        x += (unsigned __int128)1 << (s - 1);
        lane_put(out, (q ? 8 / dest : 0) + i, dest, (uint64_t)(x >> s));
    }
}

static void test_rshrn(void) {
    for (int size = 0; size < 3; ++size)
    for (int q = 0; q < 2; ++q) {
        const int bits = 8 << size;
        for (int s = 1; s <= bits; ++s)
        for (int alias = 0; alias < 2; ++alias) {
            const unsigned rd = alias ? 0 : 2;
            const uint32_t insn = (uint32_t)q << 30 | 0x0f000000u | (uint32_t)(2 * bits - s) << 16 |
                0x11u << 11 | 1u << 10 | rd;
            stub_fn fn = install(insn, rd);
            for (unsigned k = 0; k < 100; ++k) {
                V n, d, got, want, d_eff;
                gen(&n, 2 << size, 16 + k * 2);
                for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0x71 + 3 * i);
                d_eff = alias ? n : d;
                fn(&n, &n, &d, &got);
                rshrn_model(size, q, s, &n, &d_eff, &want);
                compare(q ? "rshrn2" : "rshrn", size, q, s, alias, k, &n, &d_eff, &got, &want);
            }
        }
    }
}

/* ---- compares against zero, reversal ------------------------------------------------------ */
enum { CMGT, CMGE, CMEQ, CMLE, CMLT, REV64, REV32, REV16 };
typedef struct { const char* name; int op, u, opcode, max_size; } Misc;
static const Misc misc[] = {
    {"cmgt #0", CMGT, 0, 0x08, 3}, {"cmge #0", CMGE, 1, 0x08, 3}, {"cmeq #0", CMEQ, 0, 0x09, 3},
    {"cmle #0", CMLE, 1, 0x09, 3}, {"cmlt #0", CMLT, 0, 0x0a, 3},
    {"rev64", REV64, 0, 0x00, 2}, {"rev32", REV32, 1, 0x00, 1}, {"rev16", REV16, 0, 0x01, 0},
};

static void misc_model(const Misc* o, int size, int q, const V* n, V* out) {
    const int bytes = 1 << size;
    memset(out, 0, sizeof *out);
    if (o->op >= REV64) {
        const int container = o->op == REV64 ? 8 : o->op == REV32 ? 4 : 2, count = container / bytes;
        for (int i = 0; i < (q ? 16 : 8) / bytes; ++i)
            lane_put(out, i, bytes, lane_get(n, (i / count) * count + count - 1 - i % count, bytes));
        return;
    }
    for (int i = 0; i < (q ? 16 : 8) / bytes; ++i) {
        const int64_t a = sext(lane_get(n, i, bytes), bytes);
        int hit = 0;
        switch (o->op) {
            case CMGT: hit = a > 0; break;
            case CMGE: hit = a >= 0; break;
            case CMEQ: hit = a == 0; break;
            case CMLE: hit = a <= 0; break;
            default:   hit = a < 0; break;
        }
        lane_put(out, i, bytes, hit ? ~0ull : 0);
    }
}

static void test_misc(void) {
    for (unsigned oi = 0; oi < sizeof misc / sizeof misc[0]; ++oi) {
        const Misc* o = &misc[oi];
        for (int size = 0; size <= o->max_size; ++size)
        for (int q = 0; q < 2; ++q) {
            if (size == 3 && !q) continue;
            for (int alias = 0; alias < 2; ++alias) {
                const unsigned rd = alias ? 0 : 2;
                const uint32_t insn = (uint32_t)q << 30 | (uint32_t)o->u << 29 | 0x0e000000u |
                    (uint32_t)size << 22 | 0x10u << 17 | (uint32_t)o->opcode << 12 | 2u << 10 | rd;
                stub_fn fn = install(insn, rd);
                for (unsigned k = 0; k < 16 + 220 + 100; ++k) {
                    V n, d, got, want;
                    gen(&n, 1 << size, k);
                    for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0x10 + i);
                    fn(&n, &n, &d, &got);
                    misc_model(o, size, q, &n, &want);
                    compare(o->name, size, q, 0, alias, k, &n, &d, &got, &want);
                }
            }
        }
    }
}


/* ---- hot-loop kernel: the forms chained on live vector registers ------------------------------ */
#define ITER 2500
static uint8_t in_a[ITER][16], in_b[ITER][16], out_1[ITER][16], out_2[ITER][16];

static void kernel(void) {
    const uint8_t *a = &in_a[0][0], *b = &in_b[0][0]; uint8_t *o1 = &out_1[0][0], *o2 = &out_2[0][0];
    long n = ITER;
    __asm__ volatile(
        "1:\n\t"
        "ldr q16, [%0], #16\n\tldr q17, [%1], #16\n\t"
        "urshr v18.8h, v16.8h, #6\n\t"
        "srshr v19.8h, v17.8h, #3\n\t"
        "urshr v20.16b, v16.16b, #3\n\t"
        "srshr v21.16b, v17.16b, #5\n\t"
        "srshr v22.4s, v16.4s, #11\n\t"
        "urshr v23.2d, v17.2d, #20\n\t"
        "rshrn v24.8b, v18.8h, #4\n\t"
        "rshrn2 v24.16b, v19.8h, #3\n\t"
        "cmgt v25.8h, v19.8h, #0\n\t"
        "cmlt v26.16b, v21.16b, #0\n\t"
        "cmle v27.4s, v22.4s, #0\n\t"
        "cmge v28.2d, v23.2d, #0\n\t"
        "rev32 v29.16b, v24.16b\n\t"
        "rev16 v30.16b, v20.16b\n\t"
        "rev64 v31.8h, v18.8h\n\t"
        "eor v0.16b, v24.16b, v25.16b\n\teor v0.16b, v0.16b, v26.16b\n\teor v0.16b, v0.16b, v27.16b\n\t"
        "eor v1.16b, v28.16b, v29.16b\n\teor v1.16b, v1.16b, v30.16b\n\teor v1.16b, v1.16b, v31.16b\n\t"
        "str q0, [%2], #16\n\tstr q1, [%3], #16\n\t"
        "subs %4, %4, #1\n\tb.ne 1b"
        : "+r"(a), "+r"(b), "+r"(o1), "+r"(o2), "+r"(n) :
        : "memory", "cc", "v0", "v1", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24",
          "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

static void eor(V* dst, const V* a, const V* b) { for (int i = 0; i < 16; ++i) dst->b[i] = a->b[i] ^ b->b[i]; }

static void test_kernel(void) {
    for (int i = 0; i < ITER; ++i)
        for (int j = 0; j < 16; ++j) { in_a[i][j] = (uint8_t)rnd(); in_b[i][j] = (uint8_t)rnd(); }
    kernel();
    V z;
    memset(&z, 0, sizeof z);
    for (int i = 0; i < ITER; ++i) {
        V v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, t, x0, x1;
        memcpy(&v16, in_a[i], 16); memcpy(&v17, in_b[i], 16);
        round_shift_model(1, 1, 1, 0, 6, &v16, &v18);
        round_shift_model(0, 1, 1, 0, 3, &v17, &v19);
        round_shift_model(1, 0, 1, 0, 3, &v16, &v20);
        round_shift_model(0, 0, 1, 0, 5, &v17, &v21);
        round_shift_model(0, 2, 1, 0, 11, &v16, &v22);
        round_shift_model(1, 3, 1, 0, 20, &v17, &v23);
        rshrn_model(0, 0, 4, &v18, &z, &v24);
        rshrn_model(0, 1, 3, &v19, &v24, &t); v24 = t;
        misc_model(&misc[0], 1, 1, &v19, &v25);          /* cmgt #0, .8h */
        misc_model(&misc[4], 0, 1, &v21, &v26);          /* cmlt #0, .16b */
        misc_model(&misc[3], 2, 1, &v22, &v27);          /* cmle #0, .4s */
        misc_model(&misc[1], 3, 1, &v23, &v28);          /* cmge #0, .2d */
        misc_model(&misc[6], 0, 1, &v24, &v29);          /* rev32 .16b */
        misc_model(&misc[7], 0, 1, &v20, &v30);          /* rev16 .16b */
        misc_model(&misc[5], 1, 1, &v18, &v31);          /* rev64 .8h */
        eor(&x0, &v24, &v25); eor(&x0, &x0, &v26); eor(&x0, &x0, &v27);
        eor(&x1, &v28, &v29); eor(&x1, &x1, &v30); eor(&x1, &x1, &v31);
        ++checks;
        if (memcmp(out_1[i], &x0, 16) || memcmp(out_2[i], &x1, 16)) {
            if (++fails <= 8) printf("FAIL kernel iteration %d\n", i);
        }
    }
}

int main(void) {
    stub = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    test_round_shift();
    test_rshrn();
    test_misc();
    test_kernel();
    if (fails) { printf("jit_simd_round: %u of %u checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_round: %u checks passed\n", checks);
    return 0;
}
