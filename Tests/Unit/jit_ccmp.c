// jit_ccmp.c — Test 32-bit CCMP/CCMN flag computation.
//
// This test validates the Turn 56 fix: the JIT was using 64-bit sub/add
// for 32-bit CCMP/CCMN, which computed the Sign Flag from bit 63 instead
// of bit 31. For a 32-bit operation like `ccmp w3, #2, #0, cs` where
// w3=0xFFFFFFFF, the 64-bit sub gives SF=0 (positive) while the 32-bit
// sub gives SF=1 (negative). This caused the ARM N flag to be wrong.
//
// Build: make cross SRC=Tests/Unit/jit_ccmp.c OUT=Tests/Unit/jit_ccmp.elf
// Run:   ./bifrost-emu Tests/Unit/jit_ccmp.elf
//        ./bifrost-emu --no-jit Tests/Unit/jit_ccmp.elf
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

// Test CCMP with 32-bit values that have bit 31 set.
// The JIT must compute N from bit 31, not bit 63.
static int test_ccmp_32bit_negative(void) {
    // Set up: w0 = 0xFFFFFFFF (32-bit, bit 31 set, but zero-extended to 64 bits
    // so bit 63 is clear). cmp w0, #0 → C=1 (no borrow, 0xFFFFFFFF >= 0).
    // Then ccmp w1, #2, #0, cs → since C=1, do cmp w1, #2.
    // If w1 = 0xFFFFFFFF: w1 - 2 = 0xFFFFFFFD. N=1 (bit 31 set), C=1 (no borrow).
    //
    // The bug: 64-bit sub gives 0x00000000FFFFFFFD, SF=0 → N=0 (wrong).
    // The fix: 32-bit sub gives 0xFFFFFFFD, SF=1 → N=1 (correct).
    uint32_t w0 = 0xFFFFFFFF;
    uint32_t w1 = 0xFFFFFFFF;
    uint64_t pstate;

    __asm__ volatile (
        "mov w0, %w[val0]\n"
        "mov w1, %w[val1]\n"
        "cmp w0, #0\n"           // sets C=1 (w0 >= 0)
        "ccmp w1, #2, #0, cs\n"  // CS=true → cmp w1, #2
        "mrs %[ps], nzcv\n"      // read NZCV flags
        : [ps] "=r"(pstate)
        : [val0] "r"(w0), [val1] "r"(w1)
        : "w0", "w1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 32-bit neg: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 1 && c == 1);
}

// Test CCMP with 32-bit values that are small positive.
static int test_ccmp_32bit_positive(void) {
    uint32_t w0 = 0xFFFFFFFF;
    uint32_t w1 = 5;  // w1 - 2 = 3, positive
    uint64_t pstate;

    __asm__ volatile (
        "mov w0, %w[val0]\n"
        "mov w1, %w[val1]\n"
        "cmp w0, #0\n"
        "ccmp w1, #2, #0, cs\n"
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [val0] "r"(w0), [val1] "r"(w1)
        : "w0", "w1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 32-bit pos: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 0 && c == 1);
}

// Test CCMP condition FALSE path (NZCV = immediate).
// CCMP semantics: if condition is TRUE → do the compare; if FALSE → set NZCV=imm.
// To test the FALSE path, we need the condition to be FALSE.
// NE is FALSE when Z=1 (equal). So: cmp w0, #5 with w0=5 → Z=1 → NE=false.
// Then ccmp w1, #2, #0xf, ne → NE=false → set NZCV=0xF.
static int test_ccmp_cond_false(void) {
    volatile uint32_t w0_val = 5;  // equal to cmp value → Z=1 → NE=false
    volatile uint32_t w1 = 42;
    volatile uint64_t pstate = 0;

    __asm__ volatile (
        "mov w0, %w[v0]\n"
        "mov w1, %w[v1]\n"
        "cmp w0, #5\n"            // 5-5=0: Z=1, NE is FALSE
        "ccmp w1, #2, #0xf, ne\n" // NE=false → set NZCV=0xF
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [v0] "r"((uint32_t)w0_val), [v1] "r"((uint32_t)w1)
        : "w0", "w1", "cc", "memory"
    );

    // NZCV = 0xF = N=1, Z=1, C=1, V=1
    uint64_t n = (pstate >> 31) & 1;
    uint64_t z = (pstate >> 30) & 1;
    uint64_t c = (pstate >> 29) & 1;
    uint64_t v = (pstate >> 28) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp cond-false(ne): N=%lld Z=%lld C=%lld V=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)z,
                       (unsigned long long)c, (unsigned long long)v,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 1 && z == 1 && c == 1 && v == 1);
}

// Test 64-bit CCMP still works.
static int test_ccmp_64bit(void) {
    uint64_t x0 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t x1 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t pstate;

    __asm__ volatile (
        "mov x0, %[val0]\n"
        "mov x1, %[val1]\n"
        "cmp x0, #0\n"
        "ccmp x1, #2, #0, cs\n"
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [val0] "r"(x0), [val1] "r"(x1)
        : "x0", "x1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 64-bit: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    // x1=0xFFFFFFFFFFFFFFFF, x1-2 = 0xFFFFFFFFFFFFFFFD. N=1, C=1 (no borrow).
    return (n == 1 && c == 1);
}

// Every condition, with the incoming flags in memory (MSR) or in the host
// (CMP: subtraction carry, CMN: addition carry), for CCMP and CCMN in both widths
// with register, immediate and XZR operands, against a C model of NZCV. The
// result is read by all fourteen CSET conditions, a B.HI, MRS and the operand
// registers afterwards (the immediate path must not lose a live register).
static unsigned nzcv_sub(uint64_t a, uint64_t b, int w32) {
    if (w32) {
        uint32_t x = (uint32_t)a, y = (uint32_t)b, r = x - y;
        return (r >> 31) << 3 | (r == 0) << 2 | (x >= y) << 1 | (((x ^ y) & (x ^ r)) >> 31);
    }
    uint64_t r = a - b;
    return (unsigned)(r >> 63) << 3 | (r == 0) << 2 | (a >= b) << 1 | (unsigned)(((a ^ b) & (a ^ r)) >> 63);
}
static unsigned nzcv_add(uint64_t a, uint64_t b, int w32) {
    if (w32) {
        uint32_t x = (uint32_t)a, y = (uint32_t)b, r = x + y;
        return (r >> 31) << 3 | (r == 0) << 2 | (r < x) << 1 | ((~(x ^ y) & (x ^ r)) >> 31);
    }
    uint64_t r = a + b;
    return (unsigned)(r >> 63) << 3 | (r == 0) << 2 | (r < a) << 1 | (unsigned)((~(a ^ b) & (a ^ r)) >> 63);
}
static int cond_holds(unsigned f, int c) {
    int n = f >> 3 & 1, z = f >> 2 & 1, cy = f >> 1 & 1, v = f & 1, r;
    switch (c >> 1) {
        case 0: r = z; break;
        case 1: r = cy; break;
        case 2: r = n; break;
        case 3: r = v; break;
        case 4: r = cy && !z; break;
        case 5: r = n == v; break;
        case 6: r = !z && n == v; break;
        default: return 1;  // AL and NV
    }
    return (c & 1) ? !r : r;
}

enum { ENTRY_MSR, ENTRY_CMP, ENTRY_CMN };
enum { FORM_REG, FORM_IMM, FORM_ZR1, FORM_ZR2 };
#define CCMP_IMM 13
#define CSETS \
    "cset w9, eq\n cset w10, ne\n cset w11, cs\n cset w12, cc\n cset w13, mi\n cset w14, pl\n" \
    "cset w15, vs\n cset w16, vc\n cset w17, hi\n cset w18, ls\n cset w19, ge\n cset w20, lt\n" \
    "cset w21, gt\n cset w22, le\n"
#define PACK \
    "orr %w[m], w9, w10, lsl #1\n orr %w[m], %w[m], w11, lsl #2\n orr %w[m], %w[m], w12, lsl #3\n" \
    "orr %w[m], %w[m], w13, lsl #4\n orr %w[m], %w[m], w14, lsl #5\n orr %w[m], %w[m], w15, lsl #6\n" \
    "orr %w[m], %w[m], w16, lsl #7\n orr %w[m], %w[m], w17, lsl #8\n orr %w[m], %w[m], w18, lsl #9\n" \
    "orr %w[m], %w[m], w19, lsl #10\n orr %w[m], %w[m], w20, lsl #11\n orr %w[m], %w[m], w21, lsl #12\n" \
    "orr %w[m], %w[m], w22, lsl #13\n"
typedef struct { uint64_t ps, mask, sum; } ccmp_out;
typedef ccmp_out (*ccmp_fn)(uint64_t e0, uint64_t e1, uint64_t a, uint64_t b);
#define ENTRY_ASM_MSR "msr nzcv, %[e0]\n"
#define ENTRY_ASM_CMP "cmp %[e0], %[e1]\n"
#define ENTRY_ASM_CMN "cmn %[e0], %[e1]\n"
#define DEF_FN(E, CN, OP, W, FORM, OPS, NZ) \
    static ccmp_out fn_##E##_##CN##_##OP##_##W##_##FORM(uint64_t e0, uint64_t e1, uint64_t a, uint64_t b) { \
        ccmp_out o; \
        __asm__ volatile(ENTRY_ASM_##E #OP " " OPS ", #" #NZ ", " #CN "\n" CSETS \
                         "b.hi 1f\n mov w23, #0\n b 2f\n1: mov w23, #1\n2: mrs %[ps], nzcv\n" PACK \
                         "orr %w[m], %w[m], w23, lsl #14\n add %[s], %[a], %[b]\n" \
                         : [ps] "=&r"(o.ps), [m] "=&r"(o.mask), [s] "=&r"(o.sum) \
                         : [e0] "r"(e0), [e1] "r"(e1), [a] "r"(a), [b] "r"(b) \
                         : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
                           "x19", "x20", "x21", "x22", "x23", "cc"); \
        return o; \
    }
#define ROW(E, CN, OP, W, FORM, OPS, NZ) \
    {fn_##E##_##CN##_##OP##_##W##_##FORM, ENTRY_##E, CI_##CN, NZ, #OP[3] == 'n', W == 32, FORM_##FORM, #E " " #OP " w" #W " " #FORM " " #CN},
// The immediate NZCV differs per condition so every value 0-15 is tested.
#define CI_eq 0
#define CI_ne 1
#define CI_cs 2
#define CI_cc 3
#define CI_mi 4
#define CI_pl 5
#define CI_vs 6
#define CI_vc 7
#define CI_hi 8
#define CI_ls 9
#define CI_ge 10
#define CI_lt 11
#define CI_gt 12
#define CI_le 13
#define CI_al 14
#define CI_nv 15
#define FORMS(X, E, CN, NZ) \
    X(E, CN, ccmp, 64, REG, "%[a], %[b]", NZ)  X(E, CN, ccmp, 32, REG, "%w[a], %w[b]", NZ) \
    X(E, CN, ccmn, 64, REG, "%[a], %[b]", NZ)  X(E, CN, ccmn, 32, REG, "%w[a], %w[b]", NZ) \
    X(E, CN, ccmp, 64, IMM, "%[a], #13", NZ)   X(E, CN, ccmp, 32, IMM, "%w[a], #13", NZ) \
    X(E, CN, ccmn, 64, IMM, "%[a], #13", NZ)   X(E, CN, ccmn, 32, IMM, "%w[a], #13", NZ) \
    X(E, CN, ccmp, 64, ZR1, "xzr, %[b]", NZ)   X(E, CN, ccmn, 32, ZR1, "wzr, %w[b]", NZ) \
    X(E, CN, ccmp, 32, ZR2, "%w[a], wzr", NZ)  X(E, CN, ccmn, 64, ZR2, "%[a], xzr", NZ)
#define CONDS(X, E) \
    FORMS(X, E, eq, 5) FORMS(X, E, ne, 4) FORMS(X, E, cs, 7) FORMS(X, E, cc, 6) \
    FORMS(X, E, mi, 1) FORMS(X, E, pl, 0) FORMS(X, E, vs, 3) FORMS(X, E, vc, 2) \
    FORMS(X, E, hi, 13) FORMS(X, E, ls, 12) FORMS(X, E, ge, 15) FORMS(X, E, lt, 14) \
    FORMS(X, E, gt, 9) FORMS(X, E, le, 8) FORMS(X, E, al, 11) FORMS(X, E, nv, 10)
#define ALL(X) CONDS(X, MSR) CONDS(X, CMP) CONDS(X, CMN)
ALL(DEF_FN)
static const struct {
    ccmp_fn fn;
    int entry, cond, nzcv, add, w32, form;
    const char* name;
} ccmp_cases[] = { ALL(ROW) };

static int test_ccmp_table(void) {
    static const uint64_t vals[] = {0, 1, CCMP_IMM, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
                                    0x7fffffffffffffffULL, 0x8000000000000000ULL,
                                    0xfffffffffffffff3ULL};
    static const uint64_t host_entry[][2] = {
        {5, 3}, {3, 5}, {7, 7}, {0x8000000000000000ULL, 1}, {0x7fffffffffffffffULL, 0xffffffffffffffffULL},
        {0xffffffffffffffffULL, 1}};
    const int nv = sizeof(vals) / sizeof(vals[0]);
    const int nh = sizeof(host_entry) / sizeof(host_entry[0]);
    int bad = 0;
    for (unsigned k = 0; k < sizeof(ccmp_cases) / sizeof(ccmp_cases[0]); ++k) {
        const int ne = ccmp_cases[k].entry == ENTRY_MSR ? 16 : nh;
        for (int e = 0; e < ne; ++e) {
            uint64_t e0, e1 = 0;
            unsigned in;
            if (ccmp_cases[k].entry == ENTRY_MSR) {
                e0 = (uint64_t)e << 28;
                in = (unsigned)e;
            } else {
                e0 = host_entry[e][0];
                e1 = host_entry[e][1];
                in = ccmp_cases[k].entry == ENTRY_CMP ? nzcv_sub(e0, e1, 0) : nzcv_add(e0, e1, 0);
            }
            for (int i = 0; i < nv; ++i) {
                for (int j = 0; j < nv; ++j) {
                    const uint64_t a = vals[i], b = vals[j];
                    const int form = ccmp_cases[k].form;
                    const uint64_t x = form == FORM_ZR1 ? 0 : a;
                    const uint64_t y = form == FORM_IMM ? CCMP_IMM : form == FORM_ZR2 ? 0 : b;
                    unsigned want = (unsigned)ccmp_cases[k].nzcv;
                    if (cond_holds(in, ccmp_cases[k].cond))
                        want = ccmp_cases[k].add ? nzcv_add(x, y, ccmp_cases[k].w32)
                                                 : nzcv_sub(x, y, ccmp_cases[k].w32);
                    uint64_t want_mask = 0;
                    for (int c = 0; c < 14; ++c) want_mask |= (uint64_t)cond_holds(want, c) << c;
                    want_mask |= (uint64_t)cond_holds(want, 8) << 14;
                    const ccmp_out o = ccmp_cases[k].fn(e0, e1, a, b);
                    if ((o.ps >> 28) != want || o.mask != want_mask || o.sum != a + b) {
                        if (bad++ < 8) {
                            char msg[200];
                            int len = snprintf(msg, sizeof(msg),
                                "table %s: in=%x a=%llx b=%llx: nzcv %llx want %x, csets %llx want %llx, sum %s\n",
                                ccmp_cases[k].name, in, (unsigned long long)a, (unsigned long long)b,
                                (unsigned long long)(o.ps >> 28), want, (unsigned long long)o.mask,
                                (unsigned long long)want_mask, o.sum == a + b ? "ok" : "WRONG");
                            write(2, msg, len);
                        }
                    }
                }
            }
        }
    }
    return bad == 0;
}

int main(void) {
    CHECK(test_ccmp_32bit_negative(), "32-bit CCMP with negative result: N=1 C=1");
    CHECK(test_ccmp_32bit_positive(), "32-bit CCMP with positive result: N=0 C=1");
    CHECK(test_ccmp_cond_false(), "CCMP condition false: NZCV=0xF");
    CHECK(test_ccmp_64bit(), "64-bit CCMP with negative result: N=1 C=1");
    CHECK(test_ccmp_table(), "CCMP/CCMN table: every condition, flag source, width and form");

    char msg[128];
    int len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
