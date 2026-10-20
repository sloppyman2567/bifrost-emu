// jit_flags_overwrite.c — a flag-setting instruction rewrites all of NZCV, so the JIT drops
// flags still held in the host instead of saving them. Checked against C's idea of CMP/CMN/TST:
// the first flags feed a CSET, the second setter's flags feed the branch and MRS; and flags that
// must survive an intervening load or a plain ADD still reach their reader.
#include <stdint.h>
#include <stdio.h>

static int fails;
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
static uint32_t nzcv_tst(uint64_t a, uint64_t b) { uint64_t r = a & b; return (((int64_t)r < 0) << 31) | ((r == 0) << 30); }
#define CHECK(cond, what, a, b) do { if (!(cond)) { ++fails; if (fails < 12) printf("NG %s a=%llx b=%llx\n", what, (unsigned long long)(a), (unsigned long long)(b)); } } while (0)

int main(void) {
    static const uint64_t v[] = {0, 1, 2, 0x7f, 0x80, 0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull,
                                 0x8000000000000000ull, 0x8000000000000001ull, 0xfffffffffffffffeull, ~0ull};
    volatile uint64_t cell = 0;
    for (unsigned i = 0; i < sizeof v / sizeof *v; ++i)
        for (unsigned j = 0; j < sizeof v / sizeof *v; ++j) {
            uint64_t a = v[i], b = v[j], c = v[(i * 5 + j * 3) % 12], d = v[(i + j * 7) % 12];
            uint64_t eq1, lt2, state;
            // 1. first CMP feeds CSET; second CMP overwrites; MRS sees the second's flags.
            __asm__ volatile("cmp %3, %4\n cset %0, eq\n cmp %5, %6\n cset %1, lt\n mrs %2, nzcv"
                : "=&r"(eq1), "=&r"(lt2), "=&r"(state) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(eq1 == (a == b), "cset after first cmp", a, b);
            CHECK(lt2 == ((int64_t)c < (int64_t)d), "cset after second cmp", c, d);
            CHECK((state & 0xf0000000) == nzcv_sub(c, d), "nzcv after second cmp", c, d);
            // 2. ADDS then TST then CMN, each the only reader of the previous op's flags being the next.
            uint64_t cs; uint64_t s1;
            __asm__ volatile("adds %0, %3, %4\n csel %1, %3, %4, hs\n tst %5, %6\n mrs %2, nzcv"
                : "=&r"(s1), "=&r"(cs), "=&r"(state) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(cs == (nzcv_add(a, b) & 0x20000000 ? a : b), "csel after adds", a, b);
            CHECK((state & 0xf0000000) == nzcv_tst(c, d), "nzcv after tst", c, d);
            // 3. Flags that must survive an intervening load (which compares for its guard) and a plain ADD.
            uint64_t ld, plain, lo;
            cell = d;
            __asm__ volatile("cmp %3, %4\n ldr %0, [%5]\n add %1, %1, #1\n cset %2, hi"
                : "=&r"(ld), "=&r"(plain), "=&r"(lo) : "r"(a), "r"(b), "r"(&cell), "1"((uint64_t)7) : "cc", "memory");
            CHECK(ld == d && plain == 8, "load/add values", a, b);
            CHECK(lo == (a > b), "flags survive load and add", a, b);
            // 4. Branch on the second setter while the first's flags were consumed by CINC.
            uint64_t count = 0, taken = 0;
            __asm__ volatile("mov %0, #0\n cmp %2, %3\n cinc %0, %0, eq\n cmp %4, %5\n b.ne 1f\n mov %1, #1\n b 2f\n 1: mov %1, #2\n 2:"
                : "=&r"(count), "=&r"(taken) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(count == (a == b), "cinc from first cmp", a, b);
            CHECK(taken == (c != d ? 2 : 1), "branch from second cmp", c, d);
        }
    printf("jit_flags_overwrite: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
