// jit_simd_fminmax_zero.c — vector FMAX/FMIN/FMAXNM/FMINNM over every pair
// of special values in .4s, .2s and .2d. x86 MIN/MAX return the second input
// on equal lanes; Arm orders +0 above -0. Ties are fixed inline and NaN
// inputs take the interpreter step. The reference follows the Arm ARM
// FPMax/FPMin/FPMaxNum/FPMinNum with FPCR.DN=0; qemu-aarch64 agrees with it.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

static int is_nan32(uint32_t x) { return (x & 0x7f800000u) == 0x7f800000u && (x & 0x7fffffu); }
static int is_snan32(uint32_t x) { return is_nan32(x) && !(x & 0x400000u); }
static int is_nan64(uint64_t x) {
    return (x & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (x & 0xfffffffffffffull);
}
static int is_snan64(uint64_t x) { return is_nan64(x) && !(x & 0x8000000000000ull); }

// op: 0 FMAX, 1 FMIN, 2 FMAXNM, 3 FMINNM.
static uint32_t ref32(int op, uint32_t a, uint32_t b) {
    if (op >= 2 && is_nan32(a) != is_nan32(b)) {  // FPMaxNum/FPMinNum: a lone quiet NaN loses
        if (is_nan32(a) && !is_snan32(a)) return b;
        if (is_nan32(b) && !is_snan32(b)) return a;
    }
    if (is_snan32(a)) return a | 0x400000u;
    if (is_snan32(b)) return b | 0x400000u;
    if (is_nan32(a)) return a;
    if (is_nan32(b)) return b;
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    if (fa == fb) return (op & 1) ? (a | b) : (a & b);  // signed zeros
    return ((op & 1) ? fa < fb : fa > fb) ? a : b;
}
static uint64_t ref64(int op, uint64_t a, uint64_t b) {
    if (op >= 2 && is_nan64(a) != is_nan64(b)) {
        if (is_nan64(a) && !is_snan64(a)) return b;
        if (is_nan64(b) && !is_snan64(b)) return a;
    }
    if (is_snan64(a)) return a | 0x8000000000000ull;
    if (is_snan64(b)) return b | 0x8000000000000ull;
    if (is_nan64(a)) return a;
    if (is_nan64(b)) return b;
    double da, db;
    memcpy(&da, &a, 8); memcpy(&db, &b, 8);
    if (da == db) return (op & 1) ? (a | b) : (a & b);
    return ((op & 1) ? da < db : da > db) ? a : b;
}

#define VOP(insn, arr, d, a, b)                                                     \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n movi v2.2d, #0\n" insn " v2." arr \
                     ", v0." arr ", v1." arr "\n str q2, [%0]"                       \
                     :: "r"(d), "r"(a), "r"(b) : "v0", "v1", "v2", "memory")
#define RUN4(op, arr, d, a, b)                       \
    switch (op) {                                    \
    case 0: VOP("fmax", arr, d, a, b); break;        \
    case 1: VOP("fmin", arr, d, a, b); break;        \
    case 2: VOP("fmaxnm", arr, d, a, b); break;      \
    default: VOP("fminnm", arr, d, a, b); break;     \
    }

static const char* const names[] = {"fmax", "fmin", "fmaxnm", "fminnm"};

int main(void) {
    static const uint32_t s[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u,
                                 0x00000001u, 0x80000001u, 0x7f800000u, 0xff800000u,
                                 0x7fc00001u, 0xffc00002u, 0x7f800003u, 0xff800004u};
    static const uint64_t d[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull,
                                 0xbff0000000000000ull, 1, 0x8000000000000001ull,
                                 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                 0x7ff8000000000001ull, 0xfff8000000000002ull,
                                 0x7ff0000000000003ull, 0xfff0000000000004ull};
    const int n = sizeof(s) / sizeof(s[0]);
    for (int op = 0; op < 4; ++op) {
        // .4s: four pairs per instruction, so NaN and zero lanes share vectors.
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; j += 4) {
                uint32_t a[4], b[4], out[4];
                for (int k = 0; k < 4; ++k) { a[k] = s[i]; b[k] = s[(j + k) % n]; }
                RUN4(op, "4s", out, a, b);
                for (int k = 0; k < 4; ++k, ++checks)
                    if (out[k] != ref32(op, a[k], b[k])) {
                        ++failures;
                        printf("FAIL %s.4s %08x,%08x -> %08x want %08x\n", names[op], a[k], b[k],
                               out[k], ref32(op, a[k], b[k]));
                    }
            }
        // .2s: inactive upper lanes hold NaNs and must not matter.
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                uint32_t a[4] = {s[i], s[j], 0x7f800003u, 0}, b[4] = {s[j], s[i], 0, 0x7fc00001u};
                uint32_t out[4];
                RUN4(op, "2s", out, a, b);
                checks += 3;
                if (out[0] != ref32(op, a[0], b[0]) || out[1] != ref32(op, a[1], b[1]) ||
                    out[2] || out[3]) {
                    ++failures;
                    printf("FAIL %s.2s %08x,%08x -> %08x %08x %08x %08x\n", names[op], s[i], s[j],
                           out[0], out[1], out[2], out[3]);
                }
            }
        // .2d
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                uint64_t a[2] = {d[i], d[j]}, b[2] = {d[j], d[i]}, out[2];
                RUN4(op, "2d", out, a, b);
                for (int k = 0; k < 2; ++k, ++checks)
                    if (out[k] != ref64(op, a[k], b[k])) {
                        ++failures;
                        printf("FAIL %s.2d %016llx,%016llx -> %016llx\n", names[op],
                               (unsigned long long)a[k], (unsigned long long)b[k],
                               (unsigned long long)out[k]);
                    }
            }
    }
    printf("simd_fminmax_zero: %d checks %s\n", checks, failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
