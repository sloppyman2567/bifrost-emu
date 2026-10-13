// SSE/VEX encoding helpers shared by the SIMD codegen families.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
// Scratch vectors are assembled completely before a tentative FP result
// is committed. A NaN exit must see the original instruction's inputs.
void FrostJIT::load_fp_vector(int xmm, int v, bool q) {
    emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
    emit_modrm_disp(xmm, CPU_REG, V_LO_OFF + v * 8);
    if (q) {
        emit_byte(0x0F); emit_byte(0x16);
        emit_modrm_disp(xmm, CPU_REG, V_HI_OFF + v * 8);
    }
}
void FrostJIT::store_fp_vector(int v, bool q) {
    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
    emit_modrm_disp(0, CPU_REG, V_LO_OFF + v * 8);
    if (q) {
        emit_byte(0x0F); emit_byte(0x17);
        emit_modrm_disp(0, CPU_REG, V_HI_OFF + v * 8);
    } else {
        emit_push(RAX); emit_mov_imm32_zext(RAX, 0);
        emit_store(CPU_REG, V_HI_OFF + v * 8, RAX); emit_pop(RAX);
    }
}
// ── Local emit helpers for the memory-path SIMD ops ──
// The ops below (SIMD_2REG/CVTF/ADDP/XTN/TBL/INS) are NOT in
// vec_cache_compatible_op, so the vec cache is never active in their
// blocks and every XMM reg is free scratch. All emit helpers are
// REX.R/REX.B-aware so XMM0-15 can be used freely.
void FrostJIT::emit_xmm_rex(int dst, int src) {
    uint8_t rex = 0x40;
    if (dst >= 8) rex |= 0x04;   // REX.R
    if (src >= 8) rex |= 0x01;   // REX.B
    if (rex != 0x40) emit_byte(rex);
}
void FrostJIT::modrm3(int dst, int src) {
    emit_byte(0xC0 | ((dst & 7) << 3) | (src & 7));
}
// SSE2 two-operand integer op: 66 [REX] 0F op reg,reg
void FrostJIT::sse2_op(int op, int dst, int src) {
    emit_byte(0x66); emit_xmm_rex(dst, src); emit_byte(0x0F);
    emit_byte(static_cast<uint8_t>(op)); modrm3(dst, src);
}
// SSSE3/SSE4.1 two-operand integer op in the 0F 38 map.
void FrostJIT::sse2_38(int op, int dst, int src) {
    emit_byte(0x66); emit_xmm_rex(dst, src); emit_byte(0x0F); emit_byte(0x38);
    emit_byte(static_cast<uint8_t>(op)); modrm3(dst, src);
}
// Scalar SSE FP op (0F map, no 66): 0F [REX] op reg,reg [ib]
void FrostJIT::sse_fp(int op, int dst, int src) {
    emit_xmm_rex(dst, src); emit_byte(0x0F);
    emit_byte(static_cast<uint8_t>(op)); modrm3(dst, src);
}
// Truncating SSE FP→int (F3 prefix): F3 [REX] 0F op reg,reg
// (cvttps2dq/cvttpd2dq — 66 0F 5B cvtps2dq ROUNDS; F3 0F 5B truncates)
void FrostJIT::sse2_f3(int op, int dst, int src) {
    emit_byte(0xF3); emit_xmm_rex(dst, src); emit_byte(0x0F);
    emit_byte(static_cast<uint8_t>(op)); modrm3(dst, src);
}
// Word shuffles share opcode 0x70 with pshufd but need legacy
// prefixes: F2 0F 70 = pshuflw (low 64 bits), F3 0F 70 = pshufhw
// (high 64 bits). sse2_op emits 66 0F 70 = pshufd — using it for a
// word shuffle silently shuffles DWORDS instead (neverball IDCT
// miscompile: every .h MUL_ELEM lane picked the wrong coefficient).
void FrostJIT::sse_f2(int op, int dst, int src) {
    emit_byte(0xF2); emit_xmm_rex(dst, src); emit_byte(0x0F);
    emit_byte(static_cast<uint8_t>(op)); modrm3(dst, src);
}
// Imm-shift in the 0F 71/72/73 group: 66 [REX] 0F op /digit ib.
// kind: SHL=4, SHR=5, SAR=7 (see emit_shift_imm8).
void FrostJIT::sse2_imm(int op, int dst, int digit, uint8_t cnt) {
    emit_byte(0x66); emit_xmm_rex(0, dst); emit_byte(0x0F);
    emit_byte(static_cast<uint8_t>(op)); modrm3(digit, dst);
    emit_byte(cnt);
}
// movdqa dst, src (register form).
void FrostJIT::movdqa(int dst, int src) { sse2_op(0x6F, dst, src); }
// 64-bit loads/stores to the guest vreg memory space (movsd/movhpd).
void FrostJIT::load_lo(int xmm, int off) {
    emit_byte(0xF2); emit_xmm_rex(xmm, 0); emit_byte(0x0F); emit_byte(0x10);
    emit_modrm_disp(xmm & 7, CPU_REG, off);
}
void FrostJIT::load_hi(int xmm, int off) {
    emit_byte(0x66); emit_xmm_rex(xmm, 0); emit_byte(0x0F); emit_byte(0x16);
    emit_modrm_disp(xmm & 7, CPU_REG, off);
}
void FrostJIT::store_lo(int xmm, int off) {
    emit_byte(0xF2); emit_xmm_rex(xmm, 0); emit_byte(0x0F); emit_byte(0x11);
    emit_modrm_disp(xmm & 7, CPU_REG, off);
}
void FrostJIT::store_hi(int xmm, int off) {
    emit_byte(0x66); emit_xmm_rex(xmm, 0); emit_byte(0x0F); emit_byte(0x17);
    emit_modrm_disp(xmm & 7, CPU_REG, off);
}
// Load a full 16-byte vector (v_lo + v_hi) into an XMM reg.
void FrostJIT::load_vec(int xmm, int vreg) {
    load_lo(xmm, V_LO_OFF + vreg * 8);
    load_hi(xmm, V_HI_OFF + vreg * 8);
}
// Store the low 8 bytes of an XMM reg to a vector, zeroing v_hi for Q=0.
void FrostJIT::store_vec(int xmm, int vreg, bool q) {
    store_lo(xmm, V_LO_OFF + vreg * 8);
    if (q) store_hi(xmm, V_HI_OFF + vreg * 8);
    else   fp_zero_hi(vreg);
}
// Broadcast a 64-bit constant into both halves of an XMM register:
// movabs RAX, c; vmovq xmm, rax; punpcklqdq xmm, xmm
void FrostJIT::emit_mask(int xmm, uint64_t c) {
    clobber_flags();
    flush_invalidate_host_regs(1u << RAX);
    clobber_host_reg(RAX);
    emit_mov_imm64(RAX, c);
    emit_vmovq_gpr_to_xmm(xmm, RAX);
    sse2_op(0x6C, xmm, xmm);  // punpcklqdq
}
// Load an arbitrary 16-byte constant (two 64-bit halves) into an XMM
// reg. XMM5 is a dedicated scratch (the memory-path SIMD ops below are
// never vec-cache pinned, so every XMM reg is free).
void FrostJIT::emit_mask16(int xmm, const uint8_t m[16]) {
    uint64_t lo = 0, hi = 0;
    memcpy(&lo, m, 8);
    memcpy(&hi, m + 8, 8);
    clobber_flags();
    flush_invalidate_host_regs(1u << RAX);
    clobber_host_reg(RAX);
    emit_mov_imm64(RAX, lo);
    emit_vmovq_gpr_to_xmm(xmm, RAX);
    emit_mov_imm64(RAX, hi);
    emit_vmovq_gpr_to_xmm(5, RAX);
    sse2_op(0x6C, xmm, 5);  // punpcklqdq xmm, xmm5
}
// Saturating integer ops: set FPSR.QC unless every active byte of xa equals
// xb (the result had the caller's non-saturated value). Q=0 tests the low
// 8 bytes. Clobbers xa, xb and host flags (callers have committed lazy NZCV);
// live GPRs are preserved.
void FrostJIT::emit_simd_qc_unless_equal(int xa, int xb, bool q) {
    sse2_op(0x74, xa, xb);                      // pcmpeqb xa, xb
    size_t skip;
    if (has_sse41()) {
        sse2_op(0x74, xb, xb);                  // pcmpeqb xb, xb: all ones
        if (!q) {                               // movq xb, xb: low half only
            emit_byte(0xF3); emit_xmm_rex(xb, xb); emit_byte(0x0F); emit_byte(0x7E);
            modrm3(xb, xb);
        }
        sse2_38(0x17, xa, xb);                  // ptest xa, xb: CF = all equal
        skip = emit_jcc_rel8_placeholder(0x2);  // JC
    } else {
        emit_push(RAX);
        sse2_op(0xD7, RAX, xa);                 // pmovmskb eax, xa
        if (q) { emit_byte(0x3D); emit_u32(0xFFFF); }  // cmp eax, 0xFFFF
        else { emit_byte(0x3C); emit_byte(0xFF); }     // cmp al, 0xFF
        emit_pop(RAX);                          // POP leaves the flags intact
        skip = emit_jcc_rel8_placeholder(0x4);  // JE
    }
    emit_byte(0x81); emit_modrm_disp(1, CPU_REG, FPSR_OFF);  // or dword [cpu+FPSR], QC
    emit_u32(1u << 27);
    patch_jcc_rel8(skip, static_cast<int8_t>(code_buf_used_ - (skip + 2)));
}
}  // namespace arm64emu
