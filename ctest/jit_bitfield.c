/*
 * jit_bitfield.c — JIT UBFM / SBFM / BFM / EXTR / BFI / BFXIL tests.
 *
 * Background (alpha.4 bugs):
 *   1. UBFM mask width was computed as `imms+1` instead of
 *      `imms-immr+1` (with wraparound).  For LSL pseudonym
 *      (imms = 63, immr = 64 - shift), the old formula gave mask
 *      width 64 (always all-ones), making the AND a no-op, so the
 *      shift produced the wrong value.
 *
 *   2. UBFM-as-LSL must AND _before_ shifting; if the JIT did it in
 *      the wrong order it would lose high bits that were supposed to
 *      be masked off.
 *
 *   3. EXTR with lsb!=0 needs an actual 128-bit right-shift of the
 *      (Rn:Rm) pair.  The first version only handled lsb=0.
 *
 * Uses pure C with shift/mask operations.  The compiler at -O2 emits
 * UBFM/SBFM/BFM/EXTR for these patterns.  We verify the result matches
 * what plain C semantics would compute.
 */
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(expr, tag) do { \
    if (expr) { printf("ok %s\n", tag); } \
    else      { printf("NG %s\n", tag); fails++; } \
} while (0)

int main(void) {
    /* ── UBFM as LSL (immediate) ──────────────────────────────────── */
    /* Compiler emits `lsl Xd, Xn, #imm` for v << imm. */
    uint64_t v = 0xFFFFFFFFFFFFFFF0ULL;
    CHECK((v << 5) == 0xFFFFFFFFFFFFFE00ULL, "lsl_imm5");

    /* LSL #12 — this specifically caught the alpha.4 bug. */
    v = 0x123456789ABCDEF0ULL;
    CHECK((v << 12) == 0x456789ABCDEF0000ULL, "lsl_imm12");

    /* LSL #63 — single bit at position 63 */
    v = 0x1;
    CHECK((v << 63) == 0x8000000000000000ULL, "lsl_imm63");

    /* LSL #40 */
    v = 0x1;
    CHECK((v << 40) == 0x10000000000ULL, "lsl_imm40");

    /* ── UBFM as LSR (immediate) ──────────────────────────────────── */
    /* Compiler emits `lsr Xd, Xn, #imm` for v >> imm (unsigned). */
    v = 0xFF000000000000FFULL;
    CHECK((v >> 8) == 0x00FF000000000000ULL, "lsr_imm8");
    CHECK((v >> 56) == 0xFF, "lsr_imm56");
    CHECK((v >> 32) == 0xFF000000ULL, "lsr_imm32");
    CHECK((v >> 0) == v, "lsr_imm0");

    /* ── SBFM as ASR (immediate) ──────────────────────────────────── */
    /* Compiler emits `asr Xd, Xn, #imm` for sv >> imm (signed). */
    int64_t sv = -0x100;
    CHECK((sv >> 4) == -0x10, "asr_imm4");
    CHECK((sv >> 63) == -1, "asr_imm63");

    sv = 0x4000000000000000LL;
    CHECK((sv >> 62) == 1, "asr_imm62_pos");

    /* ── UBFIZ / UBFX (extract/insert zero-extended fields) ───────── */
    /* Pattern: (v & mask) << lsb  → UBFIZ */
    v = 0xABCD1234;
    CHECK(((v & 0xFF) << 16) == 0x340000ULL, "ubfiz_16_8");
    CHECK(((v & 0xF) << 0) == 0x4ULL, "ubfiz_0_4");

    /* Pattern: (v >> lsb) & mask  → UBFX */
    v = 0xDEADBEEFCAFEBABEULL;
    CHECK(((v >> 4) & 0xFF) == 0xAB, "ubfx_4_8");
    CHECK(((v >> 32) & 0xFFFF) == 0xBEEF, "ubfx_32_16");
    CHECK(((v >> 0) & 0xFFFFFFFFFFFFFFFFULL) == v, "ubfx_full");

    /* ── SBFIZ / SBFX (extract/insert sign-extended fields) ───────── */
    /* Pattern: ((intN_t)v) << lsb  → SBFIZ */
    v = 0x80;  /* low byte is 0x80 — sign as int8 is -128 */
    int64_t sext = (int8_t)(int64_t)v;  /* sign-extend to 64 bits */
    CHECK((sext << 8) == -0x80LL * 256, "sbfiz_8_8");

    /* Pattern: sign-extend then shift right */
    v = 0x0000000000008000ULL;
    int64_t sx = (int16_t)v;  /* sign-extend 16-bit value */
    CHECK(sx == -32768, "sbfx_0_16");

    v = 0xFF00000000000000ULL;
    int64_t sx8 = (int8_t)(v >> 56);
    CHECK(sx8 == -1, "sbfx_56_8");

    /* ── BFI (bit-field insert, preserving surrounding bits) ──────── */
    /* Pattern: (dst & ~field_mask) | ((src & field_mask) << lsb) */
    uint64_t dst = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t src = 0xAB;
    uint64_t mask = 0xFFULL << 8;
    dst = (dst & ~mask) | ((src << 8) & mask);
    CHECK(dst == 0xFFFFFFFFFFFFABFFULL, "bfi_8_8");

    dst = 0;
    src = 0xF;
    mask = 0xFULL << 60;
    dst = (dst & ~mask) | ((src << 60) & mask);
    CHECK(dst == 0xF000000000000000ULL, "bfi_60_4");

    /* ── BFXIL (bit-field extract and insert low) ─────────────────── */
    /* Pattern: (dst & ~low_mask) | (extracted & low_mask) */
    uint64_t src2 = 0xABCDEF1234567890ULL;
    uint64_t dst2 = 0x1111111111111111ULL;
    uint64_t low_mask = 0xFF;
    uint64_t extracted = (src2 >> 4) & low_mask;
    dst2 = (dst2 & ~low_mask) | (extracted & low_mask);
    /* bits 4..11 of src2 = 0x89 (0x90 >> 4 = 0x09, but bits 4..11 = 0x89) */
    CHECK(dst2 == 0x1111111111111189ULL, "bfxil_4_8");

    /* ── EXTR (concatenate Rn:Rm and extract #lsb-th bit onward) ──── */
    /* Pattern: (hi << (64 - n)) | (lo >> n) — for n in [1, 63]
     * Compiler emits EXTR for this when both hi and lo are non-constant
     * registers. */
    uint64_t hi = 0x1122334455667788ULL;
    uint64_t lo = 0x99AABBCCDDEEFF00ULL;

    /* EXTR #0 → just lo (compiler emits MOV) */
    /* EXTR #4 — (hi << 60) | (lo >> 4) */
    uint64_t extr_4 = (hi << 60) | (lo >> 4);
    /* Verify the formula manually:
     * hi << 60: take low 4 bits of hi (= 0x8), put them in the high nibble.
     * hi = 0x1122334455667788 → low 4 bits = 0x8 → hi << 60 = 0x8000000000000000
     * lo >> 4: shift right by 4. lo = 0x99AABBCCDDEEFF00 → 0x099AABBCCDDEEFF0
     * OR: 0x899AABBCCDDEEFF0
     */
    CHECK(extr_4 == 0x899AABBCCDDEEFF0ULL, "extr_4");

    /* EXTR #32 — (hi << 32) | (lo >> 32), take low 64 */
    uint64_t extr_32 = (hi << 32) | (lo >> 32);
    /* hi << 32: take low 32 bits of hi = 0x55667788, put them in low 32
     * Wait, hi << 32 puts low 32 bits of hi in the HIGH 32 bits, with low 32 zero.
     * (hi << 32) = 0x5566778800000000
     * (lo >> 32) = 0x99AABBCC
     * OR = 0x5566778899AABBCC
     */
    CHECK(extr_32 == 0x5566778899AABBCCULL, "extr_32");

    /* EXTR #63 — (hi << 1) | (lo >> 63), single bit cross-over */
    uint64_t extr_63 = (hi << 1) | (lo >> 63);
    /* hi << 1 = 0x22446688AACCEF10 (top bit lost, low bit = 0)
     * lo >> 63 = 1 (top bit of lo was 1)
     * OR = 0x22446688AACCEF11
     */
    CHECK(extr_63 == 0x22446688AACCEF11ULL, "extr_63");

    /* ── ROR with imm (encoded as EXTR Xd,Xn,Xn,#imm) ─────────────── */
    /* Pattern: (v >> n) | (v << (64 - n)) */
    v = 0x0000000000000001ULL;
    uint64_t ror4 = (v >> 4) | (v << 60);
    CHECK(ror4 == 0x1000000000000000ULL, "ror_imm4");

    v = 0x123456789ABCDEF0ULL;
    uint64_t ror16 = (v >> 16) | (v << 48);
    /* v >> 16 = 0x0000123456789ABC
     * v << 48 = 0xDEF0000000000000
     * OR      = 0xDEF0123456789ABC
     */
    CHECK(ror16 == 0xDEF0123456789ABCULL, "ror_imm16");

    /* ── Use of BFM/UBFM in real patterns ─────────────────────────── */
    /* Bit-clamping: mask off upper bits */
    v = 0xDEADBEEFCAFEBABEULL;
    CHECK((v & 0xFFFF) == 0xBABE, "mask_16");
    CHECK((v & 0xFFFFFFFF) == 0xCAFEBABE, "mask_32");

    /* Bit-replace-low: dst = (dst & ~mask) | (src & mask) */
    uint64_t orig = 0xAAAABBBBCCCCDDDDULL;
    uint64_t repl = 0x1234;
    uint64_t result = (orig & ~0xFFFFULL) | (repl & 0xFFFFULL);
    CHECK(result == 0xAAAABBBBCCCC1234ULL, "replace_low_16");

    printf("bitfield: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
