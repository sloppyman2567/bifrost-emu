// jit_imm_flags_zext.c — flag-setting instructions with an immediate operand and zero-extension of 32-bit
// results. SUBS/ADDS/CMP/CMN/TST/ANDS with immediates of both widths (including 12-bit shifted forms and logical
// immediates) fold the constant into the x86 instruction; W results and 32-bit loads must reach the 64-bit
// register zero-extended even when the source registers hold garbage in bits 63:32. Every result, NZCV after
// the instruction and the branch taken on the flags is hashed; the hash is QEMU's.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t h;
static void mix(uint64_t x) { h = (h ^ x) * 1099511628211ull; h ^= h >> 31; }
static const uint64_t vals[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0xfff, 0x1000, 0xfff000, 0x7fffffff, 0x80000000ull,
    0xffffffffull, 0x100000000ull, 0x1ffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull,
    0xdeadbeef00000001ull, 0xdeadbeef80000000ull, 0xffffffff00000000ull, 0xaaaaaaaaaaaaaaaaull, ~0ull};
#define NV (sizeof vals / sizeof *vals)
static uint64_t nz(void) { uint64_t s; __asm__ volatile("mrs %0, nzcv" : "=r"(s)); return s >> 28; }

#define ARITH(ins, w, imm) for (unsigned i = 0; i < NV; ++i) { uint64_t r, s, br; \
    __asm__ volatile(#ins " %" w "0, %" w "3, #" imm "\n mrs %1, nzcv\n mov %2, #0\n b.hi 1f\n b.lt 2f\n b 3f\n 1: mov %2, #1\n b 3f\n 2: mov %2, #2\n 3:" \
        : "=&r"(r), "=&r"(s), "=&r"(br) : "r"(vals[i]) : "cc"); mix(r); mix(s >> 28); mix(br); }
#define CMPI(ins, w, imm) for (unsigned i = 0; i < NV; ++i) { uint64_t s, br; \
    __asm__ volatile(#ins " %" w "2, #" imm "\n mrs %0, nzcv\n mov %1, #0\n b.hi 1f\n b.lt 2f\n b 3f\n 1: mov %1, #1\n b 3f\n 2: mov %1, #2\n 3:" \
        : "=&r"(s), "=&r"(br) : "r"(vals[i]) : "cc"); mix(s >> 28); mix(br); }
#define TSTI(w, imm) for (unsigned i = 0; i < NV; ++i) { uint64_t s, br; \
    __asm__ volatile("tst %" w "2, #" imm "\n mrs %0, nzcv\n mov %1, #0\n b.eq 1f\n b 2f\n 1: mov %1, #1\n 2:" \
        : "=&r"(s), "=&r"(br) : "r"(vals[i]) : "cc"); mix(s >> 28); mix(br); }
#define ANDSI(w, imm) for (unsigned i = 0; i < NV; ++i) { uint64_t r, s; \
    __asm__ volatile("ands %" w "0, %" w "2, #" imm "\n mrs %1, nzcv" : "=&r"(r), "=&r"(s) : "r"(vals[i]) : "cc"); mix(r); mix(s >> 28); }

static void immediates(void) {
    ARITH(subs, "x", "0")
    ARITH(subs, "x", "1")
    ARITH(subs, "x", "7")
    ARITH(subs, "x", "255")
    ARITH(subs, "x", "4095")
    ARITH(subs, "x", "1, lsl #12")
    ARITH(subs, "x", "4095, lsl #12")
    ARITH(adds, "x", "0")
    ARITH(adds, "x", "1")
    ARITH(adds, "x", "7")
    ARITH(adds, "x", "255")
    ARITH(adds, "x", "4095")
    ARITH(adds, "x", "1, lsl #12")
    ARITH(adds, "x", "4095, lsl #12")
    CMPI(cmp, "x", "0")
    CMPI(cmp, "x", "1")
    CMPI(cmp, "x", "7")
    CMPI(cmp, "x", "255")
    CMPI(cmp, "x", "4095")
    CMPI(cmp, "x", "1, lsl #12")
    CMPI(cmp, "x", "4095, lsl #12")
    CMPI(cmn, "x", "0")
    CMPI(cmn, "x", "1")
    CMPI(cmn, "x", "7")
    CMPI(cmn, "x", "255")
    CMPI(cmn, "x", "4095")
    CMPI(cmn, "x", "1, lsl #12")
    CMPI(cmn, "x", "4095, lsl #12")
    ARITH(subs, "w", "0")
    ARITH(subs, "w", "1")
    ARITH(subs, "w", "7")
    ARITH(subs, "w", "255")
    ARITH(subs, "w", "4095")
    ARITH(subs, "w", "1, lsl #12")
    ARITH(subs, "w", "4095, lsl #12")
    ARITH(adds, "w", "0")
    ARITH(adds, "w", "1")
    ARITH(adds, "w", "7")
    ARITH(adds, "w", "255")
    ARITH(adds, "w", "4095")
    ARITH(adds, "w", "1, lsl #12")
    ARITH(adds, "w", "4095, lsl #12")
    CMPI(cmp, "w", "0")
    CMPI(cmp, "w", "1")
    CMPI(cmp, "w", "7")
    CMPI(cmp, "w", "255")
    CMPI(cmp, "w", "4095")
    CMPI(cmp, "w", "1, lsl #12")
    CMPI(cmp, "w", "4095, lsl #12")
    CMPI(cmn, "w", "0")
    CMPI(cmn, "w", "1")
    CMPI(cmn, "w", "7")
    CMPI(cmn, "w", "255")
    CMPI(cmn, "w", "4095")
    CMPI(cmn, "w", "1, lsl #12")
    CMPI(cmn, "w", "4095, lsl #12")
    TSTI("x", "0xff")
    TSTI("x", "0xffffffff")
    TSTI("x", "0x8000000000000000")
    TSTI("x", "0xaaaaaaaaaaaaaaaa")
    TSTI("x", "0x7fffffffffffffff")
    TSTI("x", "0x1")
    TSTI("w", "0xff")
    TSTI("w", "0x80000000")
    TSTI("w", "0xaaaaaaaa")
    TSTI("w", "0xffff0000")
    TSTI("w", "0x1")
    ANDSI("x", "0xff")
    ANDSI("x", "0xffffffff")
    ANDSI("x", "0x8000000000000000")
    ANDSI("x", "0xaaaaaaaaaaaaaaaa")
    ANDSI("x", "0x7fffffffffffffff")
    ANDSI("x", "0x1")
    ANDSI("w", "0xff")
    ANDSI("w", "0x80000000")
    ANDSI("w", "0xaaaaaaaa")
    ANDSI("w", "0xffff0000")
    ANDSI("w", "0x1")
}

// 32-bit values from memory and from W-form ALU ops used by 64-bit consumers.
static void zero_extension(void) {
    static uint8_t mem[64] __attribute__((aligned(16)));
    for (unsigned k = 0; k < NV; ++k) {
        uint64_t a = vals[k], b = vals[(k * 7 + 3) % NV];
        memcpy(mem, &a, 8); memcpy(mem + 8, &b, 8);
        uint64_t w1, w2, w3, w4, w5, w6, x1, x2, x3, x4, x5, x6;
        __asm__ volatile(
            "ldr %w[w1], [%[m]]\n ldr %w[w2], [%[m], #8]\n ldrb %w[w3], [%[m]]\n ldrh %w[w4], [%[m], #8]\n"
            "ldp %w[w5], %w[w6], [%[m]]\n"
            "add %[x1], %[w1], %[w2]\n eor %[x2], %[w1], %[w2]\n and %[x3], %[w1], %[w2]\n orr %[x4], %[w3], %[w4]\n"
            "lsl %[x5], %[w1], #3\n add %[x6], %[w5], %[w6]\n"
            : [w1] "=&r"(w1), [w2] "=&r"(w2), [w3] "=&r"(w3), [w4] "=&r"(w4), [w5] "=&r"(w5), [w6] "=&r"(w6),
              [x1] "=&r"(x1), [x2] "=&r"(x2), [x3] "=&r"(x3), [x4] "=&r"(x4), [x5] "=&r"(x5), [x6] "=&r"(x6)
            : [m] "r"(mem) : "memory");
        mix(w1); mix(w2); mix(w3); mix(w4); mix(w5); mix(w6); mix(x1); mix(x2); mix(x3); mix(x4); mix(x5); mix(x6);
        // W-form ALU results feeding 64-bit operations
        uint64_t r1, r2, r3, r4;
        __asm__ volatile(
            "subs %w0, %w4, #1\n add %1, %0, %0\n adds %w2, %w4, #3\n add %3, %2, #1"
            : "=&r"(r1), "=&r"(r2), "=&r"(r3), "=&r"(r4) : "r"(a) : "cc");
        mix(r1); mix(r2); mix(r3); mix(r4);
        // 64-bit results, which may have bits above 31, read back through W-form operations
        uint64_t y1, y2, y3, y4, y5, y6;
        __asm__ volatile(
            "subs %[r1], %[a], #1\n mov %w[y1], %w[r1]\n adds %[r2], %[a], %[b]\n uxtw %[y2], %w[r2]\n"
            "sub %[r3], %[a], %[b]\n and %w[y3], %w[r3], %w[r3]\n add %[r4], %[a], #5\n orr %w[y4], %w[r4], %w[r4]\n"
            "ldr %[r5], [%[m]]\n mov %w[y5], %w[r5]\n eor %[r6], %[a], %[b]\n mov %w[y6], %w[r6]\n"
            : [r1] "=&r"(r1), [r2] "=&r"(r2), [r3] "=&r"(r3), [r4] "=&r"(r4), [r5] "=&r"(w1), [r6] "=&r"(w2),
              [y1] "=&r"(y1), [y2] "=&r"(y2), [y3] "=&r"(y3), [y4] "=&r"(y4), [y5] "=&r"(y5), [y6] "=&r"(y6)
            : [a] "r"(a), [b] "r"(b), [m] "r"(mem) : "cc", "memory");
        mix(r1); mix(r2); mix(r3); mix(r4); mix(w1); mix(w2); mix(y1); mix(y2); mix(y3); mix(y4); mix(y5); mix(y6);
        uint64_t q1, q2;
        __asm__ volatile("subs %w0, %w2, %w3\n lsr %1, %0, #31" : "=&r"(q1), "=&r"(q2) : "r"(a), "r"(b) : "cc");
        mix(q1); mix(q2);
    }
}

int main(void) {
    h = 1469598103934665603ull;
    immediates();
    zero_extension();
    static const uint64_t want = 0x68b2df78cbaec314ull;
    if (h != want) { printf("NG hash %016llx want %016llx\n", (unsigned long long)h, (unsigned long long)want); }
    printf("jit_imm_flags_zext: %s\n", h == want ? "ALL PASS" : "FAILED");
    return h != want;
}
