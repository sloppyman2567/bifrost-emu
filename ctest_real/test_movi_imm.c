// test_movi_imm.c — AdvSIMD modified-immediate (MOVI/MVNI/ORR/BIC + MSL).
//
// The 0x0F0004xx encodings share the SHIFT table's SSHR mask, so they used
// to be swallowed as "esize=1 shift" and fell back to CALL_INTERP. Now they
// are native JIT code (Family::MODIMM from tools/opgen/simd_dp.txt). This
// test pins the exact encodings with .inst and checks the full 128-bit
// result, mirroring the interpreter block in src/interp/interp_fp.cpp.
//
// No libm, static-friendly so `make setup-tests` can build it.

#include <stdio.h>
#include <stdint.h>

// Word layout: 0x0F000400 | Q<<30 | op<<29 | (imm8>>5)<<16 | (imm8&0x1f)<<5
//               | cmode<<12 | Rd   (abc = bits[18:16], defgh = bits[9:5]).
#define MODIMM_WORD(rd, q, op, imm8, cmode) \
    (0x0F000400u | ((q) << 30) | ((op) << 29) | \
     (((imm8) >> 5) << 16) | (((imm8) & 0x1f) << 5) | ((cmode) << 12) | (rd))

#define STR2(x) #x
#define STR(x)  STR2(x)

#define DO_MODIMM(rd, q, op, imm8, cmode) \
    asm volatile(".inst " STR(MODIMM_WORD(rd, q, op, imm8, cmode)))

static int failures;

static void expect_v(const char* name, const uint8_t got[16],
                     uint64_t want_lo, uint64_t want_hi) {
    uint64_t lo, hi;
    __builtin_memcpy(&lo, got, 8);
    __builtin_memcpy(&hi, got + 8, 8);
    if (lo != want_lo || hi != want_hi) {
        printf("FAIL %s: got %016llx:%016llx want %016llx:%016llx\n", name,
               (unsigned long long)lo, (unsigned long long)hi,
               (unsigned long long)want_lo, (unsigned long long)want_hi);
        failures++;
    }
}

int main(void) {
    uint8_t buf[16];

    // --- MOVI 32-bit LSL (Q=0, vN.2s): value replicated twice, v_hi=0 ---
    DO_MODIMM(0, 0, 0, 0x3d, 6);  // movi v0.2s, #0x3d, lsl #24 (0x3d000000)
    asm volatile("str q0, [%0]" :: "r"(buf) : "memory");
    expect_v("movi2s_lsl24", buf, 0x3d0000003d000000ULL, 0);

    DO_MODIMM(1, 0, 0, 0x42, 4);  // movi v1.2s, #0x42, lsl #16 (0x00420000)
    asm volatile("str q1, [%0]" :: "r"(buf) : "memory");
    expect_v("movi2s_lsl16", buf, 0x0042000000420000ULL, 0);

    DO_MODIMM(2, 0, 0, 0x00, 0);  // movi v2.2s, #0x0 (the game's 0x0F000409)
    asm volatile("str q2, [%0]" :: "r"(buf) : "memory");
    expect_v("movi2s_zero", buf, 0, 0);

    DO_MODIMM(3, 0, 0, 0x7f, 2);  // movi v3.2s, #0x7f, lsl #8 (0x00007f00)
    asm volatile("str q3, [%0]" :: "r"(buf) : "memory");
    expect_v("movi2s_lsl8", buf, 0x00007f0000007f00ULL, 0);

    // --- MOVI 32-bit LSL (Q=1, vN.4s): value replicated into v_hi too ---
    DO_MODIMM(4, 1, 0, 0x3d, 6);
    asm volatile("str q4, [%0]" :: "r"(buf) : "memory");
    expect_v("movi4s_lsl24", buf, 0x3d0000003d000000ULL, 0x3d0000003d000000ULL);

    // --- MVNI (op=1): ~value ---
    DO_MODIMM(5, 0, 1, 0x3d, 6);  // mvni v5.2s, #0x3d, lsl #24 → ~0x3d000000
    asm volatile("str q5, [%0]" :: "r"(buf) : "memory");
    expect_v("mvni2s", buf, ~0x3d0000003d000000ULL, 0);

    // --- MOVI 16-bit LSL (cmode 8/9) ---
    DO_MODIMM(6, 0, 0, 0xab, 8);  // movi v6.4h, #0xab (imm16 = 0x00ab)
    asm volatile("str q6, [%0]" :: "r"(buf) : "memory");
    expect_v("movi4h", buf, 0x00ab00ab00ab00abULL, 0);

    DO_MODIMM(7, 1, 0, 0xab, 10);  // movi v7.8h, #0xab, lsl #8 (imm16 = 0xab00)
    asm volatile("str q7, [%0]" :: "r"(buf) : "memory");
    expect_v("movi8h_lsl8", buf, 0xab00ab00ab00ab00ULL, 0xab00ab00ab00ab00ULL);

    // --- MOVI 8-bit (cmode=14, op=0) ---
    DO_MODIMM(8, 0, 0, 0x7f, 14);  // movi v8.8b, #0x7f
    asm volatile("str q8, [%0]" :: "r"(buf) : "memory");
    expect_v("movi8b", buf, 0x7f7f7f7f7f7f7f7fULL, 0);

    // --- MOVI 64-bit (cmode=14, op=1): each imm8 bit -> 0x00/0xFF byte ---
    DO_MODIMM(9, 0, 1, 0x07, 14);  // imm8 bits 0,1,2 set -> 3 trailing 0xFF bytes
    asm volatile("str q9, [%0]" :: "r"(buf) : "memory");
    expect_v("movi1d_bits", buf, 0x0000000000ffffffULL, 0);

    // --- MOVI 32-bit MSL (cmode 12/13): imm8 << shift | ones ---
    DO_MODIMM(10, 0, 0, 0x12, 12);  // movi v10.2s, #0x12, msl #8
                                    // lane = (0x12<<8)|0xFF = 0x000012FF (32-bit)
    asm volatile("str q10, [%0]" :: "r"(buf) : "memory");
    expect_v("movi2s_msl8", buf, 0x000012ff000012ffULL, 0);

    DO_MODIMM(11, 1, 0, 0x12, 13);  // movi v11.4s, #0x12, msl #16
                                    // lane = (0x12<<16)|0xFFFF = 0x0012FFFF (32-bit)
    asm volatile("str q11, [%0]" :: "r"(buf) : "memory");
    expect_v("movi4s_msl16", buf, 0x0012ffff0012ffffULL, 0x0012ffff0012ffffULL);

    // --- ORR immediate (cmode 1,3,5,7): Vd = Vd | imm (reads Vd!) ---
    DO_MODIMM(12, 0, 0, 0x3d, 7);  // orr v12.2s, #0x3d, lsl #24 → |0x3d000000
    asm volatile("movi v12.2s, #0x12\n\t"
                 "orr v12.2s, #0x3d, lsl #24\n\t"
                 "str q12, [%0]" :: "r"(buf) : "memory", "v12");
    expect_v("orr2s", buf, 0x3d0000123d000012ULL, 0);

    // --- BIC immediate: Vd = Vd & ~imm ---
    DO_MODIMM(13, 0, 1, 0x3d, 7);  // bic v13.2s, #0x3d, lsl #24 → &~0x3d000000
    asm volatile("movi v13.2s, #0x12, lsl #24\n\t"
                 "bic v13.2s, #0x3d, lsl #24\n\t"
                 "str q13, [%0]" :: "r"(buf) : "memory", "v13");
    expect_v("bic2s", buf, 0x0200000002000000ULL, 0);

    // --- ORR/BIC 16-bit (cmode 10/11), Q=1 ---
    DO_MODIMM(14, 1, 0, 0xab, 11);  // orr v14.8h, #0xab, lsl #8
    asm volatile("movi v14.8h, #0x12\n\t"
                 "orr v14.8h, #0xab, lsl #8\n\t"
                 "str q14, [%0]" :: "r"(buf) : "memory", "v14");
    expect_v("orr8h", buf, 0xab12ab12ab12ab12ULL, 0xab12ab12ab12ab12ULL);

    DO_MODIMM(15, 1, 1, 0xab, 9);  // bic v15.8h, #0xab (imm16 = 0x00ab)
    asm volatile("movi v15.8h, #0xff\n\t"  // 16-bit form: lanes 0x00ff
                 "bic v15.8h, #0xab\n\t"   // 0x00ff & ~0x00ab = 0x0054
                 "str q15, [%0]" :: "r"(buf) : "memory", "v15");
    expect_v("bic8h", buf, 0x0054005400540054ULL, 0x0054005400540054ULL);

    if (failures) {
        printf("test_movi_imm: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_movi_imm: ALL PASS\n");
    return 0;
}
