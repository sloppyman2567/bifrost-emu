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
