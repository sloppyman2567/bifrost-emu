// Flag-setting ops whose two operands are the same register, with that
// register read again in the same block. FWD turns `cmp w23, w23` into
// SUBS v, v; the register allocator once lost v (it was later read from a
// never-written stack slot). ART's boot image inlines Arrays.copyOf as
// `cmp w23, w23; mov w5, w23` and then passed garbage as the
// System.arraycopy length.
#include <stdint.h>
#include <stdio.h>

struct out { uint64_t a, b; };

#define SAME_OP(name, insn, setup)                                         \
    static struct out name(uint64_t v) {                                   \
        struct out o;                                                      \
        __asm__ volatile(                                                  \
            "mov x23, %[v]\n"                                              \
            setup                                                          \
            insn "\n"                                                      \
            "cset w7, eq\n"                                                \
            "mov w5, w23\n"                                                \
            "add x6, x6, x7, lsl #32\n"                                    \
            "stp x5, x6, [%[o]]\n"                                         \
            : : [v] "r"(v), [o] "r"(&o)                                    \
            : "x5", "x6", "x7", "x23", "cc", "memory");                    \
        return o;                                                          \
    }

// .b = result register x6 (0 when unused) + (Z flag << 32)
SAME_OP(cmp_w,  "cmp w23, w23",       "mov x6, #0\n")
SAME_OP(cmp_x,  "cmp x23, x23",       "mov x6, #0\n")
SAME_OP(subs_w, "subs w6, w23, w23",  "")
SAME_OP(adds_x, "adds x6, x23, x23",  "")
SAME_OP(tst_x,  "tst x23, x23",       "mov x6, #0\n")
SAME_OP(adcs_x, "adcs x6, x23, x23",  "cmp xzr, xzr\n")   // C = 1
SAME_OP(sbcs_w, "sbcs w6, w23, w23",  "cmn xzr, xzr\n")   // C = 0 (borrow)

// boot.oat CharsetEncoder.replacement(): the operand is reloaded after an
// in-block call, compared with itself and copied.
static uint64_t keep(uint64_t x) { return x; }
static uint64_t art_copyof(uint64_t len) {
    uint64_t r;
    __asm__ volatile(
        "mov x23, %[len]\n"
        "mov x0, #7\n"
        "blr %[fn]\n"
        "cmp w23, w23\n"
        "mov w5, w23\n"
        "mov x1, x22\n"
        "mov x3, x0\n"
        "mov x23, x3\n"
        "mov %[r], x5\n"
        : [r] "=r"(r)
        : [len] "r"(len), [fn] "r"(keep)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
          "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x23", "x30",
          "cc", "memory");
    return r;
}

// Keep twelve independent values live across flag arithmetic. Check both
// operand aliases, unused CMP/CMN results, W-result zeroing and full-width
// source preservation when a W-form comparison does not write its inputs.
#define PRESSURE_OP(name, instruction)                                    \
    extern void name(uint64_t, uint64_t, uint64_t *);                       \
    __asm__(".text\n.align 4\n.global " #name "\n.type " #name ",%function\n" \
        #name ":\n mov x3,x0\n mov x4,x1\n"                                \
        "add x5,x0,#1\n add x6,x0,#2\n add x7,x0,#3\n add x8,x0,#4\n"       \
        "add x9,x0,#5\n add x10,x0,#6\n add x11,x0,#7\n add x12,x0,#8\n"    \
        "add x13,x0,#9\n add x14,x0,#10\n mov x15,#0\n"                     \
        instruction "\n mrs x17,nzcv\n"                                    \
        "stp x3,x4,[x2]\n stp x5,x6,[x2,#16]\n stp x7,x8,[x2,#32]\n"       \
        "stp x9,x10,[x2,#48]\n stp x11,x12,[x2,#64]\n"                      \
        "stp x13,x14,[x2,#80]\n stp x15,x17,[x2,#96]\n ret\n"              \
        ".size " #name ",.-" #name "\n");
#define PRESSURE_WIDTH(suffix, reg)                                       \
    PRESSURE_OP(pressure_add_##suffix, "adds " reg "15," reg "3," reg "4") \
    PRESSURE_OP(pressure_add_a_##suffix, "adds " reg "3," reg "3," reg "4") \
    PRESSURE_OP(pressure_add_b_##suffix, "adds " reg "4," reg "3," reg "4") \
    PRESSURE_OP(pressure_sub_##suffix, "subs " reg "15," reg "3," reg "4") \
    PRESSURE_OP(pressure_sub_a_##suffix, "subs " reg "3," reg "3," reg "4") \
    PRESSURE_OP(pressure_sub_b_##suffix, "subs " reg "4," reg "3," reg "4") \
    PRESSURE_OP(pressure_cmp_##suffix, "cmp " reg "3," reg "4")            \
    PRESSURE_OP(pressure_cmn_##suffix, "cmn " reg "3," reg "4")            \
    PRESSURE_OP(pressure_tst_##suffix, "tst " reg "3," reg "4")
PRESSURE_WIDTH(x, "x")
PRESSURE_WIDTH(w, "w")

static int check_pressure(void) {
    struct flag_case {
        void (*run)(uint64_t, uint64_t, uint64_t *);
        unsigned bits, subtract, logical, dest;
    };
#define PRESSURE_CASES(suffix, bits)                                      \
        {pressure_add_##suffix, bits, 0, 0, 12},                           \
        {pressure_add_a_##suffix, bits, 0, 0, 0},                          \
        {pressure_add_b_##suffix, bits, 0, 0, 1},                          \
        {pressure_sub_##suffix, bits, 1, 0, 12},                           \
        {pressure_sub_a_##suffix, bits, 1, 0, 0},                          \
        {pressure_sub_b_##suffix, bits, 1, 0, 1},                          \
        {pressure_cmp_##suffix, bits, 1, 0, 14},                           \
        {pressure_cmn_##suffix, bits, 0, 0, 14},                           \
        {pressure_tst_##suffix, bits, 0, 1, 14}
    const struct flag_case cases[] = {PRESSURE_CASES(x, 64), PRESSURE_CASES(w, 32)};
    const uint64_t values[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
    for (unsigned repeat = 0; repeat < 4; ++repeat)
        for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k)
            for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 8; ++j) {
                const struct flag_case *c = &cases[k];
                uint64_t a = values[i], b = values[j], want[14], got[14];
                want[0] = a; want[1] = b;
                for (unsigned n = 0; n < 10; ++n) want[n + 2] = a + n + 1;
                want[12] = 0;
                if (c->bits == 32) { a = (uint32_t)a; b = (uint32_t)b; }
                uint64_t r = c->logical ? a & b : c->subtract ? a - b : a + b;
                if (c->bits == 32) r = (uint32_t)r;
                unsigned n = r >> (c->bits - 1), z = r == 0;
                unsigned carry = c->logical ? 0 : c->subtract ? a >= b :
                    c->bits == 32 ? a + b > UINT32_MAX : r < a;
                unsigned overflow = c->logical ? 0 :
                    ((c->subtract ? a ^ b : ~(a ^ b)) & (a ^ r)) >> (c->bits - 1) & 1;
                want[13] = (uint64_t)(n << 3 | z << 2 | carry << 1 | overflow) << 28;
                if (c->dest < 14) want[c->dest] = r;
                c->run(values[i], values[j], got);
                for (unsigned slot = 0; slot < 14; ++slot) if (got[slot] != want[slot]) {
                    printf("FAIL pressure op=%u i=%u j=%u slot=%u got=%llx want=%llx\n",
                        k, i, j, slot, (unsigned long long)got[slot],
                        (unsigned long long)want[slot]);
                    return 1;
                }
            }
    return 0;
}

static int fails;
static void check(const char* what, uint64_t v, struct out got, uint64_t a, uint64_t b) {
    if (got.a != a || got.b != b) {
        if (fails++ < 10)
            printf("FAIL %s v=%#llx: w5=%#llx (want %#llx) result=%#llx (want %#llx)\n", what,
                   (unsigned long long)v, (unsigned long long)got.a, (unsigned long long)a,
                   (unsigned long long)got.b, (unsigned long long)b);
    }
}

int main(void) {
    if (check_pressure()) return 1;
    const uint64_t values[] = {0, 1, 0x7f, 0x80000000u, 0xffffffffu, 0x123456789abcdef0ull,
                               ~0ull, 0x8000000000000000ull};
    const uint64_t z = 1ull << 32;
    for (int rep = 0; rep < 200; ++rep) {
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            uint64_t v = values[i];
            uint64_t w = (uint32_t)v;
            check("cmp w", v, cmp_w(v), w, z);
            check("cmp x", v, cmp_x(v), w, z);
            check("subs w", v, subs_w(v), w, z);
            check("adds x", v, adds_x(v), w, (v + v) + (v + v == 0 ? z : 0));
            check("tst x", v, tst_x(v), w, v == 0 ? z : 0);
            check("adcs x", v, adcs_x(v), w, (v + v + 1) + (v + v + 1 == 0 ? z : 0));
            uint32_t sb = (uint32_t)(w - w - 1);
            check("sbcs w", v, sbcs_w(v), w, sb + (sb == 0 ? z : 0));
            uint64_t r = art_copyof(v);
            if (r != w && fails++ < 10)
                printf("FAIL art copyOf len=%#llx: w5=%#llx\n", (unsigned long long)v,
                       (unsigned long long)r);
        }
    }
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    puts("ALL PASS: same-operand flag ops keep the forwarded operand");
    return 0;
}
