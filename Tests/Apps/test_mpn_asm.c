// test_mpn_asm.c — replicate glibc's __mpn_mul_1 inner loop in inline asm
// to find which instruction produces the wrong carry.
//
// glibc's __mpn_mul_1 (from libc.so.6 disassembly):
//   4e310: lsl x5, x2, #3          ; x5 = n * 8
//   4e314: neg x4, x2              ; x4 = -n (index)
//   4e318: add x1, x1, x5          ; x1 = ap + n*8
//   4e31c: lsr x9, x3, #32         ; x9 = mult >> 32 (hi32 of mult)
//   4e320: and x8, x3, #0xffffffff ; x8 = mult & 0xFFFFFFFF (lo32)
//   4e324: add x10, x0, x5         ; x10 = rp + n*8
//   4e328: mov x11, #0x100000000   ; x11 = 2^32
//   4e32c: mov x0, #0              ; carry = 0
// loop:
//   4e330: ldr x2, [x1, x4, lsl #3] ; x2 = ap[i]
//   4e334: and x5, x2, #0xffffffff  ; x5 = lo32(ap[i])
//   4e338: lsr x2, x2, #32          ; x2 = hi32(ap[i])
//   4e33c: mul x3, x5, x8           ; x3 = lo32*lo32(mult)
//   4e340: mul x7, x2, x8           ; x7 = hi32*lo32(mult)
//   4e344: madd x6, x9, x5, x7      ; x6 = hi32(mult)*lo32 + x7
//   4e348: mul x2, x9, x2           ; x2 = hi32(mult)*hi32
//   4e34c: add x5, x0, w3, uxtw     ; x5 = carry + uxtw(x3)
//   4e350: add x3, x6, x3, lsr #32  ; x3 = x6 + (x3 >> 32)
//   4e354: add x6, x2, x11          ; x6 = x2 + 2^32
//   4e358: cmp x3, x7               ; compare for carry detection
//   4e35c: add x5, x5, x3, lsl #32  ; x5 = x5 + (x3 << 32)
//   4e360: csel x2, x6, x2, cc      ; if cc (x3 < x7), x2 = x6, else x2
//   4e364: lsr x3, x3, #32          ; x3 = x3 >> 32
//   4e368: cmp x5, x0               ; compare for carry detection
//   4e36c: str x5, [x10, x4, lsl #3] ; rp[i] = x5
//   4e370: cinc x3, x3, cc          ; if cc (x5 < carry), x3++
//   4e374: add x0, x3, x2           ; new carry = x3 + x2
//   4e378: adds x4, x4, #1
//   4e37c: b.ne loop
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

// Reference implementation using __int128
static uint64_t mpn_mul_1_ref(uint64_t *rp, const uint64_t *ap,
                               int n, uint64_t mult) {
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        unsigned __int128 prod = (unsigned __int128)ap[i] * (unsigned __int128)mult;
        uint64_t lo = (uint64_t)prod;
        uint64_t hi = (uint64_t)(prod >> 64);
        uint64_t sum = lo + carry;
        if (sum < lo) hi++;
        rp[i] = sum;
        carry = hi;
    }
    return carry;
}

// Test a single iteration of the __mpn_mul_1 inner loop
// using inline assembly that replicates glibc's code exactly.
static uint64_t test_single_iter(uint64_t limb, uint64_t mult, uint64_t carry_in,
                                  uint64_t *result_out) {
    uint64_t result = 0, carry_out = 0;

    __asm__ volatile(
        "mov x0, %[carry]\n"
        "mov x3, %[mult]\n"
        "mov x2, %[limb]\n"
        // Decompose mult into hi32/lo32
        "lsr x9, x3, #32\n"
        "and x8, x3, #0xffffffff\n"
        // Decompose limb into hi32/lo32
        "and x5, x2, #0xffffffff\n"
        "lsr x2, x2, #32\n"
        // Partial products
        "mul x3, x5, x8\n"        // x3 = lo*lo
        "mul x7, x2, x8\n"        // x7 = hi*lo
        "madd x6, x9, x5, x7\n"  // x6 = mhi*lo + hi*lo
        "mul x2, x9, x2\n"        // x2 = mhi*hi
        // Combine
        "add x5, x0, w3, uxtw\n" // x5 = carry + uxtw(x3)
        "add x3, x6, x3, lsr #32\n" // x3 = x6 + (x3>>32)
        "mov x11, #0x100000000\n"
        "add x6, x2, x11\n"       // x6 = x2 + 2^32
        "cmp x3, x7\n"
        "add x5, x5, x3, lsl #32\n"
        "csel x2, x6, x2, cc\n"  // if x3 < x7 (cc), x2=x6, else x2
        "lsr x3, x3, #32\n"
        "cmp x5, x0\n"
        "mov %[result], x5\n"
        "cinc x3, x3, cc\n"      // if x5 < carry (cc), x3++
        "add x0, x3, x2\n"
        "mov %[carry_out], x0\n"
        : [result] "=r"(result),
          [carry_out] "=r"(carry_out)
        : [limb] "r"(limb),
          [mult] "r"(mult),
          [carry] "r"(carry_in)
        : "x0", "x2", "x3", "x5", "x6", "x7", "x8", "x9", "x11", "memory", "cc"
    );

    *result_out = result;
    return carry_out;
}

int main() {
    // Test cases: (limb, mult, carry) → expected (result, carry_out)
    struct {
        uint64_t limb, mult, carry;
        uint64_t exp_res, exp_carry;
    } tests[] = {
        // Simple cases
        {0xFFFFFFFFULL, 10, 0, 0xFFFFFFF6ULL, 9},
        {0x00000001ULL, 10, 0, 0xA, 0},
        {0x100000000ULL, 10, 0, 0xA00000000ULL, 0},
        // Cases that exercise carry propagation
        {0xFFFFFFFFULL, 0xFFFFFFFFULL, 0, 0xFFFFFFFE00000001ULL, 0},
        {0xFFFFFFFFFFFFFFFFULL, 2, 0, 0xFFFFFFFFFFFFFFFEULL, 1},
        // Cases with incoming carry
        {0, 10, 1, 1, 0},
        {0xFFFFFFFFULL, 10, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFF5ULL, 0xA},
    };

    int pass = 0, fail = 0;
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        uint64_t result, carry;
        carry = test_single_iter(tests[i].limb, tests[i].mult,
                                  tests[i].carry, &result);

        // Compute reference
        uint64_t ref_res, ref_carry;
        uint64_t ap[1] = {tests[i].limb};
        uint64_t rp[1] = {0};
        ref_carry = mpn_mul_1_ref(rp, ap, 1, tests[i].mult);
        // Add carry_in manually for reference
        {
            unsigned __int128 prod = (unsigned __int128)tests[i].limb * (unsigned __int128)tests[i].mult;
            uint64_t lo = (uint64_t)prod + tests[i].carry;
            uint64_t hi = (uint64_t)(prod >> 64);
            if (lo < (uint64_t)prod) hi++;
            ref_res = lo;
            ref_carry = hi;
        }

        if (result == ref_res && carry == ref_carry) {
            printf("PASS [%d]: limb=0x%llx mult=%llu carry=0x%llx -> res=0x%llx carry=0x%llx\n",
                   i, (unsigned long long)tests[i].limb,
                   (unsigned long long)tests[i].mult,
                   (unsigned long long)tests[i].carry,
                   (unsigned long long)result,
                   (unsigned long long)carry);
            pass++;
        } else {
            printf("FAIL [%d]: limb=0x%llx mult=%llu carry=0x%llx\n",
                   i, (unsigned long long)tests[i].limb,
                   (unsigned long long)tests[i].mult,
                   (unsigned long long)tests[i].carry);
            printf("  got:      res=0x%llx carry=0x%llx\n",
                   (unsigned long long)result,
                   (unsigned long long)carry);
            printf("  expected: res=0x%llx carry=0x%llx\n",
                   (unsigned long long)ref_res,
                   (unsigned long long)ref_carry);
            fail++;
        }
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    write(1, "[done]\n", 7);
    return fail > 0 ? 1 : 0;
}
