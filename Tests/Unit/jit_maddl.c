// UMADDL/UMULL widening multiply-add differential (neverball/zlib hunt).
// zlib inflate does `umaddl x25, w25, w24, x1` to form the loop end
// pointer; a miscompile here yields a wild end pointer and the
// near-NULL LDP crash. Cover high-bit and carry cases.
#include <stdint.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(c, name) do { \
    if (c) { printf("ok %s\n", name); } \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

int main(void) {
    uint64_t r;
    uint32_t a, b;
    uint64_t c;
    // 1. basic: 0x30 * w + x
    a = 0x10; b = 0x30; c = 0x11a6bf90ULL;
    __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    CHECK(r == 0x11a6bf90ULL + 0x10u * 0x30u, "umaddl_basic");
    // 2. high bits (63:32) of Xn/Xm must be ignored: materialize full
    // 64-bit values, use the W views. (A plain `mov wN` zero-extends so
    // it can never set them; real code can carry junk-high via 64-bit
    // producers.)
    {
        uint64_t xa = 0xDEAD000000000010ULL;  /* low32 = 0x10 */
        uint64_t xb = 0xBEEF000000000030ULL;  /* low32 = 0x30 */
        uint64_t xc = 0x1000;
        __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(xa), "r"(xb), "r"(xc));
        CHECK(r == 0x1300u, "umaddl_low32");
    }
    // 3. full 32-bit max product + carry into high word
    a = 0xFFFFFFFFu; b = 0xFFFFFFFFu; c = 1;
    __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    CHECK(r == 0xFFFFFFFE00000002ULL, "umaddl_max");
    // 4. addend high bits preserved (64-bit add, no truncation)
    a = 2; b = 3; c = 0xFFFFFFFF00000000ULL;
    __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    CHECK(r == 0xFFFFFFFF00000006ULL, "umaddl_addend64");
    // 5. unsigned (not signed): 0x80000000 * 2 = 0x100000000
    a = 0x80000000u; b = 2; c = 0;
    __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    CHECK(r == 0x100000000ULL, "umaddl_unsigned");
    // 6. umull basic
    a = 0x12345678u; b = 0x100u; c = 0;
    __asm__ volatile ("umull %0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));
    CHECK(r == (uint64_t)0x12345678u * 0x100u, "umull_basic");
    // 7. umull max
    a = 0xFFFFFFFFu; b = 0xFFFFFFFFu;
    __asm__ volatile ("umull %0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));
    CHECK(r == 0xFFFFFFFE00000001ULL, "umull_max");
    // 8. smaddl negative
    int32_t sa = -5, sb = 7; int64_t sc = 100;
    int64_t sr;
    __asm__ volatile ("smaddl %0, %w1, %w2, %3" : "=r"(sr) : "r"(sa), "r"(sb), "r"(sc));
    CHECK(sr == 100 - 35, "smaddl_neg");
    printf("maddl: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
