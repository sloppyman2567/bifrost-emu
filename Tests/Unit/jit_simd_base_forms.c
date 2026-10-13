/*
 * jit_simd_base_forms.c — base ARMv8.0 FP/SIMD forms found by the coverage
 * sweep (Scripts/Testing/simd_coverage.py).
 *
 * Each form runs over boundary inputs (Vn = v0, Vm = v1 and v17, Vd = v2
 * preloaded as accumulator) and the result plus FPSR.QC are hashed per
 * encoding, then compared with a table produced by QEMU 11.1 with
 * `-cpu cortex-a53`, an ARMv8.0 profile without FP16 (`jit_simd_base_forms
 * gen` prints it; `-cpu max` gives the same table). Covered: compare with zero (CMLE
 * used to run as CMEQ), saturating add/sub (JIT UQSUB.h, QC), SUQADD/USQADD,
 * SQXTN family (QC), URECPE/URSQRTE, vector and scalar FCVT{N,P,M,A,Z}{S,U},
 * AES, register and immediate shifts, halving ops, SQDMULH/SQRDMULH,
 * SQDMLAL/SQDMLSL/SQDMULL in every shape, SABA/UABA, PMUL, by-element forms
 * with M:Rm, TBL/TBX with one table register, EXT/INS, and the scalar forms
 * that execute as element 0 of the vector operation. The reserved encodings
 * below must raise SIGILL, as under QEMU.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FORMS(X) \
    /* Compare with zero, CNT/NOT/RBIT */ \
    X(0x2e209802, 1) X(0x6ee09802, 1) X(0x0e209802, 1) X(0x4ea08802, 1) \
    X(0x6e608802, 1) X(0x0e20a802, 1) X(0x4e205802, 1) X(0x6e205802, 1) \
    X(0x6e605802, 1) \
    /* SUQADD/USQADD/SQABS/SQNEG, narrowing, unsigned estimates */ \
    X(0x4e203802, 1) X(0x6e603802, 1) X(0x6ea03802, 1) X(0x6ee03802, 1) \
    X(0x4e207802, 1) X(0x6ea07802, 1) X(0x0e214802, 1) X(0x4e614802, 1) \
    X(0x2ea14802, 1) X(0x2e212802, 1) X(0x6e612802, 1) X(0x0ea1c802, 1) \
    X(0x4ea1c802, 1) X(0x2ea1c802, 1) X(0x6ea1c802, 1) \
    /* Vector FCVT{N,P,M,A}{S,U} */ \
    X(0x4e21a802, 4) X(0x6e61a802, 8) X(0x4ea1a802, 4) X(0x6ee1a802, 8) \
    X(0x4e21b802, 4) X(0x6e61b802, 8) X(0x4e21c802, 4) X(0x6e61c802, 8) \
    X(0x0e21c802, 4) \
    /* Scalar FCVT*, two-register misc, FRECPX, FMAXNMP/FMINNMP */ \
    X(0x5e21a802, 4) X(0x7e61a802, 8) X(0x5ea1a802, 4) X(0x5e61b802, 8) \
    X(0x7e21b802, 4) X(0x5e21c802, 4) X(0x7e61c802, 8) X(0x5ea1b802, 4) \
    X(0x7ee1b802, 8) X(0x5e203802, 1) X(0x7e603802, 1) X(0x5ea07802, 1) \
    X(0x7ee07802, 1) X(0x5ee0b802, 1) X(0x7ee0b802, 1) X(0x5e214802, 1) \
    X(0x7e614802, 1) X(0x7ea12802, 1) X(0x5ea1f802, 4) X(0x5ee1f802, 8) \
    X(0x7e30c802, 4) X(0x7ef0c802, 8) \
    /* Three-same: saturating add/sub, shifts, halving, doubling multiply */ \
    X(0x6e612c02, 1) X(0x2e612c02, 1) X(0x4e610c02, 1) X(0x6e210c02, 1) \
    X(0x4e212c02, 1) X(0x4ea10c02, 1) X(0x6ee12c02, 1) X(0x4e214c02, 1) \
    X(0x6e614c02, 1) X(0x4ea15c02, 1) X(0x6ee15c02, 1) X(0x4e615402, 1) \
    X(0x6ea15402, 1) X(0x6e212402, 1) X(0x4e610402, 1) X(0x6ea11402, 1) \
    X(0x4e61b402, 1) X(0x6ea1b402, 1) X(0x0e61b402, 1) X(0x4e217c02, 1) \
    X(0x6e617c02, 1) X(0x0ea17c02, 1) X(0x6e219c02, 1) \
    /* Three-different */ \
    X(0x0e619002, 1) X(0x4ea19002, 1) X(0x0ea1b002, 1) X(0x4e61b002, 1) \
    X(0x0e61d002, 1) X(0x4ea1d002, 1) X(0x4ea14002, 1) \
    /* Scalar three-same and three-different */ \
    X(0x5e210c02, 1) X(0x7e610c02, 1) X(0x5ea12c02, 1) X(0x7ee12c02, 1) \
    X(0x5ee14402, 1) X(0x7ee14402, 1) X(0x5e214c02, 1) X(0x7ee14c02, 1) \
    X(0x5ee15402, 1) X(0x7ee15402, 1) X(0x5e615c02, 1) X(0x7ea15c02, 1) \
    X(0x5e61b402, 1) X(0x7ea1b402, 1) X(0x5e21dc02, 4) X(0x5e61dc02, 8) \
    X(0x5e619002, 1) X(0x5ea1b002, 1) X(0x5e61d002, 1) \
    /* By element, vector and scalar (v17 exercises M:Rm for .s) */ \
    X(0x4f71c802, 1) X(0x4fb1d802, 1) X(0x0fb13002, 1) X(0x4f51b802, 1) \
    X(0x0f61a002, 1) X(0x4f918802, 1) X(0x6f610802, 1) X(0x5f71c002, 1) \
    X(0x5f91d802, 1) X(0x5fb13802, 1) X(0x5f51b002, 1) X(0x5fa11802, 4) \
    X(0x5fc11802, 8) X(0x7fc19802, 8) \
    /* Shift by immediate: scalar and vector narrowing, wide shifts */ \
    X(0x5f0b9402, 1) X(0x5f139c02, 1) X(0x7f2b8402, 1) X(0x7f278c02, 1) \
    X(0x7f219402, 1) X(0x7f0f9c02, 1) X(0x4f0b9402, 1) X(0x0f219c02, 1) \
    X(0x4f415402, 1) X(0x6f7f0402, 1) X(0x0f08a402, 1) X(0x0f108402, 1) \
    /* AES, TBL/TBX with one table register, EXT, INS */ \
    X(0x4e284802, 1) X(0x4e285802, 1) X(0x4e286802, 1) X(0x4e287802, 1) \
    X(0x0e010002, 1) X(0x0e011002, 1) X(0x2e013802, 1) X(0x6e017802, 1) \
    X(0x6e0d6402, 1)

/* Reserved or unallocated in this CPU profile: SIGILL under QEMU too. */
#define RESERVED(X) \
    X(0x2e20a802) /* CMLT #0 with U = 1 */          X(0x2ee09802) /* CMLE #0 .1d */ \
    X(0x4ee1c802) /* URECPE .2d */                  X(0x0e61d802) /* SCVTF .1d */ \
    X(0x0e20d802) /* SCVTF space, bit 16 clear */  X(0x0ee0b802) /* ABS .1d */ \
    X(0x0e605802) /* CNT with size 1 */             X(0x2e004002) /* EXT .8b #8 */ \
    X(0x6e008002) /* EXT with bit 15 set */         X(0x2e010402) /* INS (element), Q = 0 */ \
    X(0x1f800002) /* FMADD, ftype 10 */             X(0x1ee01002) /* FMOV h, #imm (FP16) */ \
    X(0x1e202002) /* FCMP, opcode2<2:0> != 0 */     X(0x1e209802) /* FP 2-source opcode 1001 */ \
    X(0x1e020002) /* SCVTF s, w, #fbits > 32 */     X(0x9ea20002) /* SCVTF, ftype 10 */ \
    X(0x0fa02402) /* bits 23 and 10 set */          X(0x0f405402) /* SHL .1d */ \
    X(0x0f408402) /* SHRN, immh 1xxx */             X(0x0f40a402) /* SSHLL, immh 1xxx */ \
    X(0x0ee13402) /* CMGT .1d */                    X(0x4ee16402) /* SMAX .2d */ \
    X(0x0ee10002) /* SADDL, size 3 */               X(0x0e61e002) /* PMULL, size 1 */ \
    X(0x0e21b402) /* SQDMULH, size 0 */             X(0x5e214402) /* SSHL b */ \
    X(0x5ea0b802) /* ABS s */                       X(0x5ee14802) /* SQXTN from d */ \
    X(0x5f409402) /* SQSHRN, immh 1xxx */           X(0x0e61c402) /* FMAXNM .1d */

typedef struct { uint8_t b[16]; } V;
#define RUN(word) __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n ldr q17, [%2]\n" \
    "ldr q2, [%3]\n .inst " #word "\n str q2, [%0]" :: "r"(out), "r"(n), "r"(m), "r"(d) \
    : "v0", "v1", "v2", "v17", "memory")
static void run(uint32_t word, const V* n, const V* m, const V* d, V* out) {
    switch (word) {
#define CASE(w, t) case (w): RUN(w); break;
        FORMS(CASE)
#undef CASE
    }
}
static const struct { uint32_t word; unsigned type; } forms[] = {
#define ENTRY(w, t) {(w), (t)},
    FORMS(ENTRY)
#undef ENTRY
};
#define NFORMS (sizeof(forms) / sizeof(forms[0]))

static const uint8_t bytes[] = {
    0x00, 0x01, 0x7f, 0x80, 0x81, 0xff, 0xfe, 0x40, 0xc0, 0x3f, 0x10, 0x08,
    0x20, 0xe0, 0x55, 0xaa, 0x02, 0xfd,
};
static const uint32_t f32s[] = {
    0x00000000, 0x80000000, 0x3fc00000, 0x40200000, 0xc0200000, 0x3f000000,
    0xbf000000, 0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001, 0x7f7fffff,
    0x00000001, 0x4f000000, 0xcf000001, 0x4f800000, 0x3fa00000, 0xbfe00000,
};
static const uint64_t f64s[] = {
    0, 0x8000000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
    0xc004000000000000ull, 0xbff8000000000000ull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000001ull, 0x7ff0000000000001ull,
    0x7fefffffffffffffull, 0x0000000000000001ull, 0x43e0000000000000ull,
    0xc3e0000000000001ull, 0x43f0000000000000ull, 0x3fe8000000000000ull,
};

static void fill(V* v, unsigned type, unsigned k) {
    for (unsigned lane = 0; lane < 16 / type; ++lane) {
        const unsigned sel = k * 7 + lane * (k % 5 + 1) + k / 3;
        if (type == 4) memcpy(v->b + 4 * lane, &f32s[sel % (sizeof(f32s) / 4)], 4);
        else if (type == 8) memcpy(v->b + 8 * lane, &f64s[sel % (sizeof(f64s) / 8)], 8);
        else v->b[lane] = k & 2 ? bytes[sel % sizeof(bytes)] : (uint8_t)(sel * 37 + k * 11);
    }
}
static void set_fpsr(uint64_t value) { __asm__ volatile("msr fpsr, %0" :: "r"(value)); }
static uint64_t get_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }

static sigjmp_buf trap;
static void on_sigill(int sig) { (void)sig; siglongjmp(trap, 1); }

static const uint32_t expected[] = {
    0xb1da700f, 0x2472aab5, 0xd861eec1, 0xe6ced6b5, 0xbfa383f1, 0xa5a924bf,
    0xd2c74f0e, 0xc3aa687c, 0xd207a74f, 0xd49c3904, 0x751e4e6c, 0x2306eadf,
    0x77cd5a2d, 0x95bad512, 0x49598298, 0x1f3ce086, 0x50d2a355, 0xd5ee45a5,
    0x9da62653, 0x39066655, 0xab7c145b, 0xe5a67d9a, 0x84bc65c6, 0x108b84e5,
    0x570550e1, 0x1c66345f, 0xc582eb06, 0x200f20e9, 0xafc70819, 0xdfae1846,
    0x1fb11ff4, 0xad908bbb, 0x1c6b9171, 0x0bcd8715, 0x8305037d, 0x40fb9638,
    0x4470ba1d, 0x28896654, 0x1e6e131c, 0xdce13ca5, 0xddb4d2cc, 0x23d1fe05,
    0xf69401eb, 0xfcc6fb05, 0xc80c969d, 0x96518014, 0xbffaf09c, 0x96518014,
    0x47e3b3ba, 0x5750eb25, 0x4917b605, 0x0a6042d0, 0x7b9da35d, 0xea30f444,
    0xc487bfb3, 0xee51efbe, 0xb7351dc0, 0xcfc9d6cb, 0x41b68b93, 0x381766fe,
    0x930e566b, 0x40a8685a, 0x6f3d8055, 0xe811be57, 0x9e8285a5, 0x2d30d576,
    0x5f6ed0b6, 0xea6c3b21, 0x7f7538e1, 0x910667ea, 0x2b53b8bc, 0x53508c3b,
    0xc78b6849, 0x14613075, 0x8db6d2c8, 0xed686977, 0x43f12705, 0xadac7180,
    0x449df7d3, 0x1b8475a8, 0xa626d224, 0x27f57b83, 0x89445fd3, 0x50a5f143,
    0xe65797cf, 0x715405a8, 0xf773c71e, 0x7a2a9d50, 0x61f66711, 0xa28f4b7f,
    0x37c2dc62, 0xb61906ab, 0xd36e7343, 0xcbe98f98, 0x430111eb, 0x5c1d2657,
    0x1e830d16, 0xecafd425, 0x8ec77d3b, 0x3f876762, 0xc8223745, 0xefba9530,
    0x2c3ca0c6, 0x5e805e11, 0xd0a23c1c, 0xc7371c43, 0x3b2935d2, 0x576be32a,
    0x1ef22d13, 0xf4cb336f, 0x6e676aaf, 0xf230c16e, 0x3cf9a7f9, 0x986c0f61,
    0xc5eeceac, 0xbf84fb4a, 0x5b51b0f9, 0x578a000d, 0x58eb9d5f, 0xcf921531,
    0x4917b605, 0xa18a6214, 0x700f8dd4, 0xef372192, 0x7e984e12, 0xba79232a,
    0x4125e7e1, 0xea03bb38, 0x978e89c7, 0x7c508d73, 0x3f36ab23, 0x6c826477,
    0x18174054, 0x3cd49a60, 0xe96c712e, 0x099db7db, 0x359fb364, 0x32d85ae1,
    0x99f9bd04,
};

int main(int argc, char** argv) {
    const int gen = argc > 1 && !strcmp(argv[1], "gen");
    int fails = 0, checks = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, 0);
    for (unsigned f = 0; f < NFORMS; ++f) {
        uint32_t h = 2166136261u;
        ++checks;
        if (sigsetjmp(trap, 1)) {
            printf("FAIL %08x raised SIGILL\n", forms[f].word);
            ++fails;
            continue;
        }
        for (unsigned k = 0; k < 96; ++k) {
            V n, m, d, out;
            fill(&n, forms[f].type, k);
            fill(&m, forms[f].type, k + 17);
            for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0xa5 ^ (i * 29 + k));
            memset(&out, 0, sizeof(out));
            set_fpsr(0);
            run(forms[f].word, &n, &m, &d, &out);
            const uint8_t qc = (get_fpsr() >> 27) & 1;
            for (int i = 0; i < 16; ++i) h = (h ^ out.b[i]) * 16777619u;
            h = (h ^ qc) * 16777619u;
        }
        if (gen) printf("0x%08x,%s", h, f % 6 == 5 ? "\n" : " ");
        else if (sizeof(expected) / 4 != NFORMS || h != expected[f]) {
            printf("FAIL %08x hash=%08x\n", forms[f].word, h);
            ++fails;
        }
    }
    if (gen) { printf("\n"); return 0; }

#define TRAPS(word) do { \
        ++checks; \
        if (!sigsetjmp(trap, 1)) { \
            __asm__ volatile(".inst " #word ::: "v2", "x2", "memory"); \
            printf("FAIL %s executed instead of SIGILL\n", #word); ++fails; \
        } \
    } while (0);
    RESERVED(TRAPS)
#undef TRAPS

    if (fails) { printf("jit_simd_base_forms: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_base_forms: %d checks passed\n", checks);
    return 0;
}
