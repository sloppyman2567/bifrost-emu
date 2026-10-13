/* Inline SIMD_NATIVE_EXTRA bodies: every USHL/SSHL count byte on 32- and
 * 64-bit lanes against the ARM pseudocode, and aliased Vd for each inlined
 * kind against the same instruction with distinct registers. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

int main(void) {
    shifts();
    aliases();
    if (fail) { printf("native_simd_inline: %d of %d checks FAILED\n", fail, checks); return 1; }
    printf("native_simd_inline: ALL PASS (%d checks)\n", checks);
    return 0;
}
