/*
 * jit_addsub_imm.c — JIT ADD/SUB immediate (incl. LSL #12 variant) tests.
 *
 * Background (alpha.4 bug):
 *   The 12-bit ARM64 immediate field supports an optional `LSL #12`
 *   shift (the "S" bit at position 22).  When set, the immediate is
 *   shifted left 12 bits before being added.  The JIT translator
 *   initially ignored this bit, so any code that did:
 *
 *       add x0, x0, #0x1, lsl #12   ; → x0 + 4096
 *
 *   actually computed x0 + 1 (because the shift was lost).
 *
 *   Real-world victim: musl's malloc arena sizing, which adds
 *   `#0x1, lsl #12` to round up to a page boundary.  On the buggy
 *   JIT malloc would silently allocate 4095 bytes too few.
 *
 * Uses pure C — the compiler emits ADD/SUB imm (with optional LSL #12)
 * for these arithmetic patterns.
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

/* Force the compiler to emit ADD imm (not just constant-fold). */
__attribute__((noinline))
static uint64_t add_imm(uint64_t a) { return a + 200; }

__attribute__((noinline))
static uint64_t sub_imm(uint64_t a) { return a - 250; }

__attribute__((noinline))
static uint64_t add_imm_max12(uint64_t a) { return a + 0xFFF; }

__attribute__((noinline))
static uint64_t add_imm_lsl12_one(uint64_t a) { return a + 0x1000; }

__attribute__((noinline))
static uint64_t add_imm_lsl12_max(uint64_t a) { return a + 0xFFF000; }

__attribute__((noinline))
static uint64_t sub_imm_lsl12(uint64_t a) { return a - 0x1000; }

__attribute__((noinline))
static uint64_t add_imm_combined(uint64_t a) { return a + 0x1000 + 2; }

__attribute__((noinline))
static uint32_t add_imm_lsl12_w(uint32_t a) { return a + 0x1000; }

__attribute__((noinline))
static uint32_t sub_imm_lsl12_w(uint32_t a) { return a - 0x1000; }

__attribute__((noinline))
static int adds_imm_carry(uint64_t a, uint64_t b) {
    /* Compiler emits ADDS then CSET for the < comparison */
    return a < (a + b);  /* carry occurred iff (a+b) wrapped */
}

__attribute__((noinline))
static uint64_t adds_imm_lsl12(uint64_t a) {
    /* This is just ADD with 0x1000, but we want the ADDS form... */
    return a + 0x1000;
}

__attribute__((noinline))
static uint64_t addsub_sp(uint64_t sp_in) {
    /* Use volatile to prevent the compiler from optimizing away the
     * SP manipulation. */
    volatile uint64_t sp = sp_in;
    sp = sp - 0x20;
    volatile uint64_t after = sp;
    sp = sp + 0x20;
    return after;
}

int main(void) {
    CHECK(add_imm(100) == 300, "add_imm_plain");
    CHECK(sub_imm(1000) == 750, "sub_imm_plain");
    CHECK(add_imm_max12(0) == 0xFFF, "add_imm_max12");
    CHECK(add_imm_lsl12_one(0) == 0x1000, "add_imm_lsl12_one");
    CHECK(add_imm_lsl12_max(0) == 0xFFF000, "add_imm_lsl12_max");
    CHECK(sub_imm_lsl12(0x1000000) == 0x1000000 - 0x1000, "sub_imm_lsl12");
    CHECK(add_imm_combined(1) == 0x1003, "add_imm_combined");
    CHECK(add_imm_lsl12_w(0) == 0x1000, "add_imm_lsl12_w");
    CHECK(sub_imm_lsl12_w(0x10000) == 0xF000, "sub_imm_lsl12_w");

    /* ADDS carry test — when a+b wraps, a >= a+b unsigned (carry set) */
    CHECK(adds_imm_carry(0xFFFFFFFFFFFFFFFFULL, 1) == 0, "adds_imm_carry");
    /* 1 + 2 = 3, no carry, 1 < 3 → true (1) */
    CHECK(adds_imm_carry(1, 2) == 1, "adds_imm_nocarry");

    CHECK(adds_imm_lsl12(0) == 0x1000, "adds_imm_lsl12");

    /* SP-relative add/sub — the JIT must keep these 64-bit-wide. */
    uint64_t sp_in = 0x10000;
    uint64_t after = addsub_sp(sp_in);
    CHECK(after == sp_in - 0x20, "addsub_sp");

    /* Multiple adds in sequence (forces register allocation) */
    uint64_t acc = 0;
    for (int i = 0; i < 10; i++) {
        acc = acc + 0x1000;  /* each iteration: ADD imm lsl #12 */
    }
    CHECK(acc == 0xA000, "loop_add_imm_lsl12");

    /* Negative immediate SUB (compiler may emit SUB or ADD of negated) */
    int64_t signed_acc = 0;
    for (int i = 0; i < 5; i++) {
        signed_acc = signed_acc - 0x1000;
    }
    CHECK(signed_acc == -0x5000, "loop_sub_imm_lsl12");

    /* ADD with large immediate that doesn't fit in 12 bits but fits with LSL #12 */
    uint64_t big = 0;
    big = big + 0x5000;   /* ADD imm lsl #12 (#5, lsl #12) */
    big = big + 0x5000;
    big = big + 0x5000;
    CHECK(big == 0xF000, "multi_add_imm_lsl12");

    printf("addsub_imm: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
