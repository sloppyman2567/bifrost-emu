// jit_flags_exit_dead.c — a block that exits with live host flags may skip the NZCV store when the
// successor overwrites all four flags before anything can observe them. Each case branches to successors
// that overwrite the flags first (the store is dead) or read/observe them first (the store is needed):
// CSET/CCMP/ADC, a load (it can fault), MRS, a call. The conditional edge is checked both ways, for
// B.cond (including HI/LS after an ADD), CBZ/TBZ and a plain B, and with FCMP as the overwriter.
// The last section patches a successor of a warmed, chained block: the block must be retired, and may
// be kept when the maintenance leaves the words unchanged.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fails;
static uint32_t nzcv_sub(uint64_t a, uint64_t b) {
    uint64_t r = a - b; uint32_t n = (int64_t)r < 0, z = r == 0, c = a >= b;
    uint32_t v = (int64_t)((a ^ b) & (a ^ r)) < 0;
    return (n << 31) | (z << 30) | (c << 29) | (v << 28);
}
static uint32_t nzcv_add(uint64_t a, uint64_t b) {
    uint64_t r = a + b; uint32_t n = (int64_t)r < 0, z = r == 0, c = r < a;
    uint32_t v = (int64_t)(~(a ^ b) & (a ^ r)) < 0;
    return (n << 31) | (z << 30) | (c << 29) | (v << 28);
}
static uint32_t nzcv_fcmp(double a, double b) {
    if (a != a || b != b) return 0x30000000;
    if (a == b) return 0x60000000;
    return a < b ? 0x80000000 : 0x20000000;
}
#define CHECK(cond, what, a, b) do { if (!(cond)) { ++fails; if (fails < 16) printf("NG %s a=%llx b=%llx\n", what, (unsigned long long)(a), (unsigned long long)(b)); } } while (0)
#define NZ(x) ((x) & 0xf0000000u)

int main(void) {
    static const uint64_t v[] = {0, 1, 2, 0x7f, 0x80, 0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull,
                                 0x8000000000000000ull, 0x8000000000000001ull, 0xfffffffffffffffeull, ~0ull};
    static const double fv[] = {0.0, 1.0, -1.0, 2.5, 1e300, -1e-300, __builtin_nan("")};
    volatile uint64_t cell = 5;
    for (int rep = 0; rep < 3; ++rep)
    for (unsigned i = 0; i < sizeof v / sizeof *v; ++i)
        for (unsigned j = 0; j < sizeof v / sizeof *v; ++j) {
            uint64_t a = v[i], b = v[j], c = v[(i * 5 + j * 3) % 12], d = v[(i + j * 7) % 12];
            uint64_t s;
            // A. fall-through successor overwrites; taken successor reads (MRS).
            __asm__ volatile("cmp %1, %2\n b.eq 1f\n cmp %3, %4\n mrs %0, nzcv\n b 2f\n 1: mrs %0, nzcv\n 2:"
                : "=&r"(s) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(NZ(s) == (a == b ? nzcv_sub(a, b) : nzcv_sub(c, d)), "A", a, b);
            // B. taken successor overwrites; fall-through reads.
            __asm__ volatile("cmp %1, %2\n b.ne 1f\n mrs %0, nzcv\n b 2f\n 1: cmp %3, %4\n mrs %0, nzcv\n 2:"
                : "=&r"(s) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(NZ(s) == (a != b ? nzcv_sub(c, d) : nzcv_sub(a, b)), "B", a, b);
            // C. both successors overwrite, after flag-neutral ALU instructions.
            uint64_t t1 = 3, t2 = 0;
            __asm__ volatile("cmp %2, %3\n b.lt 1f\n add %1, %1, #1\n mov %0, #9\n eor %1, %1, %1\n cmn %4, %5\n mrs %0, nzcv\n b 2f\n"
                             "1: lsl %1, %1, #2\n orr %0, %1, #1\n subs xzr, %5, %4\n mrs %0, nzcv\n 2:"
                : "=&r"(s), "+r"(t1) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(NZ(s) == ((int64_t)a < (int64_t)b ? nzcv_sub(d, c) : nzcv_add(c, d)), "C", a, b);
            // D. a load before the overwrite can fault, so the flags are still needed: a reader is correct either way.
            uint64_t ld;
            __asm__ volatile("cmp %2, %3\n b.eq 1f\n ldr %1, [%6]\n cmp %4, %5\n mrs %0, nzcv\n b 2f\n 1: mrs %0, nzcv\n 2:"
                : "=&r"(s), "=&r"(ld) : "r"(a), "r"(b), "r"(c), "r"(d), "r"(&cell) : "cc", "memory");
            CHECK(NZ(s) == (a == b ? nzcv_sub(a, b) : nzcv_sub(c, d)) && (a == b || ld == 5), "D", a, b);
            // E. flag readers at the successor: CSET, CCMP, ADC see the first compare.
            uint64_t r1, r2, r3;
            __asm__ volatile("cmp %3, %4\n b.hi 1f\n cset %0, lo\n cmp %5, %6\n b 2f\n 1: cset %0, lo\n 2:\n"
                             "cmp %3, %4\n b.eq 3f\n ccmp %5, %6, #4, ne\n cset %1, eq\n b 4f\n 3: ccmp %5, %6, #4, ne\n cset %1, eq\n 4:\n"
                             "cmp %3, %4\n b.ne 5f\n mov %2, #10\n adc %2, %2, xzr\n b 6f\n 5: mov %2, #10\n adc %2, %2, xzr\n 6:"
                : "=&r"(r1), "=&r"(r2), "=&r"(r3) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(r1 == (a < b), "E cset", a, b);
            CHECK(r2 == (a == b ? 1 : (c == d)), "E ccmp", a, b);
            CHECK(r3 == 10 + (a >= b), "E adc", a, b);
            // F. B.HI/LS after ADDS use the inverted carry: dead and live successors on both edges.
            uint64_t s2;
            __asm__ volatile("adds x9, %2, %3\n b.hi 1f\n cmp %4, %5\n mrs %0, nzcv\n b 2f\n 1: mrs %0, nzcv\n 2:\n"
                             "adds x9, %2, %3\n b.ls 3f\n mrs %1, nzcv\n b 4f\n 3: cmp %5, %4\n mrs %1, nzcv\n 4:"
                : "=&r"(s), "=&r"(s2) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc", "x9");
            {
                uint32_t f = nzcv_add(a, b); int hi = (f & 0x20000000) && !(f & 0x40000000);
                CHECK(NZ(s) == (hi ? f : nzcv_sub(c, d)), "F hi", a, b);
                CHECK(NZ(s2) == (!hi ? nzcv_sub(d, c) : f), "F ls", a, b);
            }
            // G. CBZ/TBZ: flags dead at both successors, and live at one.
            uint64_t z = (a & 1) ? 0 : 1;
            __asm__ volatile("cmp %2, %3\n cbz %6, 1f\n cmp %4, %5\n mrs %0, nzcv\n b 2f\n 1: cmn %4, %5\n mrs %0, nzcv\n 2:\n"
                             "cmp %2, %3\n tbz %6, #0, 3f\n mrs %1, nzcv\n b 4f\n 3: cmp %5, %4\n mrs %1, nzcv\n 4:"
                : "=&r"(s), "=&r"(s2) : "r"(a), "r"(b), "r"(c), "r"(d), "r"(z) : "cc");
            CHECK(NZ(s) == (z == 0 ? nzcv_add(c, d) : nzcv_sub(c, d)), "G cbz", a, b);
            CHECK(NZ(s2) == ((z & 1) == 0 ? nzcv_sub(d, c) : nzcv_sub(a, b)), "G tbz", a, b);
            // H. direct B (the flags reach a successor that overwrites them), through a long flag-neutral stretch.
            __asm__ volatile("cmp %1, %2\n b 1f\n nop\n 1: cmp %3, %4\n mrs %0, nzcv"
                : "=&r"(s) : "r"(a), "r"(b), "r"(c), "r"(d) : "cc");
            CHECK(NZ(s) == nzcv_sub(c, d), "H", a, b);
        }
    // I. FCMP as the overwriter, FCMPE and the zero form.
    for (int rep = 0; rep < 3; ++rep)
        for (unsigned i = 0; i < sizeof fv / sizeof *fv; ++i)
            for (unsigned j = 0; j < sizeof fv / sizeof *fv; ++j) {
                double x = fv[i], y = fv[j]; uint64_t s, a = i, b = j;
                __asm__ volatile("cmp %1, %2\n b.eq 1f\n fcmp %d3, %d4\n mrs %0, nzcv\n b 2f\n 1: mrs %0, nzcv\n 2:"
                    : "=&r"(s) : "r"(a), "r"(b), "w"(x), "w"(y) : "cc");
                CHECK(NZ(s) == (a == b ? nzcv_sub(a, b) : nzcv_fcmp(x, y)), "I fcmp", i, j);
                __asm__ volatile("cmp %1, %2\n b.ne 1f\n mrs %0, nzcv\n b 2f\n 1: fcmpe %d3, #0.0\n mrs %0, nzcv\n 2:"
                    : "=&r"(s) : "r"(a), "r"(b), "w"(x) : "cc");
                CHECK(NZ(s) == (a != b ? nzcv_fcmp(x, 0.0) : nzcv_sub(a, b)), "I fcmpe", i, j);
            }
    // J. A warmed, chained block whose dead-flags successor is patched must be retired. The block ends
    // at the last word of a 64-byte line and its successor starts the next one, so maintenance on the
    // successor's line does not touch the block's own words.
    uint32_t* code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { printf("mmap failed\n"); return 2; }
    for (int i = 0; i < 32; ++i) code[i] = 0xD503201F;  // nop
    code[14] = 0xEB01001F;  // cmp x0, x1
    code[15] = 0x540000A0;  // b.eq +20 -> code[20]
    code[16] = 0xEB03005F;  // cmp x2, x3   (the successor that overwrites NZCV)
    code[17] = 0xD53B4200;  // mrs x0, nzcv
    code[18] = 0xD65F03C0;  // ret
    code[20] = 0xD53B4200;  // mrs x0, nzcv
    code[21] = 0xD65F03C0;  // ret
    __builtin___clear_cache((char*)code, (char*)code + 128);
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t) = (uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t))(code + 14);
    // Phases: 0 compile with the CMP successor (dead store); 1 maintenance on the successor's line, words
    // unchanged (the block may be kept); 2 patch the successor to a NOP (the first compare is observed:
    // the block must be retired); 3 patch back to the CMP; 4 maintenance, words unchanged.
    for (int phase = 0; phase < 5; ++phase) {
        if (phase == 2) code[16] = 0xD503201F;
        if (phase == 3) code[16] = 0xEB03005F;
        if (phase) __builtin___clear_cache((char*)(code + 16), (char*)(code + 32));
        for (int n = 0; n < 3000; ++n) {
            uint64_t a = v[n % 12], b = v[(n / 12) % 12], c = v[(n * 7) % 12], d = v[(n * 5 + 1) % 12];
            uint64_t got = f(a, b, c, d);
            uint32_t want = a == b || phase == 2 ? nzcv_sub(a, b) : nzcv_sub(c, d);
            if (NZ(got) != want) { CHECK(0, "J", phase, n); break; }
        }
    }
    printf("jit_flags_exit_dead: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
