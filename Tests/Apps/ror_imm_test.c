// ror_imm_test.c — test ROR (immediate) which is encoded as UBFM.
// MD5 uses `ror w4, w4, #25` etc. — verify this works.
#include <stdio.h>
#include <stdint.h>

static uint32_t ror_imm(uint32_t v, unsigned r) {
    if (r == 0) return v;
    return (v >> r) | (v << (32 - r));
}

int main(void) {
    uint32_t v = 0x12345678;
    uint32_t got, exp;

    // Test 1: ROR by 25 (MD5 uses this in round 1)
    __asm__ volatile ("ror %w0, %w1, #25" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 25);
    printf("ROR(0x%08x, 25) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 2: ROR by 20
    __asm__ volatile ("ror %w0, %w1, #20" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 20);
    printf("ROR(0x%08x, 20) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 3: ROR by 15
    __asm__ volatile ("ror %w0, %w1, #15" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 15);
    printf("ROR(0x%08x, 15) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 4: ROR by 10
    __asm__ volatile ("ror %w0, %w1, #10" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 10);
    printf("ROR(0x%08x, 10) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 5: ROR by 27 (MD5 uses this in round 4)
    __asm__ volatile ("ror %w0, %w1, #27" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 27);
    printf("ROR(0x%08x, 27) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 6: ROR by 23 (MD5 round 3)
    __asm__ volatile ("ror %w0, %w1, #23" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 23);
    printf("ROR(0x%08x, 23) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 7: ROR by 7 (used in SHA256 sigma0)
    __asm__ volatile ("ror %w0, %w1, #7" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 7);
    printf("ROR(0x%08x, 7)  = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Test 8: ROR by 18 (SHA256 sigma1)
    __asm__ volatile ("ror %w0, %w1, #18" : "=r"(got) : "r"(v));
    exp = ror_imm(v, 18);
    printf("ROR(0x%08x, 18) = got=0x%08x exp=0x%08x  %s\n", v, got, exp, got==exp ? "OK" : "FAIL");

    // Now a 64-bit ROR
    uint64_t v64 = 0x123456789abcdef0ULL;
    uint64_t got64, exp64;
    __asm__ volatile ("ror %0, %1, #40" : "=r"(got64) : "r"(v64));
    exp64 = (v64 >> 40) | (v64 << 24);
    printf("ROR64(0x%016lx, 40) = got=0x%016lx exp=0x%016lx  %s\n", v64, got64, exp64, got64==exp64 ? "OK" : "FAIL");

    return 0;
}
