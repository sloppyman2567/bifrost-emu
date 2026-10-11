// Bitfield results must survive register pressure, memory helpers, and
// tier-2 side exits without an eager write to their scratch stack slots.
#include <stdint.h>
#include <stdio.h>

// Keep the source live after MUL, so immediate lowering must preserve it.
#define MUL_CASE(name, load, multiply) \
    extern uint64_t name(uint64_t); \
    __asm__(".text\n.align 4\n.global " #name "\n.type " #name ",%function\n" \
        #name ":\n " load "\n " multiply "\n eor x0,x0,x3\n ret\n" \
        ".size " #name ",.-" #name "\n");
MUL_CASE(mul_byte, "mov x2,#127", "mul x3,x0,x2")
MUL_CASE(mul_sbyte, "mov x2,#-128", "mul x3,x0,x2")
MUL_CASE(mul_imm32, "mov x2,#0x5678\n movk x2,#0x1234,lsl #16", "mul x3,x0,x2")
MUL_CASE(mul_negative, "mov x2,#-129", "mul x3,x0,x2")
MUL_CASE(mul_wide, "mov x2,#1\n movk x2,#1,lsl #32", "mul x3,x0,x2")
MUL_CASE(mul_unsigned32, "mov x2,#0x80000000", "mul x3,x0,x2")
MUL_CASE(mul_word, "mov w2,#0x80000000", "mul w3,w0,w2")

static int check_multipliers(void) {
    uint64_t (*functions[])(uint64_t) = {mul_byte, mul_sbyte, mul_imm32,
        mul_negative, mul_wide, mul_unsigned32, mul_word};
    const uint64_t factors[] = {127, (uint64_t)-128, 0x12345678,
        (uint64_t)-129, UINT64_C(0x100000001), 0x80000000, 0x80000000};
    const uint64_t values[] = {0, 1, 127, 128, UINT64_MAX,
        UINT64_C(0x8000000000000000), UINT64_C(0xfedcba9876543210)};
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        for (unsigned f = 0; f < 7; ++f) for (unsigned v = 0; v < 7; ++v) {
            uint64_t product = values[v] * factors[f];
            if (f == 6) product = (uint32_t)product;
            uint64_t want = values[v] ^ product, got = functions[f](values[v]);
            if (got != want) {
                printf("FAIL multiply f=%u v=%u got=%llx want=%llx\n", f, v,
                    (unsigned long long)got, (unsigned long long)want);
                return 1;
            }
        }
    }
    return 0;
}

static uint64_t input[256], output[256];

// Fifteen simultaneously live GPRs, with a body longer than the original
// 32-instruction cap. Exercise spills and aliases across adaptive cuts.
extern void long_pressure(const uint64_t *, uint64_t *, uint64_t);
__asm__(".text\n.align 4\n.global long_pressure\n.type long_pressure,%function\n"
    "long_pressure:\n ldp x3,x4,[x0]\n ldp x5,x6,[x0,#16]\n"
    "ldp x7,x8,[x0,#32]\n ldp x9,x10,[x0,#48]\n ldp x11,x12,[x0,#64]\n"
    "ldp x13,x14,[x0,#80]\n ldp x15,x16,[x0,#96]\n ldr x17,[x0,#112]\n"
    "1:\n .rept 12\n add x3,x3,x4\n eor x5,x5,x6\n add x7,x7,x8\n"
    "sub x9,x9,x10\n eor x11,x11,x12\n add x13,x13,x14\n"
    "add x15,x15,x16\n eor x17,x17,x3\n .endr\n subs x2,x2,#1\n b.ne 1b\n"
    "stp x3,x4,[x1]\n stp x5,x6,[x1,#16]\n stp x7,x8,[x1,#32]\n"
    "stp x9,x10,[x1,#48]\n stp x11,x12,[x1,#64]\n stp x13,x14,[x1,#80]\n"
    "stp x15,x16,[x1,#96]\n str x17,[x1,#112]\n ret\n"
    ".size long_pressure,.-long_pressure\n");

static int check_long_pressure(void) {
    for (unsigned repeat = 1; repeat <= 32; ++repeat) {
        uint64_t original[15], want[15], got[15];
        for (unsigned i = 0; i < 15; ++i)
            original[i] = want[i] = UINT64_C(0xfedcba9876543210) * (i + repeat);
        for (unsigned step = 0; step < repeat * 12; ++step) {
            want[0] += want[1]; want[2] ^= want[3]; want[4] += want[5];
            want[6] -= want[7]; want[8] ^= want[9]; want[10] += want[11];
            want[12] += want[13]; want[14] ^= want[0];
        }
        long_pressure(original, got, repeat);
        for (unsigned i = 0; i < 15; ++i) if (got[i] != want[i]) {
            printf("FAIL long pressure repeat=%u reg=%u got=%llx want=%llx\n",
                repeat, i, (unsigned long long)got[i], (unsigned long long)want[i]);
            return 1;
        }
    }
    return 0;
}

static uint64_t reference(unsigned count) {
    uint64_t sum = 0;
    while (count) {
        const uint64_t value = input[count & 255];
        const uint64_t u = (value >> 9) & 0x1ffff;
        uint32_t s = (uint32_t)(value >> 5) & 0x7ff;
        if (s & 0x400) s |= 0xfffff800u;
        uint64_t inserted = value & 0x1ff;
        if (inserted & 0x100) inserted |= ~UINT64_C(0x1ff);
        uint32_t shifted = (uint32_t)value >> 12;
        if ((uint32_t)value & 0x80000000u) shifted |= 0xfff00000u;
        sum ^= u;
        sum += s;
        sum ^= inserted << 3;
        sum += value >> 7;
        sum += shifted;
        sum += (uint32_t)value << 3;
        sum ^= value; // source is live across several clobbering bitfields
        sum += u;
        --count;
    }
    return sum;
}

static uint64_t run(unsigned iterations) {
    register uint64_t count asm("x0") = iterations;
    register const uint64_t *src asm("x1") = input;
    register uint64_t sum asm("x2") = 0;
    register uint64_t *dst asm("x12") = output;
    __asm__ volatile(
        "1:\n"
        "cbz x0, 2f\n" // side exit plus unconditional back-edge: tier-2 region
        "and x14, x0, #255\n"
        "add x3, x1, x14, lsl #3\n"
        "ldr x4, [x3]\n"
        "ubfx x5, x4, #9, #17\n"
        "sbfx w6, w4, #5, #11\n"
        "sbfiz x7, x4, #3, #9\n"
        "lsr x8, x4, #7\n"
        "asr w9, w4, #12\n"
        "lsl w10, w4, #3\n"
        "eor x2, x2, x5\n"
        "add x2, x2, x6\n"
        "eor x2, x2, x7\n"
        "add x2, x2, x8\n"
        "add x2, x2, x9\n"
        "add x2, x2, x10\n"
        "eor x2, x2, x4\n"
        "add x13, x12, x14, lsl #3\n"
        "str x5, [x13]\n"
        "ldr x11, [x13]\n"
        "add x2, x2, x11\n"
        "sub x0, x0, #1\n"
        "b 1b\n"
        "2:\n"
        : "+r"(count), "+r"(sum)
        : "r"(src), "r"(dst)
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
          "x11", "x13", "x14", "cc", "memory");
    return sum;
}

int main(void) {
    if (check_multipliers() || check_long_pressure()) return 1;
    uint64_t seed = UINT64_C(0xfedcba9876543210);
    for (unsigned i = 0; i < 256; ++i) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        input[i] = seed;
    }
    const unsigned counts[] = {1, 63, 8192, 8193};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        uint64_t want = reference(counts[i]);
        uint64_t got = run(counts[i]);
        if (got != want) {
            printf("FAIL: bitfield count=%u got=%llx want=%llx\n", counts[i],
                   (unsigned long long)got, (unsigned long long)want);
            return 1;
        }
    }
    puts("ALL PASS: bitfield regalloc, live sources, spills and region exits");
    return 0;
}
