// jit_branch_flags.c — conditional branches on flags that live in cpu.pstate.
// 1. Every condition against all sixteen NZCV values, the flags set by MSR in the previous block: EQ/NE/CS/CC/
//    MI/PL/VS/VC test one pstate bit in memory, the other conditions load the flags.
// 2. Chains of conditional branches, each in its own block, ending in MRS: a block that only consumes flags it
//    loaded from pstate must leave pstate as it found it, on its taken and fall-through edges, after both CMP
//    and ADDS (the carry-inverted HI/LS forms).
// 3. 16-byte pair copies (LDP/STP Q) of many sizes and alignments, including copies that cross a page boundary.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fails;
#define CHECK(c, what, a, b) do { if (!(c)) { ++fails; if (fails < 16) printf("NG %s %llx %llx\n", what, (unsigned long long)(a), (unsigned long long)(b)); } } while (0)

static int cond_true(int cc, unsigned nzcv) {
    const int n = (nzcv >> 3) & 1, z = (nzcv >> 2) & 1, c = (nzcv >> 1) & 1, v = nzcv & 1;
    switch (cc) {
        case 0: return z;            case 1: return !z;
        case 2: return c;            case 3: return !c;
        case 4: return n;            case 5: return !n;
        case 6: return v;            case 7: return !v;
        case 8: return c && !z;      case 9: return !(c && !z);
        case 10: return n == v;      case 11: return n != v;
        case 12: return !z && n == v; default: return z || n != v;
    }
}

#define BR_TEST(cc, name) static uint64_t name(uint64_t nzcv) { uint64_t r; \
    __asm__ volatile("msr nzcv, %1\n b." #cc " 1f\n mov %0, #0\n b 2f\n 1: mov %0, #1\n 2:" : "=&r"(r) : "r"(nzcv << 28) : "cc"); return r; }
BR_TEST(eq, br_eq) BR_TEST(ne, br_ne) BR_TEST(cs, br_cs) BR_TEST(cc, br_cc) BR_TEST(mi, br_mi) BR_TEST(pl, br_pl)
BR_TEST(vs, br_vs) BR_TEST(vc, br_vc) BR_TEST(hi, br_hi) BR_TEST(ls, br_ls) BR_TEST(ge, br_ge) BR_TEST(lt, br_lt)
BR_TEST(gt, br_gt) BR_TEST(le, br_le)
static uint64_t (*const br[14])(uint64_t) = {br_eq, br_ne, br_cs, br_cc, br_mi, br_pl, br_vs, br_vc,
                                             br_hi, br_ls, br_ge, br_lt, br_gt, br_le};

static uint64_t nzcv_now(void) { uint64_t s; __asm__ volatile("mrs %0, nzcv" : "=r"(s)); return s & 0xf0000000; }
static uint32_t nzcv_sub(uint64_t a, uint64_t b) {
    uint64_t r = a - b; uint32_t n = (int64_t)r < 0, z = r == 0, c = a >= b;
    uint32_t v = (int64_t)((a ^ b) & (a ^ r)) < 0;
    return (n << 31) | (z << 30) | (c << 29) | (v << 28);
}
static uint32_t nzcv_add(uint64_t a, uint64_t b) {
    uint64_t r = a + b; uint32_t n = (int64_t)r < 0, z = r == 0, c = r < a;
    uint32_t v = (int64_t)(~(a ^ b) & (a ^ r)) < 0;
    return (n << 31) | (z << 30) | (c << 29) | (v << 28);
}

__attribute__((noinline)) static void pair_copy(void* d, const void* s, long n) {
    __asm__ volatile(
        "1: ldp q0, q1, [%1], #32\n stp q0, q1, [%0], #32\n subs %2, %2, #32\n b.ne 1b"
        : "+r"(d), "+r"(s), "+r"(n) : : "v0", "v1", "cc", "memory");
}

int main(void) {
    // 1. conditions x flags
    for (int cc = 0; cc < 14; ++cc)
        for (unsigned f = 0; f < 16; ++f) {
            for (int rep = 0; rep < 3; ++rep)
                CHECK(br[cc](f) == (uint64_t)cond_true(cc, f), "cond", cc, f);
        }
    // 2. chains of branches that only consume flags
    static const uint64_t v[] = {0, 1, 2, 0x7f, 0x80, 0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull,
                                 0x8000000000000000ull, 0x8000000000000001ull, 0xfffffffffffffffeull, ~0ull};
    for (int rep = 0; rep < 3; ++rep)
        for (unsigned i = 0; i < 12; ++i)
            for (unsigned j = 0; j < 12; ++j) {
                uint64_t a = v[i], b = v[j], s, path;
                // cmp, then branches on N, Z, V, signed and unsigned orderings; each lands on MRS
                __asm__ volatile(
                    "cmp %2, %3\n"
                    "mov %1, #0\n"
                    "b.mi 10f\n b.eq 11f\n b.vs 12f\n b.hi 13f\n b.lt 14f\n b.ge 15f\n"
                    "mov %1, #9\n b 20f\n"
                    "10: mov %1, #1\n b 20f\n 11: mov %1, #2\n b 20f\n 12: mov %1, #3\n b 20f\n"
                    "13: mov %1, #4\n b 20f\n 14: mov %1, #5\n b 20f\n 15: mov %1, #6\n"
                    "20: mrs %0, nzcv"
                    : "=&r"(s), "=&r"(path) : "r"(a), "r"(b) : "cc");
                const uint32_t f = nzcv_sub(a, b);
                const unsigned nzcv = f >> 28;
                const int N = nzcv >> 3 & 1, Z = nzcv >> 2 & 1, C = nzcv >> 1 & 1, V = nzcv & 1;
                const uint64_t want = N ? 1 : Z ? 2 : V ? 3 : (C && !Z) ? 4 : (N != V) ? 5 : 6;
                CHECK(path == want, "cmp chain path", a, b);
                CHECK((s & 0xf0000000) == f, "cmp chain nzcv", a, b);
                // ADDS, so HI/LS see the add carry; branch on LS first, then CS, then GT
                __asm__ volatile(
                    "adds xzr, %2, %3\n"
                    "mov %1, #0\n"
                    "b.ls 10f\n b.cs 11f\n b.gt 12f\n"
                    "mov %1, #9\n b 20f\n"
                    "10: mov %1, #1\n b 20f\n 11: mov %1, #2\n b 20f\n 12: mov %1, #3\n"
                    "20: mrs %0, nzcv"
                    : "=&r"(s), "=&r"(path) : "r"(a), "r"(b) : "cc");
                const uint32_t g = nzcv_add(a, b);
                const unsigned m = g >> 28;
                const int n2 = m >> 3 & 1, z2 = m >> 2 & 1, c2 = m >> 1 & 1, v2 = m & 1;
                const uint64_t want2 = !(c2 && !z2) ? 1 : c2 ? 2 : (!z2 && n2 == v2) ? 3 : 9;
                CHECK(path == want2, "adds chain path", a, b);
                CHECK((s & 0xf0000000) == g, "adds chain nzcv", a, b);
            }
    // flags must survive several consuming blocks and a later plain read
    {
        uint64_t s;
        __asm__ volatile("msr nzcv, %1\n b.pl 1f\n b.vs 2f\n b.ls 3f\n 1: 2: 3: mrs %0, nzcv" : "=&r"(s) : "r"((uint64_t)0xa << 28) : "cc");
        CHECK((s & 0xf0000000) == 0xa0000000u, "msr + consuming blocks", s, 0);
    }
    // 3. Q pair copies
    uint8_t* region = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    static uint8_t src[4096 + 64] __attribute__((aligned(64)));
    for (unsigned i = 0; i < sizeof src; ++i) src[i] = (uint8_t)(i * 131 + 7);
    for (long n = 32; n <= 1024; n += 32)
        for (int so = 0; so < 64; so += 16)
            for (int doff = 0; doff < 4096 - 1024; doff += 1344 + doff / 7 + 16) {
                uint8_t* d = region + 4096 - 64 + doff % 128;   // destinations that cross the page boundary
                memset(region, 0xCC, 3 * 4096);
                pair_copy(d, src + so, n);
                CHECK(memcmp(d, src + so, n) == 0, "pair copy data", n, so);
                CHECK(d[-1] == 0xCC && d[n] == 0xCC, "pair copy bounds", n, so);
            }
    printf("jit_branch_flags: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
