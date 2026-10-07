// jit_simd_fp_nan.c — vector FP special cases follow the ARM ARM
// (FPCR.DN=0): with two NaN operands the first signalling NaN wins, else
// the first quiet NaN, quietened; invalid operations give +qNaN; FMAX/FMIN
// order +0 above -0; FMAXNM/FMINNM prefer a number over a lone quiet NaN;
// FMLA/FMLS are fused and FMLS negates its multiplicand (NaNs included);
// FMULX makes inf*0 = ±2; FABD clears the NaN sign; FADDP pairs in order.
//
// The interpreter used host C++ arithmetic, so the NaN it returned depended
// on how GCC ordered the operands. Expected values match qemu-aarch64.
// The JIT's native vector FP still uses x86 NaN rules, so the suite runs
// this test in the interpreter only (INTERP row).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define OP_S(insn, d, a, b)                                                   \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%0]\n" insn       \
                     " v2.4s, v0.4s, v1.4s\n str q2, [%0]\n"                   \
                     :: "r"(d), "r"(a), "r"(b) : "v0", "v1", "v2", "memory")
#define OP_D(insn, d, a, b)                                                   \
    __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q2, [%0]\n" insn       \
                     " v2.2d, v0.2d, v1.2d\n str q2, [%0]\n"                   \
                     :: "r"(d), "r"(a), "r"(b) : "v0", "v1", "v2", "memory")

static void expect_s(const char* what, const uint32_t* got, const uint32_t* want) {
    checks++;
    if (memcmp(got, want, 16) != 0) {
        failures++;
        printf("FAIL %s: got %08x %08x %08x %08x want %08x %08x %08x %08x\n", what,
               got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
    }
}
static void expect_d(const char* what, const uint64_t* got, const uint64_t* want) {
    checks++;
    if (memcmp(got, want, 16) != 0) {
        failures++;
        printf("FAIL %s: got %016llx %016llx want %016llx %016llx\n", what,
               (unsigned long long)got[0], (unsigned long long)got[1],
               (unsigned long long)want[0], (unsigned long long)want[1]);
    }
}

#define QN1 0x7FC00001u   // quiet NaNs with distinct payloads
#define QN2 0x7FC00002u
#define SN3 0x7F800003u   // signalling NaN (quietens to 0x7FC00003)
#define NQN 0xFFC00007u   // negative quiet NaN
#define ONE 0x3F800000u
#define INF 0x7F800000u
#define NZ  0x80000000u
#define DN  0x7FC00000u   // ARM default NaN

int main(void) {
    uint32_t a[4], b[4], d[4];
    #define S4(x0, x1, x2, x3, arr) do { arr[0] = x0; arr[1] = x1; arr[2] = x2; arr[3] = x3; } while (0)

    // FADD: two quiet NaNs → first; quiet vs signalling → signalling;
    // inf + -inf → default NaN.
    S4(QN1, QN1, INF, ONE, a); S4(QN2, SN3, 0xFF800000u, ONE, b); S4(0, 0, 0, 0, d);
    OP_S("fadd", d, a, b);
    { uint32_t w[4] = {QN1, 0x7FC00003u, DN, 0x40000000u}; expect_s("fadd", d, w); }
    // FSUB with NaN in the second operand only: that NaN, quietened.
    S4(ONE, ONE, INF, 0, a); S4(QN2, SN3, INF, 0, b);
    OP_S("fsub", d, a, b);
    { uint32_t w[4] = {QN2, 0x7FC00003u, DN, 0}; expect_s("fsub", d, w); }
    // FMUL 0 * inf → default NaN.
    S4(0, NZ, ONE, ONE, a); S4(INF, INF, ONE, QN1, b);
    OP_S("fmul", d, a, b);
    { uint32_t w[4] = {DN, DN, ONE, QN1}; expect_s("fmul", d, w); }
    // FMAX/FMIN signed zeros and NaN propagation.
    S4(0, NZ, NZ, QN1, a); S4(NZ, 0, NZ, ONE, b);
    OP_S("fmax", d, a, b);
    { uint32_t w[4] = {0, 0, NZ, QN1}; expect_s("fmax", d, w); }
    OP_S("fmin", d, a, b);
    { uint32_t w[4] = {NZ, NZ, NZ, QN1}; expect_s("fmin", d, w); }
    // FMAXNM/FMINNM: a lone quiet NaN loses; a signalling NaN propagates.
    S4(QN1, ONE, SN3, QN1, a); S4(ONE, QN2, ONE, QN2, b);
    OP_S("fmaxnm", d, a, b);
    { uint32_t w[4] = {ONE, ONE, 0x7FC00003u, QN1}; expect_s("fmaxnm", d, w); }
    OP_S("fminnm", d, a, b);
    { uint32_t w[4] = {ONE, ONE, 0x7FC00003u, QN1}; expect_s("fminnm", d, w); }
    // FMLA: fused (exact (1+2^-23)^2 - (1+2^-22) = 2^-46), a quiet-NaN
    // addend with inf*0 → default NaN, NaN choice addend first.
    S4(0x3F800001u, 0, ONE, QN1, a); S4(0x3F800001u, INF, ONE, QN2, b);
    S4(0xBF800002u, 0x7FC00005u, QN2, 0x7FC00006u, d);
    OP_S("fmla", d, a, b);
    { uint32_t w[4] = {0x28800000u, DN, QN2, 0x7FC00006u}; expect_s("fmla", d, w); }
    // FMLS negates the multiplicand before FPMulAdd, so its NaN flips sign.
    S4(QN1, ONE, 0x3F800001u, ONE, a); S4(ONE, ONE, 0x3F800001u, ONE, b);
    S4(ONE, ONE, 0x3F800002u, 0x40000000u, d);
    OP_S("fmls", d, a, b);
    { uint32_t w[4] = {0xFFC00001u, 0, 0xA8800000u, ONE}; expect_s("fmls", d, w); }
    // FMULX: inf * 0 = 2 with the XOR of the signs.
    S4(INF, INF, 0xFF800000u, ONE, a); S4(0, NZ, NZ, 0x40000000u, b);
    OP_S("fmulx", d, a, b);
    { uint32_t w[4] = {0x40000000u, 0xC0000000u, 0x40000000u, 0x40000000u}; expect_s("fmulx", d, w); }
    // FABD clears the sign of a NaN result.
    S4(NQN, ONE, 0, 0, a); S4(ONE, NQN, ONE, 0, b);
    OP_S("fabd", d, a, b);
    { uint32_t w[4] = {0x7FC00007u, 0x7FC00007u, ONE, 0}; expect_s("fabd", d, w); }
    // FADDP: pairs of Vn then Vm, first NaN of each pair.
    S4(QN1, QN2, ONE, ONE, a); S4(SN3, QN1, INF, 0xFF800000u, b);
    OP_S("faddp", d, a, b);
    { uint32_t w[4] = {QN1, 0x40000000u, 0x7FC00003u, DN}; expect_s("faddp", d, w); }

    // Double precision: same NaN choice and +qNaN default.
    uint64_t da[2] = {0x7FF8000000000001ull, 0x7FF0000000000000ull};
    uint64_t db[2] = {0x7FF8000000000002ull, 0xFFF0000000000000ull};
    uint64_t dd[2] = {0, 0};
    OP_D("fadd", dd, da, db);
    { uint64_t w[2] = {0x7FF8000000000001ull, 0x7FF8000000000000ull}; expect_d("fadd.2d", dd, w); }
    uint64_t ea[2] = {0x7FF0000000000003ull, 0x8000000000000000ull};
    uint64_t eb[2] = {0x7FF8000000000004ull, 0x0000000000000000ull};
    OP_D("fmax", dd, ea, eb);
    { uint64_t w[2] = {0x7FF8000000000003ull, 0}; expect_d("fmax.2d", dd, w); }

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d/%d checks passed\n", checks, checks);
    return 0;
}
