/*
 * jit_fp_half_convert.c — half-precision conversions and NaN results.
 *
 * SpongeBob's outfit repack (Spine-Unity GetRepackedSkin) widens half-float
 * pixels with vector FCVTL. It fell through to an unrelated handler, and the
 * character drew without its body. These fixed values follow Arm's FPConvert
 * and FPRoundInt (FPCR defaults) and pass unchanged under QEMU:
 *   - FCVTL/FCVTL2 .4s from .4h/.8h: exact, subnormals, quieted NaN payloads
 *   - FCVTN/FCVTN2 .4h/.8h from .4s: ties-to-even, overflow, tiny values
 *   - scalar FCVT S/D <-> H (two forms were mis-encoded and ran as NOPs)
 *   - FRINTN quiets a signalling NaN; scalar FADDP uses ARM NaN rules
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void expect(const char* name, const void* got, const void* want, size_t n) {
    ++checks;
    if (memcmp(got, want, n)) {
        const uint8_t* g = got; const uint8_t* w = want;
        printf("FAIL %s got=", name);
        for (size_t i = n; i-- > 0;) printf("%02x", g[i]);
        printf(" want=");
        for (size_t i = n; i-- > 0;) printf("%02x", w[i]);
        printf("\n");
        ++fails;
    }
}
#define OP(fn, ins) static void fn(const void* in, void* out) { \
    __asm__ volatile("ldr q0, [%1]\n movi v2.16b, #0x5a\n " ins "\n str q2, [%0]" \
        :: "r"(out), "r"(in) : "v0", "v2", "memory"); }
OP(fcvtl, "fcvtl v2.4s, v0.4h")
OP(fcvtl2, "fcvtl2 v2.4s, v0.8h")
OP(fcvtn, "fcvtn v2.4h, v0.4s")
OP(fcvtn2, "fcvtn2 v2.8h, v0.4s")
OP(fcvt_sh, "fcvt s2, h0")
OP(fcvt_dh, "fcvt d2, h0")
OP(fcvt_hs, "fcvt h2, s0")
OP(fcvt_hd, "fcvt h2, d0")
OP(frintn, "frintn v2.4s, v0.4s")
OP(faddp, "faddp s2, v0.2s")

int main(void) {
    uint32_t out[4];
    { /* zeros, 1.0, 1.875 (0x3f80); high half ignored for FCVTL */
        const uint16_t in[8] = {0x0000, 0x8000, 0x3c00, 0x3f80, 0xffff, 0xffff, 0xffff, 0xffff};
        const uint32_t want[4] = {0x00000000, 0x80000000, 0x3f800000, 0x3ff00000};
        fcvtl(in, out); expect("fcvtl normal", out, want, 16);
    }
    { /* smallest/largest subnormal, infinity, signalling NaN (quieted) */
        const uint16_t in[8] = {0, 0, 0, 0, 0x0001, 0x03ff, 0x7c00, 0x7d01};
        const uint32_t want[4] = {0x33800000, 0x387fc000, 0x7f800000, 0x7fe02000};
        fcvtl2(in, out); expect("fcvtl2 special", out, want, 16);
    }
    { /* 1.0; 65520 ties to even -> inf; 1+2^-11 ties down; 1+3*2^-11 ties up */
        const uint32_t in[4] = {0x3f800000, 0x477ff000, 0x3f801000, 0x3f803000};
        const uint32_t want[4] = {0x3c00 | 0x7c00u << 16, 0x3c00 | 0x3c02u << 16, 0, 0};
        fcvtn(in, out); expect("fcvtn rounding", out, want, 16);
    }
    { /* sNaN payload, 2^-24, 2^-25 (tie -> 0), just above 2^-25; low half kept */
        const uint32_t in[4] = {0x7f800001, 0x33800000, 0x33000000, 0x33000001};
        const uint32_t want[4] = {0x5a5a5a5a, 0x5a5a5a5a, 0x7e00 | 0x0001u << 16, 0x0000 | 0x0001u << 16};
        fcvtn2(in, out); expect("fcvtn2 tiny/nan", out, want, 16);
    }
    { /* scalar widening: 0x3555 -> single; subnormal 0x0001 -> 2^-24 double */
        const uint32_t in[4] = {0x3555, 0, 0, 0};
        const uint32_t want[4] = {0x3eaaa000, 0, 0, 0};
        fcvt_sh(in, out); expect("fcvt s,h", out, want, 16);
        const uint32_t in2[4] = {0x0001, 0, 0, 0};
        const uint32_t want2[4] = {0x00000000, 0x3e700000, 0, 0};
        fcvt_dh(in2, out); expect("fcvt d,h", out, want2, 16);
    }
    { /* scalar narrowing: 1/3 rounds to 0x3555; NaN keeps its quiet payload */
        const uint32_t in[4] = {0x3eaaaaab, 0, 0, 0};
        const uint32_t want[4] = {0x3555, 0, 0, 0};
        fcvt_hs(in, out); expect("fcvt h,s", out, want, 16);
        const uint32_t in2[4] = {0x55555555, 0x3fd55555, 0, 0};
        fcvt_hd(in2, out); expect("fcvt h,d", out, want, 16);
        const uint32_t in3[4] = {0x7fc12345, 0, 0, 0};
        const uint32_t want3[4] = {0x7e09, 0, 0, 0};
        fcvt_hs(in3, out); expect("fcvt h,s nan", out, want3, 16);
    }
    { /* FRINTN: signalling NaN is quieted, numbers round to even */
        const uint32_t in[4] = {0x7f800001, 0x40200000, 0x3fc00000, 0xbf000000};
        const uint32_t want[4] = {0x7fc00001, 0x40000000, 0x40000000, 0x80000000};
        frintn(in, out); expect("frintn", out, want, 16);
    }
    { /* FADDP: inf + -inf is +qNaN; a signalling NaN wins over a quiet one */
        const uint32_t in[4] = {0x7f800000, 0xff800000, 0, 0};
        const uint32_t want[4] = {0x7fc00000, 0, 0, 0};
        faddp(in, out); expect("faddp invalid", out, want, 16);
        const uint32_t in2[4] = {0x7fc00001, 0x7f800002, 0, 0};
        const uint32_t want2[4] = {0x7fc00002, 0, 0, 0};
        faddp(in2, out); expect("faddp nan order", out, want2, 16);
    }
    if (fails) { printf("jit_fp_half_convert: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_fp_half_convert: %d checks passed\n", checks);
    return 0;
}
