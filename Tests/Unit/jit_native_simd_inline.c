/* Inline SIMD_NATIVE_EXTRA bodies: every USHL/SSHL count byte on 32- and
 * 64-bit lanes against the ARM pseudocode, and aliased Vd for each inlined
 * kind against the same instruction with distinct registers. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
typedef struct { uint64_t lo, hi; } V;
static int fail, checks;

static uint64_t shift_lane(uint64_t a, int8_t s, unsigned bits, int is_signed) {
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    const int negative = (a >> (bits - 1)) & 1;
    int64_t sa = (int64_t)(a << (64 - bits)) >> (64 - bits);
    if (s >= 0) return (unsigned)s >= bits ? 0 : (a << s) & mask;
    if ((unsigned)-s >= bits) return is_signed && negative ? mask : 0;
    return is_signed ? (uint64_t)(sa >> -s) & mask : a >> -s;
}

/* .inst words below encode rd=v2, rn=v0, rm=v1. */
#define RUN3(word, n, m, out) __asm__ volatile( \
    "ldr q0, [%1]\n ldr q1, [%2]\n movi v2.16b, #0x5a\n .inst %c3\n str q2, [%0]" \
    :: "r"(&(out)), "r"(&(n)), "r"(&(m)), "i"(word) : "v0", "v1", "v2", "memory")

static void check_shift(const char* name, V got, V n, V m, unsigned bits, int q, int is_signed) {
    V want = {0, 0};
    const unsigned lanes = (q ? 128 : 64) / bits;
    for (unsigned i = 0; i < lanes; ++i) {
        const uint64_t* nw = i * bits < 64 ? &n.lo : &n.hi;
        const uint64_t* mw = i * bits < 64 ? &m.lo : &m.hi;
        uint64_t* ww = i * bits < 64 ? &want.lo : &want.hi;
        const unsigned pos = (i * bits) % 64;
        const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
        const uint64_t a = (*nw >> pos) & mask;
        const int8_t s = (int8_t)(*mw >> pos);
        *ww |= shift_lane(a, s, bits, is_signed) << pos;
    }
    ++checks;
    if (memcmp(&got, &want, sizeof got)) {
        printf("FAIL %s n=%016llx:%016llx m=%016llx:%016llx got=%016llx:%016llx want=%016llx:%016llx\n",
               name, (unsigned long long)n.hi, (unsigned long long)n.lo,
               (unsigned long long)m.hi, (unsigned long long)m.lo,
               (unsigned long long)got.hi, (unsigned long long)got.lo,
               (unsigned long long)want.hi, (unsigned long long)want.lo);
        ++fail;
    }
}

static void shifts(void) {
    static const uint64_t values[] = {
        0x8000000180000001ull, 0x7fffffff7ffffffeull, 0xfedcba9876543210ull, 1,
    };
    for (unsigned c = 0; c < 256; ++c) {
        for (unsigned k = 0; k < sizeof values / sizeof values[0]; ++k) {
            V n = {values[k], ~values[k]}, out;
            /* Upper count bits are junk: only the low byte counts. */
            V m32 = {0xa5a5a500ull | c | (uint64_t)(0x3c3c3c00u | ((c + 1) & 255)) << 32,
                     0x96969600ull | ((c + 2) & 255) | (uint64_t)(0x69696900u | ((c + 3) & 255)) << 32};
            V m64 = {0xa5a5a5a5a5a5a500ull | c, 0x9696969696969600ull | ((c + 64) & 255)};
            RUN3(0x6ea14402u, n, m32, out); check_shift("ushl.4s", out, n, m32, 32, 1, 0);
            RUN3(0x4ea14402u, n, m32, out); check_shift("sshl.4s", out, n, m32, 32, 1, 1);
            RUN3(0x2ea14402u, n, m32, out); check_shift("ushl.2s", out, n, m32, 32, 0, 0);
            RUN3(0x0ea14402u, n, m32, out); check_shift("sshl.2s", out, n, m32, 32, 0, 1);
            RUN3(0x6ee14402u, n, m64, out); check_shift("ushl.2d", out, n, m64, 64, 1, 0);
            RUN3(0x4ee14402u, n, m64, out); check_shift("sshl.2d", out, n, m64, 64, 1, 1);
        }
    }
}

/* Each form runs as "op v2, v0, v1" and with Vd aliasing Vn or Vm. */
#define ALIAS3(name, distinct, as_n, as_m) do { \
    V a = {0x3f800000bf800000ull, 0x7fc0000000000000ull}, b = {0xbf80000040000000ull, 0x000000017f800000ull}; \
    V want, got_n, got_m; \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n " distinct "\n str q2, [%0]" \
                     :: "r"(&want), "r"(&a), "r"(&b) : "v0", "v1", "v2", "memory"); \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n " as_n "\n str q0, [%0]" \
                     :: "r"(&got_n), "r"(&a), "r"(&b) : "v0", "v1", "memory"); \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n " as_m "\n str q1, [%0]" \
                     :: "r"(&got_m), "r"(&a), "r"(&b) : "v0", "v1", "memory"); \
    checks += 2; \
    if (memcmp(&want, &got_n, sizeof want) || memcmp(&want, &got_m, sizeof want)) { \
        printf("FAIL alias %s\n", name); ++fail; } \
} while (0)

static void aliases(void) {
    ALIAS3("ext.16b", "ext v2.16b, v0.16b, v1.16b, #5", "ext v0.16b, v0.16b, v1.16b, #5", "ext v1.16b, v0.16b, v1.16b, #5");
    ALIAS3("ext.8b", "ext v2.8b, v0.8b, v1.8b, #3", "ext v0.8b, v0.8b, v1.8b, #3", "ext v1.8b, v0.8b, v1.8b, #3");
    ALIAS3("fcmeq", "fcmeq v2.4s, v0.4s, v1.4s", "fcmeq v0.4s, v0.4s, v1.4s", "fcmeq v1.4s, v0.4s, v1.4s");
    ALIAS3("fcmge", "fcmge v2.4s, v0.4s, v1.4s", "fcmge v0.4s, v0.4s, v1.4s", "fcmge v1.4s, v0.4s, v1.4s");
    ALIAS3("fcmgt.2d", "fcmgt v2.2d, v0.2d, v1.2d", "fcmgt v0.2d, v0.2d, v1.2d", "fcmgt v1.2d, v0.2d, v1.2d");
    ALIAS3("fcmlt0", "fcmlt v2.2s, v1.2s, #0.0", "fcmlt v0.2s, v1.2s, #0.0", "fcmlt v1.2s, v1.2s, #0.0");
    ALIAS3("cmeq0.2d", "cmeq v2.2d, v1.2d, #0", "cmeq v0.2d, v1.2d, #0", "cmeq v1.2d, v1.2d, #0");
    ALIAS3("rev64.8b", "rev64 v2.8b, v1.8b", "rev64 v0.8b, v1.8b", "rev64 v1.8b, v1.8b");
    ALIAS3("rev64.8h", "rev64 v2.8h, v1.8h", "rev64 v0.8h, v1.8h", "rev64 v1.8h, v1.8h");
    ALIAS3("ushl.4s", "ushl v2.4s, v0.4s, v1.4s", "ushl v0.4s, v0.4s, v1.4s", "ushl v1.4s, v0.4s, v1.4s");
    ALIAS3("sshl.2d", "sshl v2.2d, v0.2d, v1.2d", "sshl v0.2d, v0.2d, v1.2d", "sshl v1.2d, v0.2d, v1.2d");
}

/* Preload Vd with Vn as well: narrowing "2" preserves its old low half. */
#define ALIAS1(op, dst, src, imm) do { \
    V n = {0x8001ffff7fff0001ull, 0xff807f018001ffffull}, want, got; \
    __asm__ volatile("ldr q0,[%1]\n mov v2.16b,v0.16b\n " \
        op " v2." dst ",v0." src imm "\n str q2,[%0]" \
        :: "r"(&want), "r"(&n) : "v0", "v2", "memory"); \
    __asm__ volatile("ldr q0,[%1]\n " op " v0." dst ",v0." src imm "\n str q0,[%0]" \
        :: "r"(&got), "r"(&n) : "v0", "memory"); \
    ++checks; \
    if (memcmp(&got,&want,16)) { ++fail; printf("FAIL alias " op " " dst "\n"); } \
} while (0)
static void widening_aliases(void) {
#define WIDEN(op) \
    ALIAS1(op, "8h", "8b", ",#0"); ALIAS1(op "2", "8h", "16b", ",#7"); \
    ALIAS1(op, "4s", "4h", ",#15"); ALIAS1(op "2", "4s", "8h", ",#0"); \
    ALIAS1(op, "2d", "2s", ",#0"); ALIAS1(op "2", "2d", "4s", ",#31")
    WIDEN("sshll"); WIDEN("ushll");
#undef WIDEN
    ALIAS1("shll", "8h", "8b", ",#8"); ALIAS1("shll2", "8h", "16b", ",#8");
    ALIAS1("shll", "4s", "4h", ",#16"); ALIAS1("shll2", "4s", "8h", ",#16");
    ALIAS1("shll", "2d", "2s", ",#32"); ALIAS1("shll2", "2d", "4s", ",#32");
    ALIAS1("shrn", "8b", "8h", ",#1"); ALIAS1("shrn2", "16b", "8h", ",#8");
    ALIAS1("shrn", "4h", "4s", ",#16"); ALIAS1("shrn2", "8h", "4s", ",#1");
    ALIAS1("shrn", "2s", "2d", ",#1"); ALIAS1("shrn2", "4s", "2d", ",#32");
    ALIAS1("fcvtl", "2d", "2s", ""); ALIAS1("fcvtl2", "2d", "4s", "");
    ALIAS1("fcvtn", "2s", "2d", ""); ALIAS1("fcvtn2", "4s", "2d", "");
    ALIAS1("fcvtn", "4h", "4s", ""); ALIAS1("fcvtn2", "8h", "4s", "");
}
#undef ALIAS1

/* Exact binary16 values, independent of the host half-conversion ISA.
 * Subnormals are integer multiples of 2^-24, exactly representable in f32. */
static uint32_t half_bits(uint16_t x, int dn) {
    const unsigned sign = (uint32_t)(x & 0x8000) << 16;
    const unsigned exp = (x >> 10) & 31, frac = x & 1023;
    if (exp == 31) return frac ? (dn ? 0x7fc00000u : sign | 0x7fc00000u | (frac << 13))
                              : sign | 0x7f800000u;
    if (exp) return sign | ((exp + 112) << 23) | (frac << 13);
    float f = (float)frac * 0x1p-24f;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return sign | bits;
}

static void half_widen(void) {
    uint64_t saved;
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
    for (unsigned dn = 0; dn < 2; dn++) {
        uint64_t control = (uint64_t)dn << 25;
        __asm__ volatile("msr fpcr, %0" :: "r"(control));
        for (unsigned base = 0; base < 65536; base += 4) {
            uint16_t input[8];
            uint32_t want[4];
            for (unsigned i = 0; i < 4; i++) {
                input[i] = base + i;
                input[i + 4] = (base + i) ^ 0x8000;
                want[i] = half_bits(input[i], dn);
            }
            uint32_t got[4];
#define HALF_CASE(op, src) do { \
    __asm__ volatile("ldr q0, [%1]\n " op "\n str " src ", [%0]" \
        :: "r"(got), "r"(input) : "v0", "v1", "memory"); \
    ++checks; \
    if (memcmp(got, want, 16)) { \
        if (fail < 8) printf("FAIL " op " base=%u dn=%u\n", base, dn); \
        ++fail; \
    } \
} while (0)
            HALF_CASE("fcvtl v1.4s, v0.4h", "q1");
            HALF_CASE("fcvtl v0.4s, v0.4h", "q0");
            for (unsigned i = 0; i < 4; i++) want[i] = half_bits(input[i + 4], dn);
            HALF_CASE("fcvtl2 v1.4s, v0.8h", "q1");
            HALF_CASE("fcvtl2 v0.4s, v0.8h", "q0");
#undef HALF_CASE
        }
    }
    __asm__ volatile("msr fpcr, %0" :: "r"(saved));
}

/* Thirteen vectors force XMM8-15 use. The first conversion can take the
 * NaN cold step before this trip's accumulator writes, so it must commit
 * the previous trip's values. Both loop exits expose the entire layout. */
static void cached_loop(int nan, unsigned trips) {
    uint16_t input[8] = {0x3c00, 0x4000, 0x4200, 0x4400, 1, 2, 3, 4};
    if (nan) input[0] = 0x7d55;
    float out[9][4];
    unsigned count = trips;
    __asm__ volatile(
        "ldr q0,[%1]\n fmov v1.4s,#1.0\n"
        "movi v20.4s,#0\n movi v21.4s,#0\n movi v22.4s,#0\n"
        "movi v23.4s,#0\n movi v24.4s,#0\n movi v25.4s,#0\n"
        "movi v26.4s,#0\n movi v27.4s,#0\n movi v28.4s,#0\n"
        "b 1f\n 1:\n"
        "fcvtl v30.4s,v0.4h\n"
        "ushll v31.4s,v0.4h,#0\n ucvtf v31.4s,v31.4s\n"
        "rev64 v31.4s,v31.4s\n ext v31.8b,v31.8b,v30.8b,#3\n"
        "fadd v20.4s,v20.4s,v1.4s\n fadd v21.4s,v21.4s,v1.4s\n"
        "fadd v22.4s,v22.4s,v1.4s\n fadd v23.4s,v23.4s,v1.4s\n"
        "fadd v24.4s,v24.4s,v1.4s\n fadd v25.4s,v25.4s,v1.4s\n"
        "fadd v26.4s,v26.4s,v1.4s\n fadd v27.4s,v27.4s,v1.4s\n"
        "fadd v28.4s,v28.4s,v1.4s\n"
        "subs %w0,%w0,#1\n b.ne 1b\n"
        "str q20,[%2,#0]\n str q21,[%2,#16]\n str q22,[%2,#32]\n"
        "str q23,[%2,#48]\n str q24,[%2,#64]\n str q25,[%2,#80]\n"
        "str q26,[%2,#96]\n str q27,[%2,#112]\n str q28,[%2,#128]\n"
        : "+r"(count) : "r"(input), "r"(out)
        : "v0", "v1", "v20", "v21", "v22", "v23", "v24", "v25",
          "v26", "v27", "v28", "v30", "v31", "cc", "memory");
    for (unsigned r = 0; r < 9; r++) for (unsigned i = 0; i < 4; i++) {
        ++checks;
        if (out[r][i] != (float)trips) { ++fail; printf("FAIL cached loop nan=%d trips=%u\n", nan, trips); }
    }
}

static void *worker(void *arg) { return arg; }
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--mt") == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, NULL)) return 2;
    }
    shifts();
    aliases();
    widening_aliases();
    half_widen();
    for (unsigned n = 1; n <= 1024; n *= 4) {
        cached_loop(0, n);
        cached_loop(1, n);
    }
    if (fail) { printf("native_simd_inline: %d of %d checks FAILED\n", fail, checks); return 1; }
    printf("native_simd_inline: ALL PASS (%d checks)\n", checks);
    return 0;
}
