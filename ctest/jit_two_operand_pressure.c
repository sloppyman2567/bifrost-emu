// jit_two_operand_pressure.c — two-operand ALU loads under full register
// pressure.
//
// The block below is one OpenSSL sha256_block_data_order round sequence,
// entered as its own JIT block so every live register starts in CPU state.
// With FWD's shorter IR all allocatable host registers are occupied when
// `eor w8, w8, w1, lsr #10` loads its spilled operand. ensure_two_vregs()
// let Belady evict the other operand (dead after this op, so scored as
// dead), and its fallback then passed its arguments to
// force_two_vregs_to() in the wrong order. That clobbered a live host
// register, so x16/x20/x24 came back wrong and every OpenSSL SHA-256
// digest was incorrect under the JIT.
//
// Expected values come from a C model of the same 32 instructions.
// Runs under both JIT and --no-jit; "checks passed" is the pass marker.
#include <stdio.h>
#include <stdint.h>

void two_operand_block(uint64_t regs[32]);

// regs[0..30] = x0..x30 (x18/x29 untouched), regs[31] = the 8 bytes at
// [sp] before the block (in) and the word stored to [sp] (out).
__asm__(
    "    .text\n"
    "    .global two_operand_block\n"
    "two_operand_block:\n"
    "    stp x29, x30, [sp, #-112]!\n"
    "    stp x19, x20, [sp, #16]\n"
    "    stp x21, x22, [sp, #32]\n"
    "    stp x23, x24, [sp, #48]\n"
    "    stp x25, x26, [sp, #64]\n"
    "    stp x27, x28, [sp, #80]\n"
    "    str x0, [sp, #96]\n"
    "    sub sp, sp, #64\n"
    "    ldr x1, [x0, #248]\n"
    "    str x1, [sp]\n"
    "    ldp x1, x2, [x0, #8]\n"
    "    ldp x3, x4, [x0, #24]\n"
    "    ldp x5, x6, [x0, #40]\n"
    "    ldp x7, x8, [x0, #56]\n"
    "    ldp x9, x10, [x0, #72]\n"
    "    ldp x11, x12, [x0, #88]\n"
    "    ldp x13, x14, [x0, #104]\n"
    "    ldp x15, x16, [x0, #120]\n"
    "    ldr x17, [x0, #136]\n"
    "    ldp x19, x20, [x0, #152]\n"
    "    ldp x21, x22, [x0, #168]\n"
    "    ldp x23, x24, [x0, #184]\n"
    "    ldp x25, x26, [x0, #200]\n"
    "    ldp x27, x28, [x0, #216]\n"
    "    ldr x30, [x0, #240]\n"
    "    ldr x0, [x0]\n"
    "    b 1f\n"                      // start a fresh block
    "1:  ror w9, w4, #7\n"
    "    and w17, w26, w25\n"
    "    ror w8, w1, #17\n"
    "    bic w28, w27, w25\n"
    "    ror w10, w21, #2\n"
    "    add w20, w20, w2\n"
    "    eor w16, w16, w25, ror #11\n"
    "    eor w9, w9, w4, ror #18\n"
    "    orr w17, w17, w28\n"
    "    eor w28, w21, w22\n"
    "    eor w16, w16, w25, ror #25\n"
    "    eor w10, w10, w21, ror #13\n"
    "    add w20, w20, w17\n"
    "    and w19, w19, w28\n"
    "    eor w8, w8, w1, ror #19\n"
    "    eor w9, w9, w4, lsr #3\n"
    "    add w20, w20, w16\n"
    "    eor w19, w19, w22\n"
    "    eor w17, w10, w21, ror #22\n"
    "    eor w8, w8, w1, lsr #10\n"
    "    add w3, w3, w12\n"
    "    add w24, w24, w20\n"
    "    add w20, w20, w19\n"
    "    ldr w19, [x30], #4\n"
    "    add w3, w3, w9\n"
    "    add w20, w20, w17\n"
    "    add w3, w3, w8\n"
    "    ldr w8, [sp, #4]\n"
    "    str w11, [sp]\n"
    "    ror w16, w24, #6\n"
    "    add w27, w27, w19\n"
    "    ror w10, w5, #7\n"
    "    b 2f\n"                      // end the block
    "2:  ldr x29, [sp, #160]\n"
    "    str x0, [x29]\n"
    "    stp x1, x2, [x29, #8]\n"
    "    stp x3, x4, [x29, #24]\n"
    "    stp x5, x6, [x29, #40]\n"
    "    stp x7, x8, [x29, #56]\n"
    "    stp x9, x10, [x29, #72]\n"
    "    stp x11, x12, [x29, #88]\n"
    "    stp x13, x14, [x29, #104]\n"
    "    stp x15, x16, [x29, #120]\n"
    "    str x17, [x29, #136]\n"
    "    stp x19, x20, [x29, #152]\n"
    "    stp x21, x22, [x29, #168]\n"
    "    stp x23, x24, [x29, #184]\n"
    "    stp x25, x26, [x29, #200]\n"
    "    stp x27, x28, [x29, #216]\n"
    "    str x30, [x29, #240]\n"
    "    ldr w0, [sp]\n"
    "    str x0, [x29, #248]\n"
    "    add sp, sp, #64\n"
    "    ldp x19, x20, [sp, #16]\n"
    "    ldp x21, x22, [sp, #32]\n"
    "    ldp x23, x24, [sp, #48]\n"
    "    ldp x25, x26, [sp, #64]\n"
    "    ldp x27, x28, [sp, #80]\n"
    "    ldp x29, x30, [sp], #112\n"
    "    ret\n");

static uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void model(uint64_t r[32]) {
    uint32_t w[31];
    for (int i = 0; i < 31; i++) w[i] = (uint32_t)r[i];
    uint32_t sp_lo = (uint32_t)r[31], sp_hi = (uint32_t)(r[31] >> 32);
    const uint32_t* k = (const uint32_t*)(uintptr_t)r[30];
    w[9] = ror32(w[4], 7);
    w[17] = w[26] & w[25];
    w[8] = ror32(w[1], 17);
    w[28] = w[27] & ~w[25];
    w[10] = ror32(w[21], 2);
    w[20] += w[2];
    w[16] ^= ror32(w[25], 11);
    w[9] ^= ror32(w[4], 18);
    w[17] |= w[28];
    w[28] = w[21] ^ w[22];
    w[16] ^= ror32(w[25], 25);
    w[10] ^= ror32(w[21], 13);
    w[20] += w[17];
    w[19] &= w[28];
    w[8] ^= ror32(w[1], 19);
    w[9] ^= w[4] >> 3;
    w[20] += w[16];
    w[19] ^= w[22];
    w[17] = w[10] ^ ror32(w[21], 22);
    w[8] ^= w[1] >> 10;
    w[3] += w[12];
    w[24] += w[20];
    w[20] += w[19];
    w[19] = k[0];
    w[3] += w[9];
    w[20] += w[17];
    w[3] += w[8];
    w[8] = sp_hi;
    sp_lo = w[11];
    w[16] = ror32(w[24], 6);
    w[27] += w[19];
    w[10] = ror32(w[5], 7);
    // W-register writes zero-extend; registers the block never writes
    // keep their full 64-bit value.
    static const int written[] = {3, 8, 9, 10, 16, 17, 19, 20, 24, 27, 28};
    for (unsigned i = 0; i < sizeof(written) / sizeof(written[0]); i++)
        r[written[i]] = w[written[i]];
    r[30] += 4;
    r[31] = sp_lo;
}

int main(void) {
    static uint32_t table[128];
    uint64_t x = 0x243f6a8885a308d3ULL;
    int checks = 0, failures = 0;
    for (int i = 0; i < 128; i++) table[i] = (uint32_t)i * 0x9e3779b9u;
    for (int trial = 0; trial < 64; trial++) {
        uint64_t in[32], got[32], want[32];
        for (int i = 0; i < 32; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            in[i] = x;
        }
        in[18] = in[29] = 0;
        in[30] = (uint64_t)(uintptr_t)(table + trial);
        for (int i = 0; i < 32; i++) got[i] = want[i] = in[i];
        two_operand_block(got);
        model(want);
        for (int i = 0; i < 32; i++) {
            if (i == 18 || i == 29) continue;
            checks++;
            if (got[i] != want[i]) {
                if (failures < 8)
                    printf("FAIL trial %d reg %d: got 0x%llx want 0x%llx\n", trial, i,
                           (unsigned long long)got[i], (unsigned long long)want[i]);
                failures++;
            }
        }
    }
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
