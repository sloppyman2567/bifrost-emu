// jit_fp_scalar_fminmax.c — scalar FMAX/FMIN/FMAXNM/FMINNM (S and D) over every pair of special values: signed
// zeros, infinities, one, the smallest subnormal, quiet and signalling NaNs. x86 MAX/MIN return the second input on
// equal operands and when either is a NaN; Arm orders +0 above -0. Equal operands are fixed inline (so a clamp to zero
// stays native) and NaN inputs take the interpreter step. The reference follows the Arm ARM
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

// The destination starts as an all-ones pattern: a scalar result must clear every bit above it.
#define SOP(insn, reg, d, a, b)                                                                   \
    __asm__ volatile("ldr " reg "0, [%1]\n ldr " reg "1, [%2]\n movi v2.2d, #0xffffffffffffffff\n" insn " " reg  \
                     "2, " reg "0, " reg "1\n str q2, [%0]"                                       \
                     :: "r"(d), "r"(a), "r"(b) : "v0", "v1", "v2", "memory")
#define RUN4(op, reg, d, a, b)                       \
    switch (op) {                                    \
    case 0: SOP("fmax", reg, d, a, b); break;        \
    case 1: SOP("fmin", reg, d, a, b); break;        \
    case 2: SOP("fmaxnm", reg, d, a, b); break;      \
    default: SOP("fminnm", reg, d, a, b); break;     \
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
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                // The other lanes of the sources hold NaNs and must not matter; the result's upper bits are zero.
                uint32_t a[4] = {s[i], 0x7f800003u, 0x7fc00001u, 1}, b[4] = {s[j], 0xffc00002u, 3, 0x7fc00001u};
                uint32_t out[4];
                RUN4(op, "s", out, a, b);
                checks += 2;
                if (out[0] != ref32(op, s[i], s[j]) || out[1] || out[2] || out[3]) {
                    ++failures;
                    printf("FAIL %s s %08x,%08x -> %08x %08x %08x %08x want %08x\n", names[op], s[i], s[j], out[0],
                           out[1], out[2], out[3], ref32(op, s[i], s[j]));
                }
            }
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                uint64_t a[2] = {d[i], 0x7ff0000000000003ull}, b[2] = {d[j], 0xfff0000000000004ull}, out[2];
                RUN4(op, "d", out, a, b);
                checks += 2;
                if (out[0] != ref64(op, d[i], d[j]) || out[1]) {
                    ++failures;
                    printf("FAIL %s d %016llx,%016llx -> %016llx %016llx want %016llx\n", names[op],
                           (unsigned long long)d[i], (unsigned long long)d[j], (unsigned long long)out[0],
                           (unsigned long long)out[1], (unsigned long long)ref64(op, d[i], d[j]));
                }
            }
    }
    printf("fp_scalar_fminmax: %d checks %s\n", checks, failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
