// test_fcvt_fpsrc.c — SIMD-scalar int<->FP conversions with FP register
// source/dest (the 0x5E200800 two-register-misc group):
//   scvtf/ucvtf sN,sN   (32-bit int -> float)
//   scvtf/ucvtf dN,dN   (64-bit int -> double)
//   fcvtzs/fcvtzu sN,sN (single    -> 32-bit int, FP dest)
//   fcvtzs/fcvtzu dN,dN (double    -> 64-bit int, FP dest)
// GCC/clang emit these for (float)int_var when the int already lives in an
// FP register (e.g. the voxel game's chunk math). No libm, static-friendly
// so `make setup-tests` can build it.
//
// Reference: interpreter semantics in src/interp/interp_fp.cpp (0x5E200800).

#include <stdio.h>
#include <stdint.h>

static int failures;

static void expect_i64(const char* name, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %s: got 0x%016llx want 0x%016llx\n",
               name, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

int main(void) {
    union { float f; uint32_t b; } fs;
    union { double d; uint64_t b; } ds;

    // --- scvtf s0, s0 (signed 32-bit int -> float), read via fmov w1, s0 ---
    for (int32_t v = -100000; v <= 100000; v += 7777) {
        uint32_t r;
        asm volatile("fmov s0, %w1\n\t"
                     "scvtf s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"((uint32_t)v) : "s0");
        fs.b = r;
        if (fs.f != (float)v) {
            printf("FAIL scvtf_s: %d -> %.9g want %.9g\n", v, fs.f, (float)v);
            failures++;
        }
    }
    // 32-bit wrapping negatives (bit pattern sign-extension)
    {
        uint32_t r;
        asm volatile("fmov s0, %w1\n\t"
                     "scvtf s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xFFFFFFFFu) : "s0");
        fs.b = r;
        if (fs.f != -1.0f) {
            printf("FAIL scvtf_s -1: got %.9g\n", fs.f);
            failures++;
        }
    }

    // --- ucvtf s0, s0 (unsigned 32-bit int -> float) ---
    {
        uint32_t r;
        asm volatile("fmov s0, %w1\n\t"
                     "ucvtf s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xFFFFFFFFu) : "s0");
        fs.b = r;
        if (fs.f != 4294967295.0f) {
            printf("FAIL ucvtf_s: got %.9g want 4294967295\n", fs.f);
            failures++;
        }
    }

    // --- scvtf d0, d0 (signed 64-bit int -> double) ---
    for (int64_t v = -1000000000; v <= 1000000000; v += 123456789) {
        uint64_t r;
        asm volatile("fmov d0, %1\n\t"
                     "scvtf d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"((uint64_t)v) : "d0");
        ds.b = r;
        if (ds.d != (double)v) {
            printf("FAIL scvtf_d: %lld -> %.17g want %.17g\n",
                   (long long)v, ds.d, (double)v);
            failures++;
        }
    }
    {
        uint64_t r;
        asm volatile("fmov d0, %1\n\t"
                     "scvtf d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0xFFFFFFFFFFFFFFFFULL) : "d0");
        ds.b = r;
        if (ds.d != -1.0) {
            printf("FAIL scvtf_d -1: got %.17g\n", ds.d);
            failures++;
        }
    }

    // --- ucvtf d0, d0 (unsigned 64-bit int -> double) ---
    {
        uint64_t r;
        asm volatile("fmov d0, %1\n\t"
                     "ucvtf d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0xFFFFFFFFFFFFFFFFULL) : "d0");
        ds.b = r;
        if (ds.d != 18446744073709551616.0) {
            printf("FAIL ucvtf_d max: got %.17g\n", ds.d);
            failures++;
        }
    }

    // --- fcvtzs/fcvtzu s0,s0 / d0,d0 (FP -> int, FP dest) ---
    // Load the FP register with a float/double VALUE via its bit pattern
    // (fmov copies bits). Truncate toward zero, saturate, NaN -> 0.
    // 3.5f = 0x40600000, -3.5f = 0xC0600000, 3.5 = 0x400C000000000000,
    // -3.5 = 0xC00C000000000000, NaN(f) = 0x7FC00000.
    {
        uint32_t r;
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzs s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0x40600000u) : "s0");
        expect_i64("fcvtzs_s 3.5", r, 3);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzs s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xC0600000u) : "s0");
        expect_i64("fcvtzs_s -3.5", r, (uint32_t)-3);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzu s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xC0600000u) : "s0");
        expect_i64("fcvtzu_s -3.5", r, 0);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzs s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0x7FC00000u) : "s0");
        expect_i64("fcvtzs_s NaN", r, 0);
    }
    {
        uint64_t r;
        asm volatile("fmov d0, %1\n\t"
                     "fcvtzs d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0x400C000000000000ULL) : "d0");
        expect_i64("fcvtzs_d 3.5", r, 3);
        asm volatile("fmov d0, %1\n\t"
                     "fcvtzs d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0xC00C000000000000ULL) : "d0");
        expect_i64("fcvtzs_d -3.5", r, (uint64_t)-3);
        asm volatile("fmov d0, %1\n\t"
                     "fcvtzu d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0xC00C000000000000ULL) : "d0");
        expect_i64("fcvtzu_d -3.5", r, 0);
    }
    // Saturation: 4294967040.0f = 0x4F7FFFFF (largest float < 2^32).
    //   fcvtzs (signed 32): > INT32_MAX -> INT32_MAX
    //   fcvtzu (unsigned 32): in range -> 4294967040 (0xFFFFFF00)
    {
        uint32_t r;
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzs s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0x4F7FFFFFu) : "s0");
        expect_i64("fcvtzs_s clamp_hi", r, (uint32_t)INT32_MAX);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzu s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0x4F7FFFFFu) : "s0");
        expect_i64("fcvtzu_s max", r, 0xFFFFFF00u);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzs s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xCF7FFFFFu) : "s0");
        expect_i64("fcvtzs_s clamp_lo", r, (uint32_t)INT32_MIN);
        asm volatile("fmov s0, %w1\n\t"
                     "fcvtzu s0, s0\n\t"
                     "fmov %w0, s0" : "=r"(r) : "r"(0xCF7FFFFFu) : "s0");
        expect_i64("fcvtzu_s neg", r, 0);
    }
    // Saturation: 2^64 as double = 0x43F0000000000000.
    //   fcvtzs (signed 64): > INT64_MAX -> INT64_MAX
    //   fcvtzu (unsigned 64): in range -> UINT64_MAX
    {
        uint64_t r;
        asm volatile("fmov d0, %1\n\t"
                     "fcvtzs d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0x43F0000000000000ULL) : "d0");
        expect_i64("fcvtzs_d clamp_hi", r, (uint64_t)INT64_MAX);
        asm volatile("fmov d0, %1\n\t"
                     "fcvtzu d0, d0\n\t"
                     "fmov %0, d0" : "=r"(r) : "r"(0x43F0000000000000ULL) : "d0");
        expect_i64("fcvtzu_d max", r, 0xFFFFFFFFFFFFFFFFULL);
    }

    if (failures == 0)
        printf("test_fcvt_fpsrc: ALL PASS\n");
    else
        printf("test_fcvt_fpsrc: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}