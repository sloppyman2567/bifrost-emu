/*
 * jit_bfext.c — JIT bitfield-extract width-masking tests (EXTR/BFM/CLZ).
 *
 * Background:
 *   The IR translator decomposes EXTR and BFM into primitive
 *   SHL/SHR/OR/AND ops. For the W-form (sf=0) those shifts MUST be
 *   emitted with an explicit 32-bit width: ARM masks both operands to
 *   the operation width, but the guest GPR slot still holds the full
 *   64-bit register. Without the width, a dirty Xm/Xn[63:32] shifts or
 *   rotates down INTO the 32-bit result window and corrupts it.
 *
 *   The CLZ constant fold must also respect the operand width: ARM
 *   `clz w` counts from bit 31 and returns 32 for zero input.
 *
 * Tests drive the exact instructions with inline asm; inputs are
 * runtime-opaque (volatile reads into registers) so GCC cannot
 * constant-fold them away, mirroring jit_carry.c's adcs32_chain test.
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

int main(void) {
    /* 1. EXTR W-form with a dirty upper source: x holds bit 32 only,
     * so the 32-bit operand value is 0 and extr must produce 0. The
     * unmasked-SHR bug pulls bit 32 down to bit 28 (0x10000000). */
    {
        volatile uint64_t vx = 0x100000000ULL;
        uint64_t x = vx, out = 0xAAAAAAA0u;
        asm volatile(
            "extr %w[out], %w[x], %w[x], #4\n"
            : [out]"=&r"(out)
            : [x]"r"(x)
            : "cc");
        CHECK(out == 0, "extr_w_dirty_upper");
    }

    /* 2. EXTR W-form positive case vs a runtime C model. */
    {
        volatile uint64_t vn = 0x123456789ABCDEF0ULL;
        volatile uint64_t vm = 0x0FEDCBA987654321ULL;
        uint64_t n = vn, m = vm, out = 0;
        uint32_t lsb = 12;
        asm volatile(
            "extr %w[out], %w[n], %w[m], #12\n"
            : [out]"=&r"(out)
            : [n]"r"(n), [m]"r"(m)
            : "cc");
        uint32_t want = (uint32_t)((((uint64_t)(uint32_t)n << 32)
                                    | (uint32_t)m) >> lsb);
        CHECK(out == want, "extr_w_model");
    }

    /* 3. EXTR X-form sanity (64-bit path must stay correct). */
    {
        volatile uint64_t vn = 0x0123456789ABCDEFULL;
        volatile uint64_t vm = 0xFEDCBA9876543210ULL;
        uint64_t n = vn, m = vm, out = 0;
        asm volatile(
            "extr %x[out], %x[n], %x[m], #8\n"
            : [out]"=&r"(out)
            : [n]"r"(n), [m]"r"(m)
            : "cc");
        uint64_t want = ((n << 56) | (m >> 8));
        CHECK(out == want, "extr_x_model");
    }

    /* 4. BFI W-form with a dirty upper source: inserting byte 0x00 at
     * bit 16 of a zero dest must give 0. The unmasked-rotate bug turns
     * src bit 32 into dest bit 16. */
    {
        volatile uint64_t vsrc = 0x100000000ULL;
        uint64_t src = vsrc, dst = 0;
        asm volatile(
            "bfi %w[dst], %w[src], #16, #8\n"
            : [dst]"+r"(dst)
            : [src]"r"(src)
            : "cc");
        CHECK(dst == 0, "bfi_w_dirty_upper");
    }

    /* 5. BFI W-form positive case: insert 0x5A at bit 16, dest keeps
     * its untouched bits. */
    {
        volatile uint64_t vsrc = 0x15A5A5A5A5A5A500ULL;
        volatile uint32_t vdst = 0xF0F0F0F0u;
        uint64_t src = vsrc;
        uint32_t dst = vdst;
        asm volatile(
            "bfi %w[dst], %w[src], #16, #8\n"
            : [dst]"+r"(dst)
            : [src]"r"(src)
            : "cc");
        uint32_t want = (vdst & ~(0xFFu << 16)) | (((uint32_t)src & 0xFF) << 16);
        CHECK(dst == want, "bfi_w_model");
    }

    /* 6. BFM X-form sanity (sf=1 path unchanged): immr=#44 > imms=#23
     * is the INSERT-LOW shape (BFI-like): the low imms+1 = 24 bits of
     * Rn are inserted at bit position width-immr = 20 of dest. */
    {
        volatile uint64_t vn = 0x89ABCDEF01234567ULL;
        volatile uint64_t vdst = 0x00000000DEADBEEFULL;
        uint64_t n = vn, dst = vdst;
        asm volatile(
            "bfm %x[dst], %x[n], #44, #23\n"
            : [dst]"+r"(dst)
            : [n]"r"(n)
            : "cc");
        uint64_t want = (vdst & ~(0xFFFFFFULL << 20))
                      | ((n & 0xFFFFFFULL) << 20);
        CHECK(dst == want, "bfm_x_model");
    }

    /* 7. CLZ W-form: zero input returns 32, 1 returns 31. */
    {
        volatile uint32_t v0 = 0, v1 = 1;
        uint32_t z = v0, o = v1, cz = 0xFFFF, co = 0xFFFF;
        asm volatile(
            "clz %w[cz], %w[z]\n"
            "clz %w[co], %w[o]\n"
            : [cz]"=&r"(cz), [co]"=&r"(co)
            : [z]"r"(z), [o]"r"(o)
            : "cc");
        CHECK(cz == 32 && co == 31, "clz_w");
    }

    /* 8. CLZ X-form: zero input returns 64. */
    {
        volatile uint64_t v0 = 0, vhi = 1ULL << 63;
        uint64_t z = v0, h = vhi, cz = 99, ch = 99;
        asm volatile(
            "clz %x[cz], %x[z]\n"
            "clz %x[ch], %x[h]\n"
            : [cz]"=&r"(cz), [ch]"=&r"(ch)
            : [z]"r"(z), [h]"r"(h)
            : "cc");
        CHECK(cz == 64 && ch == 0, "clz_x");
    }

    printf("bfext: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
