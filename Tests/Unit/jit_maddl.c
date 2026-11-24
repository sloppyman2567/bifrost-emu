// UMADDL/UMULL widening multiply-add differential (neverball/zlib hunt).
// zlib inflate does `umaddl x25, w25, w24, x1` to form the loop end
// pointer; a miscompile here yields a wild end pointer and the
// near-NULL LDP crash. Cover high-bit and carry cases.
// The widening multiply-accumulate family (SMADDL, UMADDL, SMSUBL, UMSUBL and their
// smull/umull/smnegl/umnegl aliases) is lowered to extend + MUL + ADD/SUB: sweep values
// (high junk bits in the W operands), register aliasing, the XZR forms, a dependent chain in
// one block and a loop-carried accumulator against C references.
#include <stdint.h>
#include <stdio.h>

static int fails = 0;
// Reference product mod 2^64 by shift-and-add: the compiler turns a C widening multiply into the
// instruction under test, which would make the sweeps compare it with itself.
static __attribute__((noinline)) uint64_t ref_mul(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) if ((b >> i) & 1) r += a << i;
    return r;
}
#define ZW(v) ((uint64_t)(uint32_t)(v))
#define SW(v) ((uint64_t)(int64_t)(int32_t)(v))
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
    // ---- Value sweeps against C references (random 64-bit inputs: the high halves are junk the W views must ignore)
    {
        uint64_t seed = 0x9E3779B97F4A7C15ULL;
        #define NEXT() (seed = seed * 6364136223846793005ULL + 1442695040888963407ULL, seed ^ (seed >> 29))
        int bad[12] = {0};
        for (int i = 0; i < 4000; i++) {
            uint64_t x = NEXT(), y = NEXT(), acc = NEXT();
            if (i < 8) {   // boundary lanes first
                static const uint32_t e[8] = {0, 1, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x12345678u};
                x = (x & ~0xFFFFFFFFULL) | e[i]; y = (y & ~0xFFFFFFFFULL) | e[7 - i];
            }
            const uint64_t up = ref_mul(ZW(x), ZW(y)), sp = ref_mul(SW(x), SW(y));
            uint64_t r;
            __asm__ volatile ("umaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(x), "r"(y), "r"(acc));
            if (r != acc + up) bad[0]++;
            __asm__ volatile ("smaddl %0, %w1, %w2, %3" : "=r"(r) : "r"(x), "r"(y), "r"(acc));
            if (r != acc + sp) bad[1]++;
            __asm__ volatile ("umsubl %0, %w1, %w2, %3" : "=r"(r) : "r"(x), "r"(y), "r"(acc));
            if (r != acc - up) bad[2]++;
            __asm__ volatile ("smsubl %0, %w1, %w2, %3" : "=r"(r) : "r"(x), "r"(y), "r"(acc));
            if (r != acc - sp) bad[3]++;
            __asm__ volatile ("umull %0, %w1, %w2" : "=r"(r) : "r"(x), "r"(y));
            if (r != up) bad[4]++;
            __asm__ volatile ("smull %0, %w1, %w2" : "=r"(r) : "r"(x), "r"(y));
            if (r != sp) bad[5]++;
            __asm__ volatile ("umnegl %0, %w1, %w2" : "=r"(r) : "r"(x), "r"(y));
            if (r != (uint64_t)0 - up) bad[6]++;
            __asm__ volatile ("smnegl %0, %w1, %w2" : "=r"(r) : "r"(x), "r"(y));
            if (r != (uint64_t)0 - sp) bad[7]++;
            // Zero-register operands: a W operand of wzr is 0, an Xa of xzr is the plain product.
            __asm__ volatile ("smaddl %0, wzr, %w1, %2" : "=r"(r) : "r"(y), "r"(acc));
            if (r != acc) bad[8]++;
            __asm__ volatile ("umsubl %0, %w1, wzr, %2" : "=r"(r) : "r"(x), "r"(acc));
            if (r != acc) bad[9]++;
        }
        static const char* const names[10] = {"umaddl_sweep", "smaddl_sweep", "umsubl_sweep", "smsubl_sweep",
            "umull_sweep", "smull_sweep", "umnegl_sweep", "smnegl_sweep", "smaddl_wzr", "umsubl_wzr"};
        for (int k = 0; k < 10; k++) CHECK(bad[k] == 0, names[k]);
        // Aliasing: the destination is also an operand or the accumulator.
        int alias_bad = 0;
        for (int i = 0; i < 2000; i++) {
            uint64_t x = NEXT(), y = NEXT(), acc = NEXT();
            uint64_t r;
            r = acc; __asm__ volatile ("umaddl %0, %w1, %w2, %0" : "+r"(r) : "r"(x), "r"(y));
            if (r != acc + ref_mul(ZW(x), ZW(y))) alias_bad++;
            r = x; __asm__ volatile ("smaddl %0, %w0, %w1, %2" : "+r"(r) : "r"(y), "r"(acc));
            if (r != acc + ref_mul(SW(x), SW(y))) alias_bad++;
            r = y; __asm__ volatile ("smsubl %0, %w1, %w0, %2" : "+r"(r) : "r"(x), "r"(acc));
            if (r != acc - ref_mul(SW(x), SW(y))) alias_bad++;
            r = x; __asm__ volatile ("smull %0, %w0, %w0" : "+r"(r));
            if (r != ref_mul(SW(x), SW(x))) alias_bad++;
            r = x; __asm__ volatile ("umsubl %0, %w0, %w0, %0" : "+r"(r));
            if (r != x - ref_mul(ZW(x), ZW(x))) alias_bad++;
        }
        CHECK(alias_bad == 0, "maddl_aliasing");
        // A dependent chain in one block: each result feeds the next op as operand and accumulator.
        int chain_bad = 0;
        for (int i = 0; i < 2000; i++) {
            uint64_t a = NEXT(), b = NEXT();
            uint64_t r2, r3, r4;
            __asm__ volatile (
                "umull %0, %w3, %w4\n\t"
                "smaddl %1, %w0, %w4, %0\n\t"
                "umsubl %2, %w1, %w0, %1\n\t"
                : "=&r"(r2), "=&r"(r3), "=&r"(r4) : "r"(a), "r"(b));
            const uint64_t e2 = ref_mul(ZW(a), ZW(b));
            const uint64_t e3 = e2 + ref_mul(SW(e2), SW(b));
            const uint64_t e4 = e3 - ref_mul(ZW(e3), ZW(e2));
            if (r2 != e2 || r3 != e3 || r4 != e4) chain_bad++;
        }
        CHECK(chain_bad == 0, "maddl_chain");
        // The flags set by a compare survive a widening multiply-accumulate (it writes none).
        int flag_bad = 0;
        for (int i = 0; i < 2000; i++) {
            uint64_t a = NEXT(), b = NEXT(), x = NEXT(), y = NEXT(), acc = NEXT();
            if (i & 1) b = a;   // equal operands too
            uint64_t r, lt, eq, hi;
            __asm__ volatile (
                "cmp %w4, %w5\n\t"
                "smaddl %0, %w6, %w7, %8\n\t"
                "cset %w1, lt\n\t"
                "cset %w2, eq\n\t"
                "cset %w3, hi\n\t"
                : "=&r"(r), "=&r"(lt), "=&r"(eq), "=&r"(hi) : "r"(a), "r"(b), "r"(x), "r"(y), "r"(acc) : "cc");
            if (r != acc + ref_mul(SW(x), SW(y)) || lt != ((int32_t)a < (int32_t)b) || eq != ((uint32_t)a == (uint32_t)b) ||
                hi != ((uint32_t)a > (uint32_t)b)) flag_bad++;
            __asm__ volatile (
                "cmp %w4, %w5\n\t"
                "umsubl %0, %w6, %w7, %8\n\t"
                "cset %w1, lt\n\t"
                "cset %w2, eq\n\t"
                "cset %w3, hi\n\t"
                : "=&r"(r), "=&r"(lt), "=&r"(eq), "=&r"(hi) : "r"(a), "r"(b), "r"(x), "r"(y), "r"(acc) : "cc");
            if (r != acc - ref_mul(ZW(x), ZW(y)) || lt != ((int32_t)a < (int32_t)b) || eq != ((uint32_t)a == (uint32_t)b) ||
                hi != ((uint32_t)a > (uint32_t)b)) flag_bad++;
        }
        CHECK(flag_bad == 0, "maddl_flags_preserved");
        // A loop-carried accumulator the loop pins in a host register.
        {
            uint64_t acc = 0x1234, ref = 0x1234;
            uint32_t k = 0x9E3779B1u;
            for (uint32_t i = 1; i <= 3000; i++) {
                __asm__ volatile ("smaddl %0, %w1, %w2, %0" : "+r"(acc) : "r"(i), "r"(k));
                ref += ref_mul(SW(i), SW(k));
                __asm__ volatile ("umsubl %0, %w1, %w2, %0" : "+r"(acc) : "r"(i), "r"(i));
                ref -= ref_mul(ZW(i), ZW(i));
            }
            CHECK(acc == ref, "maddl_loop_acc");
        }
        #undef NEXT
    }
    printf("maddl: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
