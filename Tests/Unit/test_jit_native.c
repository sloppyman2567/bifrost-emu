// test_jit_native.c — Test JIT-native operations end-to-end.
//
// This test exercises code paths that the JIT compiles natively:
//   - Integer arithmetic (ADD, SUB, MUL, shifts)
//   - Bitfield operations (UBFM/SBFM, LSL/LSR/ASR)
//   - Conditional select (CSEL, CSINC)
//   - FP scalar arithmetic (FADD, FMUL, FCVTZS)
//   - SIMD arithmetic (vector ADD/SUB/MUL)
//   - Memory ops (LDP/STP, LDR/STR)
//
// The test verifies correctness by comparing against known values.
// If the JIT has a codegen bug, the test will produce wrong values.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

// Test integer arithmetic
static int test_int_arith(void) {
    volatile uint64_t a = 1000, b = 7;
    uint64_t sum = a + b;
    uint64_t diff = a - b;
    uint64_t prod = a * b;
    uint64_t quot = a / b;
    uint64_t mod = a % b;
    if (sum != 1007) { printf("FAIL add: %lu\n", sum); return 1; }
    if (diff != 993) { printf("FAIL sub: %lu\n", diff); return 1; }
    if (prod != 7000) { printf("FAIL mul: %lu\n", prod); return 1; }
    if (quot != 142) { printf("FAIL div: %lu\n", quot); return 1; }
    if (mod != 6) { printf("FAIL mod: %lu\n", mod); return 1; }
    printf("PASS int_arith\n");
    return 0;
}

// Test bitfield / shift operations
static int test_bitfield(void) {
    volatile uint64_t x = 0xDEADBEEF12345678ULL;
    uint64_t lsl = x << 8;
    uint64_t lsr = x >> 4;
    int64_t asr = (int64_t)x >> 4;
    uint64_t extracted = (x >> 16) & 0xFFFF;
    // x << 8 = 0xEADBEEF1234567800 truncated to 64 bits = 0xADBEEF1234567800
    if (lsl != 0xADBEEF1234567800ULL) { printf("FAIL lsl: 0x%lx\n", lsl); return 1; }
    if (lsr != 0x0DEADBEEF1234567ULL) { printf("FAIL lsr: 0x%lx\n", lsr); return 1; }
    if (extracted != 0x1234) { printf("FAIL bfx: 0x%lx\n", extracted); return 1; }
    printf("PASS bitfield\n");
    return 0;
}

// Test conditional select
static int test_csel(void) {
    volatile int a = 10, b = 20;
    volatile int cond = 1;
    int result = cond ? a : b;
    if (result != 10) { printf("FAIL csel true: %d\n", result); return 1; }
    cond = 0;
    result = cond ? a : b;
    if (result != 20) { printf("FAIL csel false: %d\n", result); return 1; }
    printf("PASS csel\n");
    return 0;
}

// Test FP arithmetic
static int test_fp(void) {
    volatile double a = 3.14, b = 2.71;
    double sum = a + b;
    double prod = a * b;
    double quot = a / b;
    int32_t cvt = (int32_t)sum;
    if (sum < 5.84 || sum > 5.86) { printf("FAIL fp add: %f\n", sum); return 1; }
    if (prod < 8.50 || prod > 8.52) { printf("FAIL fp mul: %f\n", prod); return 1; }
    if (quot < 1.15 || quot > 1.17) { printf("FAIL fp div: %f\n", quot); return 1; }
    if (cvt != 5) { printf("FAIL fp cvt: %d\n", cvt); return 1; }
    printf("PASS fp_arith\n");
    return 0;
}

// Test SIMD arithmetic (native JIT via SIMD_ARITH)
static int test_simd(void) {
    // 8-bit lane add
    uint8x8_t a = vdup_n_u8(10);
    uint8x8_t b = vcreate_u8(0x0504030201000706ULL);
    uint8x8_t r = vadd_u8(a, b);
    uint8_t got[8];
    vst1_u8(got, r);
    uint8_t exp[8] = {16, 17, 10, 11, 12, 13, 14, 15};
    if (memcmp(got, exp, 8) != 0) {
        printf("FAIL simd add:");
        for (int i = 0; i < 8; i++) printf(" %d", got[i]);
        printf("\n");
        return 1;
    }

    // 32-bit lane mul
    uint32x2_t a2 = vcreate_u32(0x0000000300000007ULL);
    uint32x2_t b2 = vcreate_u32(0x0000000600000005ULL);
    uint32x2_t r2 = vmul_u32(a2, b2);
    uint32_t got2[2];
    vst1_u32(got2, r2);
    if (got2[0] != 35 || got2[1] != 18) {
        printf("FAIL simd mul: %u %u\n", got2[0], got2[1]);
        return 1;
    }
    printf("PASS simd_arith\n");
    return 0;
}

// Test memory operations (LDP/STP)
static int test_memory(void) {
    uint64_t buf[4] = {0};
    buf[0] = 0x1111111111111111ULL;
    buf[1] = 0x2222222222222222ULL;
    buf[2] = 0x3333333333333333ULL;
    buf[3] = 0x4444444444444444ULL;
    // Read back
    if (buf[0] != 0x1111111111111111ULL) { printf("FAIL mem st/ld 0\n"); return 1; }
    if (buf[3] != 0x4444444444444444ULL) { printf("FAIL mem st/ld 3\n"); return 1; }
    // Swap via temp
    uint64_t tmp = buf[0]; buf[0] = buf[3]; buf[3] = tmp;
    if (buf[0] != 0x4444444444444444ULL) { printf("FAIL mem swap 0\n"); return 1; }
    if (buf[3] != 0x1111111111111111ULL) { printf("FAIL mem swap 3\n"); return 1; }
    printf("PASS memory\n");
    return 0;
}

// Test loop with accumulator (exercises self-loop chaining)
static int test_loop(void) {
    volatile uint64_t n = 1000000;
    uint64_t sum = 0;
    for (uint64_t i = 1; i <= n; i++) {
        sum += i;
    }
    // sum = n * (n+1) / 2 = 1000000 * 1000001 / 2 = 500000500000
    if (sum != 500000500000ULL) {
        printf("FAIL loop: %lu\n", sum);
        return 1;
    }
    printf("PASS loop\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_int_arith();
    failures += test_bitfield();
    failures += test_csel();
    failures += test_fp();
    failures += test_simd();
    failures += test_memory();
    failures += test_loop();
    if (failures == 0) {
        printf("\ntest_jit_native: ALL PASS\n");
        return 0;
    }
    printf("\ntest_jit_native: %d FAILURES\n", failures);
    return 1;
}
