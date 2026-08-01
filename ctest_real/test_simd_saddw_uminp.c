/* test_simd_saddw_uminp.c — SADDW/SADDW2 and UMINP vector regression.
 *
 * SADDW (signed add wide, 0x0E201000 group) sign-extends the narrow lanes
 * of Vm and adds them to the wide lanes of Vn; Q selects the low half
 * (SADDW) or high half (SADDW2) of Vm. UMINP (0x2E20AC00 group) does a
 * pairwise unsigned min over adjacent lanes of Vn (low half of Vd) and Vm
 * (high half of Vd). These are used by glibc/busybox/iperf3; they were
 * silently NOP'd before v1.5.1-alpha surfaced missing SIMD ops as SIGILL.
 *
 * Build: make cross SRC=ctest_real/test_simd_saddw_uminp.c OUT=ctest_real/test_simd_saddw_uminp.elf
 * Run:   ./bifrost-emu ctest_real/test_simd_saddw_uminp.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t vn[4] __attribute__((aligned(16)));
static uint32_t vm[4] __attribute__((aligned(16)));
static uint32_t vr[4] __attribute__((aligned(16)));

#define RUN1(insn, load_n, load_m, store) \
    asm volatile(load_n "\n" load_m "\n" insn "\n" store "\n" \
        :: [a]"r"(vn), [b]"r"(vm), [c]"r"(vr) : "memory")

static void expect_saddw(uint32_t* out, int esize_src, int use_high) {
    int lanes = esize_src == 2 ? 4 : 2;
    int src_off = use_high ? lanes : 0;
    for (int i = 0; i < lanes; i++) {
        uint64_t wide = 0, narrow = 0;
        memcpy(&wide, (uint8_t*)vn + i * (2 * esize_src), 2 * esize_src);
        memcpy(&narrow, (uint8_t*)vm + (src_off + i) * esize_src, esize_src);
        int64_t se = esize_src == 2 ? (int16_t)narrow : (int32_t)narrow;
        uint64_t r = wide + (uint64_t)se;
        memcpy((uint8_t*)out + i * (2 * esize_src), &r, 2 * esize_src);
    }
}

static void expect_uminp(uint8_t* out, int esize, int q) {
    int elems_per_src = (q ? 16 : 8) / esize;
    int half = elems_per_src / 2;
    for (int i = 0; i < half; i++) {
        for (int src = 0; src < 2; src++) {
            const uint8_t* s = src == 0 ? (uint8_t*)vn : (uint8_t*)vm;
            uint64_t a = 0, b = 0;
            memcpy(&a, s + (2 * i) * esize, esize);
            memcpy(&b, s + (2 * i + 1) * esize, esize);
            uint64_t r = a < b ? a : b;
            memcpy(out + (src * half + i) * esize, &r, esize);
        }
    }
}

int main(void) {
    uint32_t exp[4];
    uint8_t expb[16];

    /* ── SADDW v.4s ← v.4s + v.4h (low half of Vm) ── */
    memcpy(vn, (uint32_t[]){1, 2, 3, 4}, 16);
    memcpy(vm, (uint32_t[]){0xFFF00000, 0x00020004, 0xFFFF0001, 0x00000000}, 16);
    RUN1("saddw v2.4s, v0.4s, v1.4h", "ld1 {v0.4s}, [%[a]]",
         "ld1 {v1.4s}, [%[b]]", "st1 {v2.4s}, [%[c]]");
    expect_saddw(exp, 2, 0);
    CK(memcmp(vr, exp, 16) == 0, "saddw 4s low half");

    /* ── SADDW2 v.4s ← v.4s + v.8h (high half of Vm) ── */
    RUN1("saddw2 v2.4s, v0.4s, v1.8h", "ld1 {v0.4s}, [%[a]]",
         "ld1 {v1.4s}, [%[b]]", "st1 {v2.4s}, [%[c]]");
    expect_saddw(exp, 2, 1);
    CK(memcmp(vr, exp, 16) == 0, "saddw2 4s high half");

    /* ── SADDW v.2d ← v.2d + v.2s (low half of Vm) ── */
    memcpy(vn, (uint32_t[]){1000000000, 0, 2000000000, 0}, 16);
    memcpy(vm, (uint32_t[]){0xFFFFFFF0, 0x00000002, 0x00000004, 0xFFFFFFFC}, 16);
    RUN1("saddw v2.2d, v0.2d, v1.2s", "ld1 {v0.2d}, [%[a]]",
         "ld1 {v1.4s}, [%[b]]", "st1 {v2.2d}, [%[c]]");
    expect_saddw(exp, 4, 0);
    CK(memcmp(vr, exp, 16) == 0, "saddw 2d low half");

    /* ── SADDW2 v.2d ← v.2d + v.4s (high half of Vm) ── */
    RUN1("saddw2 v2.2d, v0.2d, v1.4s", "ld1 {v0.2d}, [%[a]]",
         "ld1 {v1.4s}, [%[b]]", "st1 {v2.2d}, [%[c]]");
    expect_saddw(exp, 4, 1);
    CK(memcmp(vr, exp, 16) == 0, "saddw2 2d high half");

    /* ── UMINP v.8b ── */
    memcpy(vn, (uint32_t[]){0x00030201, 0x061005FF, 0, 0}, 16);
    memcpy(vm, (uint32_t[]){0xFF0001AA, 0x0A090807, 0, 0}, 16);
    RUN1("uminp v2.8b, v0.8b, v1.8b", "ld1 {v0.8b}, [%[a]]",
         "ld1 {v1.8b}, [%[b]]", "st1 {v2.8b}, [%[c]]");
    expect_uminp(expb, 1, 0);
    CK(memcmp(vr, expb, 8) == 0, "uminp 8b");

    /* ── UMINP v.16b ── */
    memcpy(vn, (uint32_t[]){0x00030201, 0x061005FF, 0x000E0D0C, 0x0A090807}, 16);
    memcpy(vm, (uint32_t[]){0xFF0001AA, 0x0A090807, 0x0E0D0C0B, 0x020100FF}, 16);
    RUN1("uminp v2.16b, v0.16b, v1.16b", "ld1 {v0.16b}, [%[a]]",
         "ld1 {v1.16b}, [%[b]]", "st1 {v2.16b}, [%[c]]");
    expect_uminp(expb, 1, 1);
    CK(memcmp(vr, expb, 16) == 0, "uminp 16b");

    /* ── UMINP v.4h ── */
    memcpy(vn, (uint32_t[]){0x00020001, 0x00040003, 0, 0}, 16);
    memcpy(vm, (uint32_t[]){0x00FF0010, 0x00020001, 0, 0}, 16);
    RUN1("uminp v2.4h, v0.4h, v1.4h", "ld1 {v0.4h}, [%[a]]",
         "ld1 {v1.4h}, [%[b]]", "st1 {v2.4h}, [%[c]]");
    expect_uminp(expb, 2, 0);
    CK(memcmp(vr, expb, 8) == 0, "uminp 4h");

    /* ── UMINP v.2s ── */
    memcpy(vn, (uint32_t[]){0x00000005, 0x00000002, 0, 0}, 16);
    memcpy(vm, (uint32_t[]){0x00000001, 0x00000004, 0, 0}, 16);
    RUN1("uminp v2.2s, v0.2s, v1.2s", "ld1 {v0.2s}, [%[a]]",
         "ld1 {v1.2s}, [%[b]]", "st1 {v2.2s}, [%[c]]");
    expect_uminp(expb, 4, 0);
    CK(memcmp(vr, expb, 8) == 0, "uminp 2s");

    printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
