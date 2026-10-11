/*
 * jit_ldp_stp.c — JIT native LDP/STP (paired load/store) tests.
 *
 * Background:
 *   alpha.4 added native codegen for GPR LDP/STP (previously these
 *   went through CALL_INTERP).  The native path must:
 *
 *     - handle pre-index, post-index, and offset addressing
 *     - correctly update the base register for pre/post forms
 *     - preserve 64-bit alignment semantics (LE pair ordering)
 *     - handle SP as base (needs 64-bit-wide addressing)
 *     - handle the 32-bit (W) form
 *
 * A bug in any of these breaks function prologues/epilogues for any
 * real program compiled at -O2 (Clang loves STP for spills).
 *
 * Uses pure C with function calls, struct pass-by-value, and
 * recursion.  The compiler emits LDP/STP for these patterns.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

/* Function with many locals → compiler spills them via STP/LDP. */
__attribute__((noinline))
static uint64_t sum_pair(uint64_t a, uint64_t b) {
    volatile uint64_t x = a, y = b;
    return x + y;
}

__attribute__((noinline))
static uint64_t sum_quad(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    volatile uint64_t w = a, x = b, y = c, z = d;
    return w + x + y + z;
}

/* 8-arg function — args 5-8 are passed on stack via LDP/STP. */
__attribute__((noinline))
static uint64_t sum_oct(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                       uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + b + c + d + e + f + g + h;
}

/* Struct return / pass-by-value — compiler emits LDP/STP for the
 * 128-bit struct. */
typedef struct { uint64_t lo, hi; } u128_t;

__attribute__((noinline))
static u128_t make_u128(uint64_t lo, uint64_t hi) {
    u128_t r;
    r.lo = lo;
    r.hi = hi;
    return r;
}

__attribute__((noinline))
static uint64_t read_lo(u128_t v) { return v.lo; }

__attribute__((noinline))
static uint64_t read_hi(u128_t v) { return v.hi; }

/* Recursive function — every call saves x29/x30 via STP at frame
 * setup, restores via LDP at return.  If LDP/STP is broken, this
 * crashes or returns wrong values. */
__attribute__((noinline))
static int fib_recursive(int n) {
    if (n < 2) return n;
    return fib_recursive(n - 1) + fib_recursive(n - 2);
}

int main(void) {
    /* ── Basic paired load/store via volatile ─────────────────────── */
    static uint64_t buf[8];
    memset(buf, 0, sizeof(buf));

    buf[0] = 0x1111111111111111ULL;
    buf[1] = 0x2222222222222222ULL;
    CHECK(buf[0] == 0x1111111111111111ULL && buf[1] == 0x2222222222222222ULL,
          "stp_basic");

    uint64_t x = buf[0], y = buf[1];
    CHECK(x == 0x1111111111111111ULL && y == 0x2222222222222222ULL,
          "ldp_basic");

    /* ── Function call with 4 args ────────────────────────────────── */
    uint64_t s4 = sum_quad(10, 20, 30, 40);
    CHECK(s4 == 100, "stp_args4");

    s4 = sum_quad(0xFFFFFFFFFFFFFFFFULL, 1, 0, 0);
    CHECK(s4 == 0, "stp_args4_overflow");

    /* ── Function call with 8 args (stack-passed args 5-8) ────────── */
    uint64_t s8 = sum_oct(1, 2, 3, 4, 5, 6, 7, 8);
    CHECK(s8 == 36, "stp_args8_stack");

    /* ── Struct return / pass-by-value (forces LDP for return reg) ── */
    u128_t v = make_u128(0xAAAABBBBCCCCDDDDULL, 0x1111222233334444ULL);
    CHECK(read_lo(v) == 0xAAAABBBBCCCCDDDDULL, "ldp_struct_lo");
    CHECK(read_hi(v) == 0x1111222233334444ULL, "ldp_struct_hi");

    /* Modify the struct (pass-by-value must not see caller's modifications) */
    u128_t v2 = v;
    v2.lo = 0;
    CHECK(read_lo(v) == 0xAAAABBBBCCCCDDDDULL, "struct_by_value_independent");

    /* ── Stack spill pattern: many locals, all used ───────────────── */
    /* The compiler spills these via STP and reloads via LDP. */
    volatile uint64_t a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    uint64_t sum = a + b + c + d + e + f + g + h;
    CHECK(sum == 36, "spill_reload");

    /* ── 32-bit W-form (store/load pair of W regs) ────────────────── */
    static uint32_t wbuf[8];
    memset(wbuf, 0, sizeof(wbuf));
    wbuf[0] = 0xCAFEBABE;
    wbuf[1] = 0xDEADBEEF;
    uint32_t wx = wbuf[0], wy = wbuf[1];
    CHECK(wx == 0xCAFEBABE && wy == 0xDEADBEEF, "ldp_w_form");

    /* ── Recursive function (stack-heavy, frame setup uses STP) ───── */
    CHECK(fib_recursive(10) == 55, "fib_recursive_10");
    CHECK(fib_recursive(20) == 6765, "fib_recursive_20");

    /* ── Deep recursion (stresses stack growth) ───────────────────── */
    /* fib(25) makes ~242785 calls — if STP/LDP is corrupting the frame
     * pointer, this will crash or produce wrong values. */
    CHECK(fib_recursive(25) == 75025, "fib_recursive_25");

    /* ── Large frame (stresses STP-based spills) ──────────────────── */
    /* A function with many locals forces the compiler to spill them via
     * STP.  If STP is mis-decoded (e.g. wrong writeback), values get
     * lost or corrupted. */
    volatile uint64_t arr[16];
    for (int i = 0; i < 16; i++) arr[i] = i * i;
    uint64_t sq_sum = 0;
    for (int i = 0; i < 16; i++) sq_sum += arr[i];
    /* 0+1+4+9+16+25+36+49+64+81+100+121+144+169+196+225 = 1240 */
    CHECK(sq_sum == 1240, "large_frame_spill");

    printf("ldp_stp: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
