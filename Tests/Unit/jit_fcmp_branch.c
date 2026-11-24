// Scalar fcmp feeding a conditional branch, for every ARM condition and every outcome (greater, equal, less,
// unordered, +-0, infinities, subnormals), in single and double precision.
//   live:  both exits are read by mrs, so each must store the exact NZCV of the compare.
//   dead:  scalar FP data processing and another fcmp follow, so the successor overwrites the flags.
//   fcsel: scalar FP data processing, then an fcsel that reads the flags the compare left.
//   fccmp: scalar FP data processing, then an fccmp that reads them, and mrs shows the result.
// The expected outcome comes from an integer model of the bit patterns, not from a C comparison, which the compiler
// would turn into the fcmp under test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { GT, EQ, LT, UN };
static const uint32_t NZCV_OF[4] = {0x2, 0x6, 0x8, 0x3};  // N=8 Z=4 C=2 V=1

static int cond_holds(int cc, uint32_t f) {
    const int n = !!(f & 8), z = !!(f & 4), c = !!(f & 2), v = !!(f & 1);
    switch (cc) {
        case 0: return z;             case 1: return !z;
        case 2: return c;             case 3: return !c;
        case 4: return n;             case 5: return !n;
        case 6: return v;             case 7: return !v;
        case 8: return c && !z;       case 9: return !c || z;
        case 10: return n == v;       case 11: return n != v;
        case 12: return !z && n == v; default: return z || n != v;
    }
}

static int classify(uint64_t x, uint64_t y, int dbl) {
    const uint64_t sign = dbl ? 1ULL << 63 : 1ULL << 31;
    const uint64_t expm = dbl ? 0x7FF0000000000000ULL : 0x7F800000ULL;
    const uint64_t frac = dbl ? 0x000FFFFFFFFFFFFFULL : 0x007FFFFFULL;
    const uint64_t all = dbl ? ~0ULL : 0xFFFFFFFFULL;
    if (((x & expm) == expm && (x & frac)) || ((y & expm) == expm && (y & frac))) return UN;
    if (((x | y) & ~sign) == 0) return EQ;
    const uint64_t kx = (x & sign) ? (~x & all) : (x | sign);
    const uint64_t ky = (y & sign) ? (~y & all) : (y | sign);
    return kx == ky ? EQ : kx > ky ? GT : LT;
}

#define CONDS(X) X(eq, 0) X(ne, 1) X(cs, 2) X(cc, 3) X(mi, 4) X(pl, 5) X(vs, 6) X(vc, 7) \
                 X(hi, 8) X(ls, 9) X(ge, 10) X(lt, 11) X(gt, 12) X(le, 13)

// W names the functions, T is the C type, R the FP register prefix (d or s), DST the GPR operand holding the bits.
#define DEFINE_TESTS(W, T, R, DST, CC, IDX) \
static void live_##W##_##CC(T x, T y, uint64_t* nz, uint64_t* tk) { \
    uint64_t n, t; \
    __asm__ volatile("fcmp %" R "2, %" R "3\n\tb." #CC " 1f\n\tmrs %0, nzcv\n\tmov %1, #0\n\tb 2f\n" \
                     "1:\n\tmrs %0, nzcv\n\tmov %1, #1\n2:" \
                     : "=&r"(n), "=&r"(t) : "w"(x), "w"(y) : "cc"); \
    *nz = n >> 28; *tk = t; } \
static uint64_t dead_##W##_##CC(T x, T y, T z) { \
    uint64_t t; \
    __asm__ volatile("fcmp %" R "2, %" R "3\n\tb." #CC " 1f\n\tfadd %" R "1, %" R "1, %" R "1\n\tfcmp %" R "2, %" R "2\n\tmov %0, #0\n\tb 2f\n" \
                     "1:\n\tfmul %" R "1, %" R "1, %" R "1\n\tfcmp %" R "2, %" R "2\n\tmov %0, #1\n2:" \
                     : "=&r"(t), "+w"(z) : "w"(x), "w"(y) : "cc"); \
    return t; } \
static void fcsel_##W##_##CC(T x, T y, T a, T b, T z, uint64_t* res, uint64_t* tk) { \
    uint64_t r, t; T sel; \
    __asm__ volatile("fcmp %" R "4, %" R "5\n\tb." #CC " 1f\n\tfadd %" R "2, %" R "2, %" R "2\n\tfcsel %" R "3, %" R "6, %" R "7, lt\n\tmov %1, #0\n\tb 2f\n" \
                     "1:\n\tfadd %" R "2, %" R "2, %" R "2\n\tfcsel %" R "3, %" R "6, %" R "7, lt\n\tmov %1, #1\n2:\n\t" \
                     "fmov " DST ", %" R "3" \
                     : "=&r"(r), "=&r"(t), "+w"(z), "=&w"(sel) : "w"(x), "w"(y), "w"(a), "w"(b) : "cc"); \
    *res = r; *tk = t; } \
static void fccmp_##W##_##CC(T x, T y, T a, T b, T z, uint64_t* nz, uint64_t* tk) { \
    uint64_t n, t; \
    __asm__ volatile("fcmp %" R "3, %" R "4\n\tb." #CC " 1f\n\tfadd %" R "2, %" R "2, %" R "2\n\tfccmp %" R "5, %" R "6, #0, lt\n\tmrs %0, nzcv\n\tmov %1, #0\n\tb 2f\n" \
                     "1:\n\tfadd %" R "2, %" R "2, %" R "2\n\tfccmp %" R "5, %" R "6, #0, lt\n\tmrs %0, nzcv\n\tmov %1, #1\n2:" \
                     : "=&r"(n), "=&r"(t), "+w"(z) : "w"(x), "w"(y), "w"(a), "w"(b) : "cc"); \
    *nz = n >> 28; *tk = t; }

#define DEFINE_D(CC, IDX) DEFINE_TESTS(d, double, "d", "%0", CC, IDX)
#define DEFINE_S(CC, IDX) DEFINE_TESTS(s, float, "s", "%w0", CC, IDX)
CONDS(DEFINE_D)
CONDS(DEFINE_S)

typedef struct { uint64_t bits[16]; int count; } Values;
static const Values DV = {{0x0000000000000000ULL, 0x8000000000000000ULL, 0x3FF0000000000000ULL, 0xBFF0000000000000ULL,
    0x4000000000000000ULL, 0x3FE0000000000000ULL, 0x7E37E43C8800759CULL, 0xFE37E43C8800759CULL,
    0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0x7FF4000000000000ULL,
    0x0000000000000001ULL, 0x8000000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0xC000000000000000ULL}, 16};
static const Values SV = {{0x00000000ULL, 0x80000000ULL, 0x3F800000ULL, 0xBF800000ULL,
    0x40000000ULL, 0x3F000000ULL, 0x7149F2CAULL, 0xF149F2CAULL,
    0x7F800000ULL, 0xFF800000ULL, 0x7FC00000ULL, 0x7FA00000ULL,
    0x00000001ULL, 0x80000001ULL, 0x007FFFFFULL, 0xC0000000ULL}, 16};

static int fails = 0, shown = 0;
static void bad(const char* what, int dbl, int cc, uint64_t x, uint64_t y, uint64_t got, uint64_t want) {
    fails++;
    if (shown++ < 12)
        printf("FAIL %s %s cond=%d x=%llx y=%llx got=%llx want=%llx\n", what, dbl ? "f64" : "f32", cc,
               (unsigned long long)x, (unsigned long long)y, (unsigned long long)got, (unsigned long long)want);
}

#define RUN_DOUBLE(CC, IDX) \
    for (int i = 0; i < DV.count; i++) for (int j = 0; j < DV.count; j++) { \
        const uint64_t xb = DV.bits[i], yb = DV.bits[j], one = 0x3FF0000000000000ULL, two = 0x4000000000000000ULL; \
        double x, y, a, b, z = 1.5; uint64_t nz, tk, res; \
        memcpy(&x, &xb, 8); memcpy(&y, &yb, 8); memcpy(&a, &one, 8); memcpy(&b, &two, 8); \
        const uint32_t f = NZCV_OF[classify(xb, yb, 1)]; const int taken = cond_holds(IDX, f); \
        live_d_##CC(x, y, &nz, &tk); \
        if (nz != f || tk != (uint64_t)taken) bad("live", 1, IDX, xb, yb, nz << 1 | tk, f << 1 | taken); \
        if (dead_d_##CC(x, y, z) != (uint64_t)taken) bad("dead", 1, IDX, xb, yb, 0, taken); \
        fcsel_d_##CC(x, y, a, b, z, &res, &tk); \
        if (tk != (uint64_t)taken || res != (cond_holds(11, f) ? one : two)) bad("fcsel", 1, IDX, xb, yb, res, cond_holds(11, f) ? one : two); \
        fccmp_d_##CC(x, y, a, b, z, &nz, &tk); \
        if (tk != (uint64_t)taken || nz != (cond_holds(11, f) ? 0x8u : 0x0u)) bad("fccmp", 1, IDX, xb, yb, nz, cond_holds(11, f) ? 8 : 0); \
    }
#define RUN_SINGLE(CC, IDX) \
    for (int i = 0; i < SV.count; i++) for (int j = 0; j < SV.count; j++) { \
        const uint32_t xb = (uint32_t)SV.bits[i], yb = (uint32_t)SV.bits[j], one = 0x3F800000u, two = 0x40000000u; \
        float x, y, a, b, z = 1.5f; uint64_t nz, tk, res; \
        memcpy(&x, &xb, 4); memcpy(&y, &yb, 4); memcpy(&a, &one, 4); memcpy(&b, &two, 4); \
        const uint32_t f = NZCV_OF[classify(xb, yb, 0)]; const int taken = cond_holds(IDX, f); \
        live_s_##CC(x, y, &nz, &tk); \
        if (nz != f || tk != (uint64_t)taken) bad("live", 0, IDX, xb, yb, nz << 1 | tk, f << 1 | taken); \
        if (dead_s_##CC(x, y, z) != (uint64_t)taken) bad("dead", 0, IDX, xb, yb, 0, taken); \
        fcsel_s_##CC(x, y, a, b, z, &res, &tk); \
        if (tk != (uint64_t)taken || (uint32_t)res != (cond_holds(11, f) ? one : two)) bad("fcsel", 0, IDX, xb, yb, res, cond_holds(11, f) ? one : two); \
        fccmp_s_##CC(x, y, a, b, z, &nz, &tk); \
        if (tk != (uint64_t)taken || nz != (cond_holds(11, f) ? 0x8u : 0x0u)) bad("fccmp", 0, IDX, xb, yb, nz, cond_holds(11, f) ? 8 : 0); \
    }

// Loops whose back edge is the fcmp + branch (a self-loop block): x counts up by 1 against a limit, or down against #0.0.
#define UPLOOP(W, T, R, CC) \
static uint64_t up_##W##_##CC(T x, T step, T lim) { \
    uint64_t n = 0; \
    __asm__ volatile("1:\n\tadd %0, %0, #1\n\tfadd %" R "1, %" R "1, %" R "2\n\tfcmp %" R "1, %" R "3\n\tb." #CC " 1b" \
                     : "+r"(n), "+w"(x) : "w"(step), "w"(lim) : "cc"); \
    return n; }
#define DOWNLOOP(W, T, R, CC) \
static uint64_t down_##W##_##CC(T x, T step) { \
    uint64_t n = 0; \
    __asm__ volatile("1:\n\tadd %0, %0, #1\n\tfadd %" R "1, %" R "1, %" R "2\n\tfcmp %" R "1, #0.0\n\tb." #CC " 1b" \
                     : "+r"(n), "+w"(x) : "w"(step) : "cc"); \
    return n; }
#define UPLOOPS(W, T, R) UPLOOP(W, T, R, mi) UPLOOP(W, T, R, lt) UPLOOP(W, T, R, cc) UPLOOP(W, T, R, ls) UPLOOP(W, T, R, le)
#define DOWNLOOPS(W, T, R) DOWNLOOP(W, T, R, gt) DOWNLOOP(W, T, R, hi) DOWNLOOP(W, T, R, ge) DOWNLOOP(W, T, R, pl) DOWNLOOP(W, T, R, cs)
UPLOOPS(d, double, "d") UPLOOPS(s, float, "s")
DOWNLOOPS(d, double, "d") DOWNLOOPS(s, float, "s")

static void check_loops(void) {
    const uint64_t L = 1000;
    const double dstep = 1.0, dlim = 1000.0, dstart = 1000.0, dneg = -1.0;
    const float sstep = 1.0f, slim = 1000.0f, sstart = 1000.0f, sneg = -1.0f;
#define EXPECT(name, got, want) do { if ((got) != (want)) { fails++; printf("FAIL loop %s got=%llu want=%llu\n", name, \
        (unsigned long long)(got), (unsigned long long)(want)); } } while (0)
    EXPECT("up_d_mi", up_d_mi(0.0, dstep, dlim), L);     EXPECT("up_s_mi", up_s_mi(0.0f, sstep, slim), L);
    EXPECT("up_d_lt", up_d_lt(0.0, dstep, dlim), L);     EXPECT("up_s_lt", up_s_lt(0.0f, sstep, slim), L);
    EXPECT("up_d_cc", up_d_cc(0.0, dstep, dlim), L);     EXPECT("up_s_cc", up_s_cc(0.0f, sstep, slim), L);
    EXPECT("up_d_ls", up_d_ls(0.0, dstep, dlim), L + 1); EXPECT("up_s_ls", up_s_ls(0.0f, sstep, slim), L + 1);
    EXPECT("up_d_le", up_d_le(0.0, dstep, dlim), L + 1); EXPECT("up_s_le", up_s_le(0.0f, sstep, slim), L + 1);
    EXPECT("down_d_gt", down_d_gt(dstart, dneg), L);     EXPECT("down_s_gt", down_s_gt(sstart, sneg), L);
    EXPECT("down_d_hi", down_d_hi(dstart, dneg), L);     EXPECT("down_s_hi", down_s_hi(sstart, sneg), L);
    EXPECT("down_d_ge", down_d_ge(dstart, dneg), L + 1); EXPECT("down_s_ge", down_s_ge(sstart, sneg), L + 1);
    EXPECT("down_d_pl", down_d_pl(dstart, dneg), L + 1); EXPECT("down_s_pl", down_s_pl(sstart, sneg), L + 1);
    EXPECT("down_d_cs", down_d_cs(dstart, dneg), L + 1); EXPECT("down_s_cs", down_s_cs(sstart, sneg), L + 1);
#undef EXPECT
}

int main(void) {
    CONDS(RUN_DOUBLE)
    CONDS(RUN_SINGLE)
    check_loops();
    printf("fcmp_branch: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
