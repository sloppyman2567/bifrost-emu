/* Regression for W-form shifted-register operands in the JIT. */
#include <stdint.h>
#include <stdio.h>

static int fails;

#define CHECK(expr, tag) do { \
    if (expr) printf("ok %s\n", tag); \
    else { printf("NG %s\n", tag); fails++; } \
} while (0)
#define CHECK_EQ(got_expr, want_expr, tag) do { \
    uint32_t got = (got_expr); \
    uint32_t want = (want_expr); \
    if (got == want) printf("ok %s\n", tag); \
    else { printf("NG %s got=0x%08x want=0x%08x\n", tag, got, want); fails++; } \
} while (0)

__attribute__((noinline))
static uint32_t add_lsr24(uint32_t lhs, uint64_t rhs)
{
    uint32_t out;
    __asm__ volatile("add %w0, %w1, %w2, lsr #24"
                     : "=&r"(out) : "r"(lhs), "r"(rhs));
    return out;
}

__attribute__((noinline))
static uint32_t add_lsl8(uint32_t lhs, uint64_t rhs)
{
    uint32_t out;
    __asm__ volatile("add %w0, %w1, %w2, lsl #8"
                     : "=&r"(out) : "r"(lhs), "r"(rhs));
    return out;
}

__attribute__((noinline))
static uint32_t add_asr24(uint32_t lhs, uint64_t rhs)
{
    uint32_t out;
    __asm__ volatile("add %w0, %w1, %w2, asr #24"
                     : "=&r"(out) : "r"(lhs), "r"(rhs));
    return out;
}

__attribute__((noinline))
static uint32_t eor_ror8(uint32_t lhs, uint64_t rhs)
{
    uint32_t out;
    __asm__ volatile("eor %w0, %w1, %w2, ror #8"
                     : "=&r"(out) : "r"(lhs), "r"(rhs));
    return out;
}

int main(void)
{
    const uint64_t high_bits_set = 0x12345678ABCDEF01ULL;
    CHECK_EQ(add_lsr24(0x10, high_bits_set), 0xBB,
             "add_w_lsr_uses_low_32");
    CHECK_EQ(add_lsl8(0x10, high_bits_set),
             (uint32_t)(0x10u + ((uint32_t)high_bits_set << 8)),
             "add_w_lsl_wraps_at_32");
    CHECK_EQ(add_asr24(0, 0x0000000081234567ULL), 0xFFFFFF81u,
             "add_w_asr_signs_from_bit_31");
    CHECK_EQ(eor_ror8(0x10, high_bits_set),
             (0x10u ^ (((uint32_t)high_bits_set >> 8) |
                       ((uint32_t)high_bits_set << 24))),
             "eor_w_ror_wraps_at_32");

    /* Constants in one block exercise IR folding as well as codegen. */
    uint32_t folded;
    __asm__ volatile(
        "mov x5, #0xef01\n"
        "movk x5, #0xabcd, lsl #16\n"
        "movk x5, #0x5678, lsl #32\n"
        "movk x5, #0x1234, lsl #48\n"
        "add %w0, wzr, w5, lsr #24\n"
        : "=r"(folded) : : "x5");
    CHECK_EQ(folded, 0xAB, "folded_w_lsr_uses_low_32");

    printf("shifted_operand_width: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
