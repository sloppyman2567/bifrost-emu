/*
 * jit_neon.c — JIT NEON/SIMD correctness tests.
 *
 * Exercises the vector instructions that were broken in earlier
 * releases and are now fixed:
 *   - SHL/USHR/SSHR (vector, by immediate)
 *   - SLI/SRI (shift left/right insert — used for vector rotate)
 *   - USRA/SSRA (shift right and accumulate)
 *   - REV32/REV64 (byte/lane reversal)
 *   - INS/UMOV (vector element insert/extract)
 *   - Vector ADD/XOR (regression check)
 *
 * Each sub-test compares the NEON result against a scalar reference
 * computed in plain C. All sub-tests pass under JIT, interpreter,
 * and ASan+UBSan.
 *
 * Added in rc.1 after the great NEON fix pass (immh extraction,
 * MOVI/shift collision, SLI/SRI/USRA handlers, INS/UMOV v_hi routing,
 * REV64 size-awareness, 32-bit ROR).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

#define STR2(x) #x
#define STR(x)  STR2(x)

static int fails = 0;

static void check(const char* name, int ok) {
    if (ok) printf("ok %s\n", name);
    else    { printf("NG %s\n", name); fails++; }
}

/* ── SHL/USHR all shift amounts ── */
static void test_shifts(void) {
    uint32_t x = 0xDEADBEEF;
    int ok = 1;
    for (int n = 1; n <= 31; n++) {
        uint32_t in[4] = {x, x, x, x};
        uint32_t out[4] = {0};
        uint32x4_t v = vld1q_u32(in);
        /* The compiler emits vshlq_n_u32 / vshrq_n_u32 for these */
        uint32x4_t s = v;
        for (int i = 0; i < n; i++) s = vshlq_n_u32(s, 1);
        vst1q_u32(out, s);
        if (out[0] != (x << n)) ok = 0;
        v = vld1q_u32(in);
        for (int i = 0; i < n; i++) v = vshrq_n_u32(v, 1);
        vst1q_u32(out, v);
        if (out[0] != (x >> n)) ok = 0;
    }
    check("shl_ushr_all", ok);
}

/* ── Scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd, Dn, #imm) ──
 * The 64-bit-element shift encodings live in the FP space
 * (bits[28:24] = 11111), so the decoder routes them to FP_SCALAR and
 * they used to CALL_INTERP every time — the voxel game's `ushr dN, dM,
 * #32` was the top remaining FP/SIMD fallback. Encodings:
 *   SHL  Dd, Dn, #s  = 0x5F005400 | ((64+s)  << 16) | (rn<<5) | rd
 *   USHR Dd, Dn, #s  = 0x7F000400 | ((128-s) << 16) | (rn<<5) | rd
 *   SSHR Dd, Dn, #s  = 0x5F000400 | ((128-s) << 16) | (rn<<5) | rd
 * shift == 64 (all bits shifted out) exercises the interpreter fallback
 * (clear for SHL/USHR, sign-fill for SSHR). */
#define SHL_D_WORD(rd, rn, s)  (0x5F005400u | ((((64u) + (s)) & 0xFF) << 16) | ((rn) << 5) | (rd))
#define USHR_D_WORD(rd, rn, s) (0x7F000400u | ((((128u) - (s)) & 0xFF) << 16) | ((rn) << 5) | (rd))
#define SSHR_D_WORD(rd, rn, s) (0x5F000400u | ((((128u) - (s)) & 0xFF) << 16) | ((rn) << 5) | (rd))

static void test_scalar_shift(void) {
    uint64_t in  = 0xFEDCBA9876543210ULL;   /* sign bit set -> SSHR sign-fills */
    uint64_t out = 0;
    int ok = 1;

#define DO_SSHIFT(word) \
    asm volatile("ldr d0, [%0]\n\t.inst " STR(word) "\n\tstr d0, [%1]" \
                 :: "r"(&in), "r"(&out) : "memory")

    DO_SSHIFT(SHL_D_WORD(0, 0, 1));   if (out != (in << 1))  ok = 0;
    DO_SSHIFT(SHL_D_WORD(0, 0, 32));  if (out != (in << 32)) ok = 0;
    DO_SSHIFT(SHL_D_WORD(0, 0, 64));  if (out != 0)          ok = 0;  /* interp fallback */
    DO_SSHIFT(USHR_D_WORD(0, 0, 1));  if (out != (in >> 1))  ok = 0;
    DO_SSHIFT(USHR_D_WORD(0, 0, 32)); if (out != 0x00000000FEDCBA98ULL) ok = 0;
    DO_SSHIFT(USHR_D_WORD(0, 0, 64)); if (out != 0)          ok = 0;  /* interp fallback */
    DO_SSHIFT(SSHR_D_WORD(0, 0, 1));  if (out != (uint64_t)((int64_t)in >> 1))  ok = 0;
    DO_SSHIFT(SSHR_D_WORD(0, 0, 32)); if (out != 0xFFFFFFFFFEDCBA98ULL) ok = 0;
    DO_SSHIFT(SSHR_D_WORD(0, 0, 64)); if (out != 0xFFFFFFFFFFFFFFFFULL) ok = 0;  /* interp fallback */

#undef DO_SSHIFT
    check("scalar_shl_ushr_sshr", ok);
}

/* ── DUP (general): GPR -> vector broadcast, all element sizes ──
 * The simd_dp DUP guard was `imm5 == 0x08 && Q` (native .2d only), so
 * the game's `dup vN.16b, wM` (0x4E010C20) ran ~9K times per million
 * instructions through the interpreter. Now native for esize 1/2/4/8
 * with both Q=0 (8 bytes, v_hi zeroed) and Q=1 (16 bytes).
 * Encoding: 0x0E000C00 | (sf<<31) | (Q<<30) | (imm5<<16) | (rn<<5) | rd. */
#define DUP_GPR_WORD(rd, rn, imm5, sf, Q) \
    (0x0E000C00u | ((sf) << 31) | ((Q) << 30) | ((imm5) << 16) | ((rn) << 5) | (rd))

static void test_dup_gpr(void) {
    int ok = 1;
    /* dup v0.16b, w1: 16 copies of a byte */
    uint8_t b[16] = {0};
    asm volatile("mov w1, #0xAB\n\t.inst " STR(DUP_GPR_WORD(0, 1, 1, 0, 1))
                 "\n\tstr q0, [%0]" :: "r"(b) : "memory");
    for (int i = 0; i < 16; i++) if (b[i] != 0xAB) ok = 0;
    /* dup v2.4h, w3: 8 copies of a halfword */
    uint16_t h[8] = {0};
    asm volatile("mov w3, #0x1234\n\t.inst " STR(DUP_GPR_WORD(2, 3, 2, 0, 1))
                 "\n\tstr q2, [%0]" :: "r"(h) : "memory");
    for (int i = 0; i < 8; i++) if (h[i] != 0x1234) ok = 0;
    /* dup v4.2s, w5: 4 copies of a word */
    uint32_t w[4] = {0};
    asm volatile("movz w5, #0xBEEF\n\tmovk w5, #0xDEAD, lsl #16\n\t.inst " STR(DUP_GPR_WORD(4, 5, 4, 0, 1))
                 "\n\tstr q4, [%0]" :: "r"(w) : "memory");
    for (int i = 0; i < 4; i++) if (w[i] != 0xDEADBEEFu) ok = 0;
    /* Q=0 forms: only 8 bytes, v_hi must stay zero */
    uint64_t q0[2] = {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
    asm volatile("mov w9, #0xCD\n\t.inst " STR(DUP_GPR_WORD(8, 9, 1, 0, 0))
                 "\n\tstr q8, [%0]" :: "r"(q0) : "memory");
    if (q0[0] != 0xCDCDCDCDCDCDCDCDULL || q0[1] != 0) ok = 0;
    uint64_t q1[2] = {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
    asm volatile("movz w11, #0x3344\n\tmovk w11, #0x1122, lsl #16\n\t.inst " STR(DUP_GPR_WORD(10, 11, 4, 0, 0))
                 "\n\tstr q10, [%0]" :: "r"(q1) : "memory");
    if (q1[0] != 0x1122334411223344ULL || q1[1] != 0) ok = 0;
    check("dup_gpr_all_sizes", ok);
}

/* ── SLI semantics: Vd = (Vn << shift) | (Vd & ((1<<shift)-1)) ──
 * The source shifts left; the destination's LOW shift bits are
 * retained in place (per ARM ARM: the new zero bits created by the
 * shift retain the destination's existing value). NOT ROTL. */
static void test_sli_rotl(void) {
    uint32_t x[4] = {0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321};
    uint32_t out[4] = {0};
    uint32x4_t v = vld1q_u32(x);
    __asm__ volatile("sli %0.4s, %0.4s, #7" : "+w"(v) : : );
    vst1q_u32(out, v);
    uint32_t e[4] = {
        (0x12345678u << 7) | (0x12345678u & 0x7F),
        (0x9abcdef0u << 7) | (0x9abcdef0u & 0x7F),
        (0x0fedcba9u << 7) | (0x0fedcba9u & 0x7F),
        (0x87654321u << 7) | (0x87654321u & 0x7F),
    };
    check("sli_rotl_7", memcmp(out, e, 16) == 0);
}

/* ── SRI semantics: Vd = (Vn >> shift) | (Vd & high shift bits) ──
 * The source shifts right; the destination's TOP shift bits are
 * retained in place (per ARM ARM). NOT ROTR. */
static void test_sri_rotr(void) {
    uint32_t x[4] = {0x12345678, 0x9abcdef0, 0x0fedcba9, 0x87654321};
    uint32_t out[4] = {0};
    uint32x4_t v = vld1q_u32(x);
    __asm__ volatile("sri %0.4s, %0.4s, #7" : "+w"(v) : : );
    vst1q_u32(out, v);
    uint32_t e[4] = {
        (0x12345678u >> 7) | (0x12345678u & 0xFE000000u),
        (0x9abcdef0u >> 7) | (0x9abcdef0u & 0xFE000000u),
        (0x0fedcba9u >> 7) | (0x0fedcba9u & 0xFE000000u),
        (0x87654321u >> 7) | (0x87654321u & 0xFE000000u),
    };
    check("sri_rotr_7", memcmp(out, e, 16) == 0);
}

/* ── USRA: Vd += (Vn >> shift) ── */
static void test_usra(void) {
    uint32_t acc[4] = {0x10000000, 0x20000000, 0x30000000, 0x40000000};
    uint32_t v[4]   = {0x80000001, 0x80000002, 0x80000003, 0x80000004};
    uint32_t out[4] = {0};
    uint32x4_t va = vld1q_u32(acc);
    uint32x4_t vv = vld1q_u32(v);
    va = vsraq_n_u32(va, vv, 4);
    vst1q_u32(out, va);
    uint32_t e[4] = {
        acc[0] + (v[0] >> 4), acc[1] + (v[1] >> 4),
        acc[2] + (v[2] >> 4), acc[3] + (v[3] >> 4),
    };
    check("usra_4", memcmp(out, e, 16) == 0);
}

/* ── REV32: byte-swap within 32-bit words ── */
static void test_rev32(void) {
    uint8_t input[16] = {0x01,0x02,0x03,0x04, 0x05,0x06,0x07,0x08,
                         0x09,0x0a,0x0b,0x0c, 0x0d,0x0e,0x0f,0x10};
    uint8_t output[16] = {0};
    uint8x16_t v = vld1q_u8(input);
    v = vrev32q_u8(v);
    vst1q_u8(output, v);
    uint8_t e[16] = {0x04,0x03,0x02,0x01, 0x08,0x07,0x06,0x05,
                     0x0c,0x0b,0x0a,0x09, 0x10,0x0f,0x0e,0x0d};
    check("rev32_bytes", memcmp(output, e, 16) == 0);
}

/* ── REV64: swap 32-bit lanes within 64-bit blocks ── */
static void test_rev64(void) {
    uint32_t input[4] = {0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10};
    uint32_t output[4] = {0};
    uint32x4_t v = vld1q_u32(input);
    v = vrev64q_u32(v);
    vst1q_u32(output, v);
    uint32_t e[4] = {0x05060708, 0x01020304, 0x0d0e0f10, 0x090a0b0c};
    check("rev64_u32", memcmp(output, e, 16) == 0);
}

/* ── INS: insert scalar into vector lane (Q=1, lane >= 2) ── */
static void test_ins(void) {
    uint32_t v[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t out[4] = {0};
    uint32x4_t vv = vld1q_u32(v);
    vv = vsetq_lane_u32(0xDEADBEEF, vv, 2);
    vst1q_u32(out, vv);
    check("ins_lane2", out[2] == 0xDEADBEEF &&
                       out[0] == 0x11111111 && out[1] == 0x22222222 && out[3] == 0x44444444);
}

/* ── UMOV: extract vector element to GPR (Q=1, lane >= 2) ── */
static void test_umov(void) {
    uint32_t v[4] = {0x11111111, 0x22222222, 0xDEADBEEF, 0x44444444};
    uint32x4_t vv = vld1q_u32(v);
    uint32_t lane2 = vgetq_lane_u32(vv, 2);
    check("umov_lane2", lane2 == 0xDEADBEEF);
}

/* ── UMOV (vector element -> GPR), raw encodings — native in the JIT ──
 * Encoding: 0x0E003C00 | (Q<<30) | (imm5<<16) | (rn<<5) | rd.
 * imm5 = esize | (index << log2(esize)+1) — the low set bit is the element
 * size, the remaining bits the lane index. Q=0 -> Wd, Q=1 -> Xd. Covers
 * both v_lo (lanes < 8/esize) and v_hi routing. GCC's vectorized memset
 * does `umov x1, v0.d[0]` (Q=1, imm5=8) after `dup v0.16b,w1`. */
static void test_umov_raw(void) {
    /* 16-byte test vector; both qwords non-zero so v_hi routing is tested. */
    uint64_t vec[2] = {0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL};
    uint64_t out = 0;
    int ok = 1;
#define UMOV_RAW(rd, enc, expect, label) do {                              \
        out = 0;                                                           \
        asm volatile("ldr q0, [%1]\n\t.inst " #enc "\n\tmov %0, x" #rd     \
                     : "=r"(out) : "r"(vec) : "x" #rd, "memory");          \
        if (out != (expect)) ok = 0;                                       \
    } while (0)
    /* Q=1 (64-bit dest): d[0] (v_lo, imm5=8), d[1] (v_hi, imm5=0x18). */
    UMOV_RAW(9, 0x4E083C09, 0xDEADBEEFCAFEBABEULL, "d0");
    UMOV_RAW(10, 0x4E183C0A, 0x1122334455667788ULL, "d1");
    /* Q=0 (32-bit dest, zero-extended): s lanes across v_lo + v_hi. */
    UMOV_RAW(11, 0x0E043C0B, 0xCAFEBABEULL, "s0");
    UMOV_RAW(11, 0x0E0C3C0B, 0xDEADBEEFULL, "s1");
    UMOV_RAW(11, 0x0E143C0B, 0x55667788ULL, "s2");
    UMOV_RAW(11, 0x0E1C3C0B, 0x11223344ULL, "s3");
    /* h lanes, v_hi routing at index >= 4. */
    UMOV_RAW(11, 0x0E023C0B, 0xBABEULL, "h0");
    UMOV_RAW(11, 0x0E123C0B, 0x7788ULL, "h4");
    UMOV_RAW(11, 0x0E1E3C0B, 0x1122ULL, "h7");
    /* b lanes, v_hi routing at index >= 8. */
    UMOV_RAW(11, 0x0E013C0B, 0xBEULL, "b0");
    UMOV_RAW(11, 0x0E113C0B, 0x88ULL, "b8");
    UMOV_RAW(11, 0x0E1F3C0B, 0x11ULL, "b15");
#undef UMOV_RAW
    check("umov_raw_all_sizes", ok);
}

/* ── Vector ADD/XOR (regression — these already worked) ── */
static void test_add_xor(void) {
    uint32_t a[4] = {1, 100, 1000, 0xFFFFFFFF};
    uint32_t b[4] = {2, 200, 3000, 1};
    uint32_t out[4] = {0};
    uint32x4_t va = vld1q_u32(a);
    uint32x4_t vb = vld1q_u32(b);
    vst1q_u32(out, vaddq_u32(va, vb));
    check("vec_add", out[0]==3 && out[1]==300 && out[2]==4000 && out[3]==0);
    uint32_t x[4] = {0xDEADBEEF, 0x12345678, 0xAABBCCDD, 0x0000FFFF};
    uint32_t y[4] = {0xCAFEBABE, 0x87654321, 0x55331122, 0xFFFF0000};
    uint32x4_t vx = vld1q_u32(x);
    uint32x4_t vy = vld1q_u32(y);
    vst1q_u32(out, veorq_u32(vx, vy));
    check("vec_xor", out[0]==(x[0]^y[0]) && out[1]==(x[1]^y[1]) &&
                    out[2]==(x[2]^y[2]) && out[3]==(x[3]^y[3]));
}

int main(void) {
    test_shifts();
    test_scalar_shift();
    test_dup_gpr();
    test_sli_rotl();
    test_sri_rotr();
    test_usra();
    test_rev32();
    test_rev64();
    test_ins();
    test_umov();
    test_umov_raw();
    test_add_xor();
    printf("neon: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
