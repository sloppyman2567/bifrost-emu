// fcvtzu_test2.c — minimal FCVTZU fixed-point test.
// Compute MD5 K[0] = floor(|sin(1)| * 2^32) using fcvtzu fixed-point.
//
// Verifies the rc.2 fix for the FCVTZS/FCVTZU/SCVTF/UCVTF fixed-point
// variants. Without the fix, these instructions were silently NOP'd
// (the integer-variant mask required bit 21 = 1; the fixed-point variant
// has bit 21 = 0 with a 6-bit scale field). The destination register
// was left unchanged, returning stale stack/register garbage.
#include <stdio.h>
#include <stdint.h>
#include <math.h>

int main(void) {
    int pass = 0, fail = 0;

    // MD5 K[0] expected: floor(|sin(1)| * 2^32) = 0xd76aa478
    double d = sin(1.0);  // 0.841470984807897
    d = fabs(d);
    printf("input d = %.15f\n", d);

    // fcvtzu w1, d0, #32 → (uint32_t)(d * 2^32), saturating to UINT32_MAX
    uint32_t got32;
    __asm__ volatile ("fcvtzu %w0, %d1, #32" : "=r"(got32) : "w"(d));
    uint32_t exp32 = (uint32_t)(d * 4294967296.0);
    printf("fcvtzu w, #32  = 0x%08x  (exp 0x%08x)  %s\n",
           got32, exp32, got32 == exp32 ? (++pass, "OK") : (++fail, "FAIL"));

    // fcvtzu x1, d0, #32 → (uint64_t)(d * 2^32)
    uint64_t got64;
    __asm__ volatile ("fcvtzu %0, %d1, #32" : "=r"(got64) : "w"(d));
    uint64_t exp64 = (uint64_t)(d * 4294967296.0);
    printf("fcvtzu x, #32  = 0x%016lx  (exp 0x%016lx)  %s\n",
           got64, exp64, got64 == exp64 ? (++pass, "OK") : (++fail, "FAIL"));

    // fcvtzs w, #32: sin(1)*2^32 = 3.61e9 which OVERFLOWS int32 (max 2.15e9).
    // ARM saturates: result = INT32_MAX = 0x7fffffff.
    int32_t got32s;
    __asm__ volatile ("fcvtzs %w0, %d1, #32" : "=r"(got32s) : "w"(d));
    int32_t exp32s = INT32_MAX;  // saturates
    printf("fcvtzs w, #32  = 0x%08x  (exp 0x%08x = INT32_MAX, saturates)  %s\n",
           (uint32_t)got32s, (uint32_t)exp32s,
           got32s == exp32s ? (++pass, "OK") : (++fail, "FAIL"));

    // Standard integer conversion (no fixed-point) — was already working.
    uint32_t got_int;
    __asm__ volatile ("fcvtzu %w0, %d1" : "=r"(got_int) : "w"(d));
    printf("fcvtzu w (int) = %u  (exp 0)  %s\n",
           got_int, got_int == 0 ? (++pass, "OK") : (++fail, "FAIL"));

    // fbits = 1: sin(1) * 2 = 1.68 → floor = 1
    uint32_t got1;
    __asm__ volatile ("fcvtzu %w0, %d1, #1" : "=r"(got1) : "w"(d));
    printf("fcvtzu w, #1   = 0x%08x  (exp 0x00000001)  %s\n",
           got1, got1 == 1 ? (++pass, "OK") : (++fail, "FAIL"));

    // Saturation: 100.5 * 2^32 = 4.3e11 > UINT32_MAX → saturates
    uint32_t got_big;
    double big = 100.5;
    __asm__ volatile ("fcvtzu %w0, %d1, #32" : "=r"(got_big) : "w"(big));
    printf("fcvtzu(100.5, #32) = 0x%08x  (exp 0xffffffff = UINT32_MAX, saturates)  %s\n",
           got_big, got_big == 0xFFFFFFFFu ? (++pass, "OK") : (++fail, "FAIL"));

    // Negative input to unsigned: saturates to 0
    uint32_t got_neg;
    double neg = -3.14;
    __asm__ volatile ("fcvtzu %w0, %d1, #32" : "=r"(got_neg) : "w"(neg));
    printf("fcvtzu(-3.14, #32) = 0x%08x  (exp 0x00000000, saturates to 0)  %s\n",
           got_neg, got_neg == 0 ? (++pass, "OK") : (++fail, "FAIL"));

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
