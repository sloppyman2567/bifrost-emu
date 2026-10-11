// vkQuake's Sky_DrawSky uses FCSEL with Rm=24/25 and Rd=31. An obsolete
// FCVTZS mask interpreted these as GPR conversions, corrupting XZR/pointers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int failures, checks;
#define CHECK(v, msg) do { checks++; if (!(v)) { failures++; printf("FAIL %s\n",msg); } } while (0)

#define TEST_SELECT(P, STORE, TYPE, RN, RM, RD) do { \
    for (int take_n = 0; take_n < 2; take_n++) { \
        TYPE n = (TYPE)1.0, m = (TYPE)0.25, out = 0; \
        uint64_t zero, saved_gpr; \
        __asm__ volatile ( \
            "mov x19, #0x1234\n" \
            "ldr " P RN ", [%[n]]\n" \
            "ldr " P RM ", [%[m]]\n" \
            "cmp %w[cond], #0\n" \
            "fcsel " P RD ", " P RN ", " P RM ", gt\n" \
            STORE " " P RD ", [%[out]]\n" \
            "orr %[zero], xzr, xzr\n" \
            "mov %[saved], x19\n" \
            : [zero] "=r"(zero), [saved] "=r"(saved_gpr) \
            : [n] "r"(&n), [m] "r"(&m), [out] "r"(&out), [cond] "r"(take_n) \
            : "x19", "v19", "v24", "v25", "v31", "cc", "memory"); \
        CHECK(out == (take_n ? n : m), "FCSEL " P RD " selects FP bits"); \
        CHECK(saved_gpr == 0x1234, "FCSEL preserves GPR X19"); \
        CHECK(zero == 0, "FCSEL preserves XZR"); \
    } \
} while (0)

static void test_xzr_destinations(void) {
    uint64_t zero;
    // Conversion and raw-move GPR destinations must discard register 31.
    __asm__ volatile (
        "fmov d0, #1.0\n"
        "fmov xzr, d0\n"
        "fmov wzr, s0\n"
        "fmov xzr, v0.d[1]\n"
        "fcvtzs xzr, d0\n"
        "fcvtzu wzr, d0\n"
        "fcvtms xzr, d0\n"
        "fcvtzu wzr, d0, #16\n"
        "orr %0, xzr, xzr\n"
        : "=r"(zero) : : "v0", "memory");
    CHECK(zero == 0, "GPR FP moves/conversions discard XZR writes");
}
int main(void) {
    TEST_SELECT("s", "str", float, "25", "24", "19");
    TEST_SELECT("s", "str", float, "24", "25", "19");
    TEST_SELECT("d", "str", double,"25", "24", "19");
    TEST_SELECT("d", "str", double,"24", "25", "19");
    TEST_SELECT("s", "str", float, "25", "24", "31");
    TEST_SELECT("s", "str", float, "24", "25", "31");
    TEST_SELECT("d", "str", double,"25", "24", "31");
    TEST_SELECT("d", "str", double,"24", "25", "31");
    // Include source/destination aliasing.
    TEST_SELECT("s", "str", float, "25", "24", "24");
    TEST_SELECT("d", "str", double,"24", "25", "25");
    test_xzr_destinations();
    printf("%d checks passed, %d failures\n", checks-failures, failures);
    return failures ? 1 : 0;
}
