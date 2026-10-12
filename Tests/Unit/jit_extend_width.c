/*
 * jit_extend_width.c — sign/zero extension lowering.
 *
 * SEXT 8/16/32 lower to MOVSX/MOVSXD and ZEXT 8/16/32 to MOVZX/MOV r32;
 * AND with 0xFF/0xFFFF/0xFFFFFFFF folds to ZEXT. None of these may disturb
 * live NZCV, and byte sources must use SPL..DIL / R8B-R15B, not AH..BH.
 * Expected values are computed in C and also pass under QEMU.
 */
#include <stdint.h>
#include <stdio.h>

static int checks, fails;
static void expect(const char* name, uint64_t got, uint64_t want) {
    ++checks;
    if (got != want) {
        printf("FAIL %s got=%016llx want=%016llx\n", name,
               (unsigned long long)got, (unsigned long long)want);
        ++fails;
    }
}
#define UN(name, ins, src, want) do { uint64_t d = 0x5a5a5a5a5a5a5a5aull, s = (src); \
    __asm__ volatile(ins : "+r"(d) : "r"(s)); expect(name, d, (want)); } while (0)
#define BIN(name, ins, a, b, want) do { uint64_t d = 0, x = (a), y = (b); \
    __asm__ volatile(ins : "=r"(d) : "r"(x), "r"(y)); expect(name, d, (want)); } while (0)

int main(void) {
    const uint64_t v[] = {0, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffffull,
                          0x80000000ull, 0xffffffffull, 0x0123456789abcdefull,
                          0xfedcba9876543210ull, ~0ull};
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        const uint64_t x = v[i], y = v[(i + 5) % (sizeof(v) / sizeof(v[0]))];
        UN("sxtb x", "sxtb %0, %w1", x, (uint64_t)(int64_t)(int8_t)x);
        UN("sxth x", "sxth %0, %w1", x, (uint64_t)(int64_t)(int16_t)x);
        UN("sxtw x", "sxtw %0, %w1", x, (uint64_t)(int64_t)(int32_t)x);
        UN("sxtb w", "sxtb %w0, %w1", x, (uint32_t)(int32_t)(int8_t)x);
        UN("sxth w", "sxth %w0, %w1", x, (uint32_t)(int32_t)(int16_t)x);
        UN("uxtb w", "uxtb %w0, %w1", x, x & 0xff);
        UN("uxth w", "uxth %w0, %w1", x, x & 0xffff);
        UN("and x ff", "and %0, %1, #0xff", x, x & 0xff);
        UN("and x ffff", "and %0, %1, #0xffff", x, x & 0xffff);
        UN("and x ffffffff", "and %0, %1, #0xffffffff", x, x & 0xffffffffull);
        UN("and w ffff", "and %w0, %w1, #0xffff", x, x & 0xffff);
        BIN("add uxtw", "add %0, %1, %w2, uxtw #1", x, y, x + ((y & 0xffffffffull) << 1));
        BIN("add sxtb", "add %0, %1, %w2, sxtb", x, y, x + (uint64_t)(int64_t)(int8_t)y);
        BIN("add uxth", "add %0, %1, %w2, uxth #2", x, y, x + ((y & 0xffff) << 2));
        BIN("sub sxth", "sub %w0, %w1, %w2, sxth", x, y,
            (uint32_t)((uint32_t)x - (uint32_t)(int32_t)(int16_t)y));
        /* Signed loads: ldrsb/ldrsh/ldrsw to X and W. */
        const uint64_t mem = x;
        uint64_t r;
        __asm__ volatile("ldrsb %0, [%1]" : "=r"(r) : "r"(&mem) : "memory");
        expect("ldrsb x", r, (uint64_t)(int64_t)(int8_t)x);
        __asm__ volatile("ldrsh %w0, [%1]" : "=r"(r) : "r"(&mem) : "memory");
        expect("ldrsh w", r, (uint32_t)(int32_t)(int16_t)x);
        __asm__ volatile("ldrsw %0, [%1]" : "=r"(r) : "r"(&mem) : "memory");
        expect("ldrsw x", r, (uint64_t)(int64_t)(int32_t)x);
        /* NZCV set before the extension is consumed after it. */
        uint64_t lt = 0, ext = 0;
        __asm__ volatile("cmp %2, %3\n sxtb %1, %w2\n uxth %w1, %w1\n cset %w0, lt"
                         : "=&r"(lt), "=&r"(ext) : "r"(x), "r"(y) : "cc");
        expect("flags across extend", lt, (int64_t)x < (int64_t)y);
        expect("extend under live flags", ext, (uint64_t)(int64_t)(int8_t)x & 0xffff);
        __asm__ volatile("cmp %w2, %w3\n and %1, %2, #0xffffffff\n cset %w0, hi"
                         : "=&r"(lt), "=&r"(ext) : "r"(x), "r"(y) : "cc");
        expect("flags across and-mask", lt, (uint32_t)x > (uint32_t)y);
        expect("and-mask under live flags", ext, x & 0xffffffffull);
    }
    /* Many live values push host allocation into R8-R15 and SPL..DIL. */
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        uint64_t a = v[i], b = a ^ 0x1111, c = a + 0x81, e = a - 0x7f01, f = ~a, g = a * 3;
        uint64_t h = a << 7, k = a >> 3, m = a | 0x80, n = a & 0xff80, o = a + 7, p = a ^ 0xff;
        __asm__ volatile(
            "sxtb %0, %w0\n sxth %1, %w1\n uxtb %w2, %w2\n uxth %w3, %w3\n"
            "sxtw %4, %w4\n sxtb %w5, %w5\n and %6, %6, #0xffffffff\n uxtb %w7, %w7\n"
            "sxth %w8, %w8\n and %9, %9, #0xffff\n sxtb %10, %w10\n uxtb %w11, %w11\n"
            : "+r"(a), "+r"(b), "+r"(c), "+r"(e), "+r"(f), "+r"(g),
              "+r"(h), "+r"(k), "+r"(m), "+r"(n), "+r"(o), "+r"(p));
        const uint64_t s = v[i];
        expect("live sxtb", a, (uint64_t)(int64_t)(int8_t)s);
        expect("live sxth", b, (uint64_t)(int64_t)(int16_t)(s ^ 0x1111));
        expect("live uxtb", c, (s + 0x81) & 0xff);
        expect("live uxth", e, (s - 0x7f01) & 0xffff);
        expect("live sxtw", f, (uint64_t)(int64_t)(int32_t)~s);
        expect("live sxtb w", g, (uint32_t)(int32_t)(int8_t)(s * 3));
        expect("live and32", h, (s << 7) & 0xffffffffull);
        expect("live uxtb 2", k, (s >> 3) & 0xff);
        expect("live sxth w", m, (uint32_t)(int32_t)(int16_t)(s | 0x80));
        expect("live and16", n, (s & 0xff80) & 0xffff);
        expect("live sxtb 2", o, (uint64_t)(int64_t)(int8_t)(s + 7));
        expect("live uxtb 3", p, (s ^ 0xff) & 0xff);
    }
    if (fails) { printf("jit_extend_width: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_extend_width: %d checks passed\n", checks);
    return 0;
}
