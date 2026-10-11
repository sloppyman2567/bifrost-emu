// test_simd_sat.c — SMOV + saturating arithmetic family (2026-08).
//
// Covers previously-missing instructions (each was a hard DecodeError →
// SIGILL before): SMOV, SQADD/UQADD, SQSUB/UQSUB, SQSHL/UQSHL (register),
// SQRSHL/UQRSHL, SRSHL/URSHL, SQABS/SQNEG, SUQADD/USQADD. Expected values
// hand-computed per the ARM ARM.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void chk(int ok, const char* what) {
    checks++;
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

#define VOP3(insn, form, out)                              \
    __asm__ volatile(insn " v0." form ", v1." form ", v2." form "\n\t" \
                     "str q0, [%0]" :: "r"(out)            \
                     : "v0", "memory")
#define VOP2(insn, form, out)                              \
    __asm__ volatile(insn " v0." form ", v1." form "\n\t"  \
                     "str q0, [%0]" :: "r"(out)            \
                     : "v0", "memory")

int main(void) {
    printf("test_simd_sat: start\n");

    // ── SQADD / UQADD ────────────────────────────────────────────────
    {
        // .16b: INT8_MIN + INT8_MIN = -256 -> -128; 100+100 = 200 -> 127
        static const uint8_t a[16] = {128, 100}, b[16] = {128, 100};
        static const uint8_t want[16] = {128 /*-128*/, 127};
        uint8_t out[16]; memset(out, 0xAA, 16);
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqadd v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(memcmp(out + 0, want + 0, 1) == 0 &&
            memcmp(out + 1, want + 1, 1) == 0,
            "sqadd .16b saturates (-128, 127)");
    }
    {
        // uqadd .8h: 60000+60000=120000 -> 65535
        static const uint16_t a[8] = {60000}, b[8] = {60000};
        uint16_t out[8];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "uqadd v0.8h, v1.8h, v2.8h\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 65535, "uqadd .8h saturates to 65535");
    }
    {
        // sqadd .4s: INT32_MAX + 1 -> INT32_MAX; -5 + 3 = -2
        static const int32_t a[4] = {2147483647, -5},
                             b[4] = {1, 3};
        int32_t out[4];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqadd v0.4s, v1.4s, v2.4s\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 2147483647 && out[1] == -2,
            "sqadd .4s (sat max, plain add)");
    }
    {
        // sqadd .2d: INT64_MAX + 1 -> INT64_MAX
        static const int64_t a[2] = {(int64_t)9223372036854775807LL},
                             b[2] = {1};
        int64_t out[2];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqadd v0.2d, v1.2d, v2.2d\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 9223372036854775807LL, "sqadd .2d saturates at INT64_MAX");
    }

    // ── SQSUB / UQSUB ────────────────────────────────────────────────
    {
        // sqsub .16b: INT8_MIN - 1 -> -128; 5 - 10 = -5; 10 - 3 = 7
        static const int8_t a[16] = {-128, 5, 10}, b[16] = {1, 10, 3};
        static const int8_t want[16] = {-128, -5, 7};
        int8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqsub v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == want[0] && out[1] == want[1] && out[2] == want[2],
            "sqsub .16b (sat min, negatives)");
    }
    {
        // uqsub .16b: 1 - 2 -> 0
        static const uint8_t a[16] = {1}, b[16] = {2};
        uint8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "uqsub v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 0, "uqsub .16b clamps to 0");
    }

    // ── SQSHL / UQSHL (register) ─────────────────────────────────────
    {
        // sqshl .8h: 5 << 3 = 40; 30000 << 2 = 120000 -> sat 32767;
        //             5 by shift amount 0xFFFF (-1) -> 5>>1 = 2 (ASR)
        static const int16_t a[8] = {5, 30000, 5},
                             b[8] = {3, 2, (int16_t)0xFFFF};
        static const int16_t want[8] = {40, 32767, 2};
        int16_t out[8];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqshl v0.8h, v1.8h, v2.8h\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == want[0] && out[1] == want[1] && out[2] == want[2],
            "sqshl .8h (left, saturate, negative->asr)");
    }
    {
        // uqshl .8h: 0x80 << 1 with huge unsigned amount 200 ->
        //             lane != 0 -> saturate to 255
        static const uint8_t a[16] = {0x80}, b[16] = {200};
        uint8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "uqshl v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 255, "uqshl .16b huge shift saturates to max");
    }
    {
        // sqshl .2d: 1 << 63 -> saturates to INT64_MAX; INT64_MIN << 1
        //             -> saturates to INT64_MIN (negative overflow)
        static const int64_t a[2] = {1, (int64_t)0x8000000000000000ULL},
                             b[2] = {63, 1};
        static const int64_t want[2] = {(int64_t)0x7FFFFFFFFFFFFFFFLL,
                                        (int64_t)0x8000000000000000ULL};
        int64_t out[2];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqshl v0.2d, v1.2d, v2.2d\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == want[0] && out[1] == want[1],
            "sqshl .2d (1<<63 sat MAX, INT64_MIN sat)");
    }

    // ── SQRSHL / UQRSHL (rounding variants) ──────────────────────────
    {
        // sqrshl .16b: -7 >> 1 rounded half-up = -3 (SSH L would give -4);
        //              5 >> 1 round = 3 (2.5 rounds away from zero)
        static const int8_t a[16] = {-7, 5}, b[16] = {-1, -1};
        static const int8_t want[16] = {-3, 3};
        int8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "sqrshl v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == want[0] && out[1] == want[1],
            "sqrshl .16b rounding on right shifts");
    }
    {
        // uqrshl .16b: signed amount -1 on unsigned lane 15 -> round(15/2)=8
        //              (plain logical right gives 7)
        static const uint8_t a[16] = {15}, b[16] = {(uint8_t)-1};
        uint8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "uqrshl v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 8, "uqrshl .16b signed amount rounds right");
    }
    {
        // srshl .16b (no saturation): 3 << 70(amount clamps to fill? no:
        // shift amount is byte lane 70 -> left overflow NOT saturated ->
        // low bits only) — use safe values instead: 12 >> 2 = 3.
        static const int8_t a[16] = {12}, b[16] = {-2};
        int8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tldr q2,[%2]\n\t"
                         "srshl v0.16b, v1.16b, v2.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a), "r"(b) : "v0","v1","v2","memory");
        chk(out[0] == 3, "srshl .16b right shift");
    }

    // ── SQABS / SQNEG ────────────────────────────────────────────────
    {
        // sqabs .16b: -128 -> +127 (SATURATED, unlike plain ABS);
        //             -5 -> 5; 7 -> 7
        static const int8_t a[16] = {-128, -5, 7};
        static const int8_t want[16] = {127, 5, 7};
        int8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tsqabs v0.16b, v1.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a) : "v0","v1","memory");
        chk(out[0] == want[0] && out[1] == want[1] && out[2] == want[2],
            "sqabs .16b saturates INT8_MIN to +127");
    }
    {
        // sqneg .16b: -128 -> +127 (saturated); 42 -> -42
        static const int8_t a[16] = {-128, 42};
        int8_t out[16];
        __asm__ volatile("ldr q1,[%1]\n\tsqneg v0.16b, v1.16b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a) : "v0","v1","memory");
        chk(out[0] == 127 && out[1] == -42,
            "sqneg .16b saturates INT8_MIN to +127");
    }
    {
        // Q=0 (.8b) zeroes the upper half of Vd.
        static const int8_t a[8] = {-128};
        uint8_t out[16]; memset(out, 0xFF, 16);
        __asm__ volatile("ldr d1,[%1]\n\tsqabs v0.8b, v1.8b\n\tstr q0,[%0]"
                         :: "r"(out), "r"(a) : "v0","v1","memory");
        static const uint8_t zeros[8] = {0};
        chk(out[0] == 127 && memcmp(out + 8, zeros, 8) == 0,
            "sqabs .8b zeroes upper half (Q=0)");
    }

    // ── SUQADD / USQADD ──────────────────────────────────────────────
    {
        // suqadd .4s (dest SIGNED, Vn UNSIGNED, sat to SIGNED range):
        //   Vd=-1 + Vn=0xFFFFFFFF -> 4294967294 -> sat INT32_MAX;
        //   Vd=INT32_MIN + Vn=0x80000000 -> -2^31 + 2^31 = 0
        static const uint32_t vd_in[4] = {0xFFFFFFFFu, 0x80000000u};
        static const int32_t vn[4] = {-1, -2147483648LL};
        static const uint32_t want[4] = {0x7FFFFFFFu, 0};
        uint32_t out[4];
        __asm__ volatile("ldr q0,[%1]\n\tldr q1,[%2]\n\t"
                         "suqadd v0.4s, v1.4s\n\tstr q0,[%0]"
                         :: "r"(out), "r"(vd_in), "r"(vn)
                         : "v0","v1","memory");
        chk(out[0] == want[0] && out[1] == want[1],
            "suqadd .4s accumulate + signed saturation");
    }
    {
        // usqadd .2d (dest UNSIGNED, Vn SIGNED, sat to UNSIGNED range):
        //   Vd=UINT64_MAX + Vn=5 -> stays UINT64_MAX;
        //   Vd=0 + Vn=-1 -> -1 clamps to 0
        static const uint64_t vd_in[2] = {~0ULL, 0};
        static const int64_t vn[2] = {5, -1};
        static const uint64_t want[2] = {~0ULL, 0};
        uint64_t out[2];
        __asm__ volatile("ldr q0,[%1]\n\tldr q1,[%2]\n\t"
                         "usqadd v0.2d, v1.2d\n\tstr q0,[%0]"
                         :: "r"(out), "r"(vd_in), "r"(vn)
                         : "v0","v1","memory");
        chk(out[0] == want[0] && out[1] == want[1],
            "usqadd .2d unsigned saturation");
    }

    // ── SMOV ─────────────────────────────────────────────────────────
    {
        // smov w0, v1.b[3]: element 0xF8 -> sign-extended 0xFFFFFFF8
        static const uint8_t bytes[16] = {0x11, 0x22, 0x33, 0xF8};
        uint64_t w = 0xAAAA;
        __asm__ volatile("ldr q1,[%1]\n\t"
                         "smov %w0, v1.b[3]"
                         : "=r"(w) : "r"(bytes) : "v1");
        chk(w == 0xFFFFFFF8ULL, "smov w, b[3] sign-extends to 32-bit");
    }
    {
        // smov x5, v1.h[5]: element 0x8001 -> 0xFFFFFFFF80001... i.e.
        // 0xFFFFFFFFFFFF8001
        static const uint16_t halves[8] = {0x1111, 0x2222, 0x3333, 0x4444,
                                           0x5555, 0x8001};
        uint64_t x = 0xAAAA;
        __asm__ volatile("ldr q1,[%1]\n\t"
                         "smov %0, v1.h[5]"
                         : "=r"(x) : "r"(halves) : "v1");
        chk(x == 0xFFFFFFFFFFFF8001ULL, "smov x, h[5] sign-extends to 64-bit");
    }
    {
        // smov x4, v1.s[1]: element 0x80000000 -> 0xFFFFFFFF80000000
        static const uint32_t words[4] = {1, 0x80000000u};
        uint64_t x = 0xAAAA;
        __asm__ volatile("ldr q1,[%1]\n\t"
                         "smov %0, v1.s[1]"
                         : "=r"(x) : "r"(words) : "v1");
        chk(x == 0xFFFFFFFF80000000ULL, "smov x, s[1] sign-extends .S element");
    }
    {
        // Positive values stay positive across widths.
        static const uint8_t bytes[16] = {0x7F};
        uint64_t w = 0;
        __asm__ volatile("ldr q1,[%1]\n\t"
                         "smov %w0, v1.b[0]"
                         : "=r"(w) : "r"(bytes) : "v1");
        chk(w == 0x7F, "smov w, b[0] positive unchanged");
    }

    printf("test_simd_sat: %d checks, %d failures\n", checks, fails);
    if (fails == 0) printf("ALL PASS\n");
    return fails ? 1 : 0;
}
