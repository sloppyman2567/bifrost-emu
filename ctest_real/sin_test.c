// sin_test.c — verify sin() works correctly under the emulator.
// MD5's K[i] = floor(abs(sin(i+1)) * 2^32). If sin is wrong, MD5 K is wrong.
#include <stdio.h>
#include <stdint.h>
#include <math.h>

int main(void) {
    // MD5 K[0] should be 0xd76aa478 = floor(|sin(1)| * 2^32)
    // sin(1) ≈ 0.841470984807897
    // 0.841470984807897 * 2^32 = 3614090340.0...
    // 0xd76aa478 = 3614090360 (decimal)

    printf("sin(1.0) = %.15f (exp 0.841470984807897)\n", sin(1.0));
    printf("sin(2.0) = %.15f (exp 0.909297426825682)\n", sin(2.0));
    printf("sin(3.0) = %.15f (exp 0.141120008059867)\n", sin(3.0));

    // Compute first few MD5 K values
    printf("\nMD5 K table:\n");
    uint32_t expected_K[8] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501
    };
    for (int i = 0; i < 8; i++) {
        double s = sin((double)(i + 1));
        double abs_s = s < 0 ? -s : s;
        // fcvtzu w1, d0, #32 means: (uint32_t)(d0 * 2^32)
        // The "32" is the fixed-point precision in fractional bits.
        uint64_t k_full = (uint64_t)(abs_s * (1ULL << 32));
        uint32_t k = (uint32_t)k_full;
        printf("K[%d] = 0x%08x  exp=0x%08x  %s\n", i, k, expected_K[i],
               k == expected_K[i] ? "OK" : "FAIL");
    }
    return 0;
}
