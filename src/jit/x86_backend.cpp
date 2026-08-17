// jit/x86_backend.cpp — low-level x86-64 instruction emitters.
//
// All emit_* primitives that produce raw x86 bytes into code_buf_.
// Includes:
//   - Byte/u32/u64 emission
//   - REX / ModR/M / SIB encoders
//   - mov (imm/reg), load/store (8/16/32/64 + sign-extend variants)
//   - ALU (add/sub/adc/sbb/and/or/xor/imul/test/cmp)
//   - Shifts (variable CL + immediate)
//   - not / neg / lzcnt / bswap
//   - Call (absolute + aligned), ret, nop, push, pop, pushfq, popfq
//   - Conditional/unconditional jumps (rel8 + rel32, with patch slots)
//   - Stack adjust (sub/add rsp, imm8) and CL mask (and cl, imm8)
//   - ARM64 register load/store (offsets into CPU struct)
//   - Flag materialization (x86 RFLAGS ↔ ARM NZCV in pstate)
//   - ARM→x86 condition code mapping
//   - Memory load/store via the direct window + slow-path C helper fallback
//
// These emitters have NO knowledge of the IR or the register allocator —
// they are pure code generators that take x86 register numbers and
// offsets. The regalloc and IR compiler layer on top.
#include "jit/frostjit.hpp"
#include "arm64_emu.hpp"  // Emulator complete type (for slow-path helpers)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace arm64emu {
void FrostJIT::emit_byte(uint8_t b) {
    if (code_buf_used_ + 1 <= CODE_BUF_SIZE)
        code_buf_[code_buf_used_++] = b;
    else
        code_buf_overflow_ = true;
}
void FrostJIT::emit_u32(uint32_t v) {
    if (code_buf_used_ + 4 <= CODE_BUF_SIZE)
        memcpy(code_buf_ + code_buf_used_, &v, 4), code_buf_used_ += 4;
    else
        code_buf_overflow_ = true;
}
void FrostJIT::emit_u64(uint64_t v) {
    if (code_buf_used_ + 8 <= CODE_BUF_SIZE)
        memcpy(code_buf_ + code_buf_used_, &v, 8), code_buf_used_ += 8;
    else
        code_buf_overflow_ = true;
}
// ── x86 helpers ────────────────────────────────────────────────────────
uint8_t FrostJIT::rex(bool w, bool r, bool x, bool b) {
    return 0x40 | (w?8:0) | (r?4:0) | (x?2:0) | (b?1:0);
}
uint8_t FrostJIT::modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (mod<<6) | ((reg&7)<<3) | (rm&7);
}
uint8_t FrostJIT::sib(uint8_t scale, uint8_t index, uint8_t base) {
    return (scale<<6) | ((index&7)<<3) | (base&7);
}
void FrostJIT::emit_mov_imm64(int dst, uint64_t imm) {
    emit_byte(rex(true,false,false,dst>=8));
    emit_byte(0xB8 + (dst&7));
    emit_u64(imm);
}
void FrostJIT::emit_mov_imm32(int dst, uint32_t imm) {
    emit_byte(rex(true,false,false,dst>=8));
    emit_byte(0xC7); emit_byte(modrm(3,0,dst&7)); emit_u32(imm);
}
void FrostJIT::emit_mov_imm32_zext(int dst, uint32_t imm) {
    if (dst >= 8) emit_byte(0x41);
    emit_byte(0xB8 + (dst&7));
    emit_u32(imm);
}
void FrostJIT::emit_mov_reg(int dst, int src) {
    if (dst==src) return;
    emit_byte(rex(true,src>=8,false,dst>=8));
    emit_byte(0x89); emit_byte(modrm(3,src&7,dst&7));
}
// Load: mov dst, [base+off] (64-bit)
void FrostJIT::emit_load(int dst, int base, int32_t off) {
    emit_byte(rex(true,dst>=8,false,base>=8));
    emit_byte(0x8B);
    if (base==4||base==12) {
        if (off==0) { emit_byte(modrm(0,dst&7,4)); emit_byte(sib(0,4,base&7)); }
        else if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,4)); emit_byte(sib(0,4,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,dst&7,4)); emit_byte(sib(0,4,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if ((base&7)==5) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,dst&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if (off==0) { emit_byte(modrm(0,dst&7,base&7)); }
    else if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
    else { emit_byte(modrm(2,dst&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
}
void FrostJIT::emit_store(int base, int32_t off, int src) {
    emit_byte(rex(true,src>=8,false,base>=8));
    emit_byte(0x89);
    if (base==4||base==12) {
        if (off==0) { emit_byte(modrm(0,src&7,4)); emit_byte(sib(0,4,base&7)); }
        else if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,4)); emit_byte(sib(0,4,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,src&7,4)); emit_byte(sib(0,4,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if ((base&7)==5) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,src&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if (off==0) { emit_byte(modrm(0,src&7,base&7)); }
    else if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
    else { emit_byte(modrm(2,src&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
}
void FrostJIT::emit_load32(int dst, int base, int32_t off) {
    emit_byte(rex(false,dst>=8,false,base>=8));
    emit_byte(0x8B);
    if (base==4||base==12) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,4)); emit_byte(sib(0,4,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,dst&7,4)); emit_byte(sib(0,4,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if ((base&7)==5) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,dst&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if (off==0) { emit_byte(modrm(0,dst&7,base&7)); }
    else if (off>=-128&&off<=127) { emit_byte(modrm(1,dst&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
    else { emit_byte(modrm(2,dst&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
}
void FrostJIT::emit_store32(int base, int32_t off, int src) {
    emit_byte(rex(false,src>=8,false,base>=8));
    emit_byte(0x89);
    if (base==4||base==12) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,4)); emit_byte(sib(0,4,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,src&7,4)); emit_byte(sib(0,4,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if ((base&7)==5) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,src&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if (off==0) { emit_byte(modrm(0,src&7,base&7)); }
    else if (off>=-128&&off<=127) { emit_byte(modrm(1,src&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
    else { emit_byte(modrm(2,src&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
}
void FrostJIT::emit_modrm_disp(int reg, int base, int32_t off) {
    if (base==4||base==12) {
        if (off==0) { emit_byte(modrm(0,reg&7,4)); emit_byte(sib(0,4,base&7)); }
        else if (off>=-128&&off<=127) { emit_byte(modrm(1,reg&7,4)); emit_byte(sib(0,4,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,reg&7,4)); emit_byte(sib(0,4,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if ((base&7)==5) {
        if (off>=-128&&off<=127) { emit_byte(modrm(1,reg&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
        else { emit_byte(modrm(2,reg&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
    } else if (off==0) { emit_byte(modrm(0,reg&7,base&7)); }
    else if (off>=-128&&off<=127) { emit_byte(modrm(1,reg&7,base&7)); emit_byte(static_cast<uint8_t>(off)); }
    else { emit_byte(modrm(2,reg&7,base&7)); emit_u32(static_cast<uint32_t>(off)); }
}
void FrostJIT::emit_load16(int dst, int base, int32_t off) {
    emit_byte(rex(false,dst>=8,false,base>=8));
    emit_byte(0x0F); emit_byte(0xB7);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load8(int dst, int base, int32_t off) {
    emit_byte(rex(false,dst>=8,false,base>=8));
    emit_byte(0x0F); emit_byte(0xB6);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load32_sx(int dst, int base, int32_t off) {
    emit_byte(rex(true,dst>=8,false,base>=8));
    emit_byte(0x63);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load16_sx(int dst, int base, int32_t off) {
    emit_byte(rex(false,dst>=8,false,base>=8));
    emit_byte(0x0F); emit_byte(0xBF);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load8_sx(int dst, int base, int32_t off) {
    emit_byte(rex(false,dst>=8,false,base>=8));
    emit_byte(0x0F); emit_byte(0xBE);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_store16(int base, int32_t off, int src) {
    emit_byte(0x66);
    emit_byte(rex(false,src>=8,false,base>=8));
    emit_byte(0x89);
    emit_modrm_disp(src, base, off);
}
void FrostJIT::emit_store8(int base, int32_t off, int src) {
    emit_byte(rex(false,src>=8,false,base>=8));
    emit_byte(0x88);
    emit_modrm_disp(src, base, off);
}
void FrostJIT::emit_add_reg(int dst, int src) {
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x01); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_add_reg_imm(int dst, int32_t imm) {
    // add r64, imm8/imm32: REX.W 83 /0 ib  |  REX.W 81 /0 id
    if (imm >= -128 && imm <= 127) {
        emit_byte(rex(true,false,false,dst>=8));
        emit_byte(0x83); emit_byte(modrm(3,0,dst&7)); emit_byte(static_cast<uint8_t>(imm));
    } else {
        emit_byte(rex(true,false,false,dst>=8));
        emit_byte(0x81); emit_byte(modrm(3,0,dst&7)); emit_u32(static_cast<uint32_t>(imm));
    }
}
void FrostJIT::emit_alu_imm(int dst, int kind, int32_t imm) {
    // r64, imm8/imm32 with the /digit from `kind` (0=ADD 1=OR 4=AND
    // 5=SUB 6=XOR), for constant-src2 folding. Same encoding family as
    // emit_add_reg_imm; imm32 sign-extends to 64 bits.
    if (imm >= -128 && imm <= 127) {
        emit_byte(rex(true,false,false,dst>=8));
        emit_byte(0x83); emit_byte(modrm(3,kind,dst&7)); emit_byte(static_cast<uint8_t>(imm));
    } else {
        emit_byte(rex(true,false,false,dst>=8));
        emit_byte(0x81); emit_byte(modrm(3,kind,dst&7)); emit_u32(static_cast<uint32_t>(imm));
    }
}
void FrostJIT::emit_sub_reg(int dst, int src) {
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x29); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_adc_reg(int dst, int src) {
    // adc r64, r64: REX.W 11 /r
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x11); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_sbb_reg(int dst, int src) {
    // sbb r64, r64: REX.W 19 /r
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x19); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_and_reg(int dst, int src) {
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x21); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_or_reg(int dst, int src) {
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x09); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_xor_reg(int dst, int src) {
    emit_byte(rex(true,src>=8,false,dst>=8)); emit_byte(0x31); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_imul_reg(int dst, int src) {
    emit_byte(rex(true,dst>=8,false,src>=8)); emit_byte(0x0F); emit_byte(0xAF); emit_byte(modrm(3,dst&7,src&7));
}
void FrostJIT::emit_test_reg(int a, int b) {
    emit_byte(rex(true,b>=8,false,a>=8)); emit_byte(0x85); emit_byte(modrm(3,b&7,a&7));
}
void FrostJIT::emit_cmp_reg(int a, int b) {
    emit_byte(rex(true,b>=8,false,a>=8)); emit_byte(0x39); emit_byte(modrm(3,b&7,a&7));
}
void FrostJIT::emit_shift_cl(int dst, int kind) {
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0xD3); emit_byte(modrm(3,kind,dst&7));
}
void FrostJIT::emit_shift_imm8(int dst, int kind, uint8_t cnt) {
    if (cnt == 0) return;
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0xC1); emit_byte(modrm(3,kind,dst&7)); emit_byte(cnt);
}
void FrostJIT::emit_not_reg(int dst) {
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0xF7); emit_byte(modrm(3,2,dst&7));
}
void FrostJIT::emit_neg_reg(int dst) {
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0xF7); emit_byte(modrm(3,3,dst&7));
}
void FrostJIT::emit_lzcnt_reg(int dst, int src) {
    emit_byte(0xF3); emit_byte(rex(true,dst>=8,false,src>=8)); emit_byte(0x0F); emit_byte(0xBD); emit_byte(modrm(3,dst&7,src&7));
}
void FrostJIT::emit_bswap_reg(int dst) {
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0x0F); emit_byte(0xC8 + (dst&7));
}
void FrostJIT::emit_call_abs(void* target) {
    emit_mov_imm64(RAX, reinterpret_cast<uint64_t>(target));
    emit_byte(0xFF); emit_byte(0xD0);
}
void FrostJIT::emit_ret() { emit_byte(0xC3); }
void FrostJIT::emit_nop() { emit_byte(0x90); }
void FrostJIT::emit_push(int reg) {
    if (reg>=8) { emit_byte(0x41); emit_byte(0x50+(reg&7)); }
    else emit_byte(0x50+reg);
}
void FrostJIT::emit_pop(int reg) {
    if (reg>=8) { emit_byte(0x41); emit_byte(0x58+(reg&7)); }
    else emit_byte(0x58+reg);
}
size_t FrostJIT::emit_jmp_rel32_placeholder() {
    size_t off = code_buf_used_; emit_byte(0xE9); emit_u32(0); return off;
}
void FrostJIT::patch_jmp_rel32(size_t off, int32_t rel) {
    memcpy(code_buf_+off+1, &rel, 4);
}
size_t FrostJIT::emit_jcc_rel32_placeholder(uint8_t cc) {
    size_t off = code_buf_used_; emit_byte(0x0F); emit_byte(0x80+cc); emit_u32(0); return off;
}
void FrostJIT::patch_jcc_rel32(size_t off, int32_t rel) {
    memcpy(code_buf_+off+2, &rel, 4);
}
// rel8 jumps: jcc rel8 = 0x70+cc <rel8> (2 bytes)
size_t FrostJIT::emit_jcc_rel8_placeholder(uint8_t cc) {
    size_t off = code_buf_used_; emit_byte(0x70 + cc); emit_byte(0); return off;
}
void FrostJIT::patch_jcc_rel8(size_t off, int8_t rel) {
    code_buf_[off+1] = static_cast<uint8_t>(rel);
}
// sub rsp, imm8 / add rsp, imm8 (REX.W 83 EC NN / REX.W 83 C4 NN)
void FrostJIT::emit_sub_rsp_imm8(uint8_t n) {
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xEC); emit_byte(n);
}
void FrostJIT::emit_add_rsp_imm8(uint8_t n) {
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte(n);
}
// and cl, imm8 — used to mask shift counts to 0..63 / 0..31.
// Encoding: REX.W 83 E1 NN.
void FrostJIT::emit_and_cl_imm8(uint8_t mask) {
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(mask);
}
// pushfq / popfq — save/restore x86 RFLAGS.
// Encoding: 0x9C / 0x9D.
// Used around C calls to preserve pending flag state, and as part of
// the RSP-16-alignment dance before calls (pushfq adjusts RSP by 8).
void FrostJIT::emit_pushfq() { emit_byte(0x9C); }
void FrostJIT::emit_popfq()  { emit_byte(0x9D); }
// emit_call_aligned — see header for the full contract.
// At JIT body entry RSP%16==8. After the caller's `num_pushed` pushes,
// RSP%16 == (8 + 8*num_pushed) % 16. We need RSP%16==0 right before
// the CALL instruction (ABI requirement).
//
// Total pushes including the pushfq below = num_pushed + 1. For
// RSP%16==0 we need (8 * (num_pushed + 1)) % 16 == 0, i.e.
// num_pushed must be EVEN. If num_pushed is ODD, we emit `sub rsp, 8`
// first (effectively adding 1 more "push"), making the total even.
void FrostJIT::emit_call_aligned(void* target, int num_pushed) {
    bool need_align = (num_pushed & 1) != 0;  // ODD → misaligned
    if (need_align) emit_sub_rsp_imm8(8);
    emit_pushfq();
    emit_call_abs(target);
    emit_popfq();
    if (need_align) emit_add_rsp_imm8(8);
}
// ── ARM64 reg access (all in [RBX + REGS_OFF + 8*n]) ──────────────────
void FrostJIT::emit_load_arm(int xr, int ar) {
    if (ar >= 0 && ar <= 30) emit_load(xr, CPU_REG, REGS_OFF + 8*ar);
    else if (ar == 31)       emit_load(xr, CPU_REG, SP_OFF);
    else                     emit_mov_imm32(xr, 0); // XZR
}
void FrostJIT::emit_store_arm(int ar, int xr) {
    if (ar >= 0 && ar <= 30) emit_store(CPU_REG, REGS_OFF + 8*ar, xr);
    else if (ar == 31)       emit_store(CPU_REG, SP_OFF, xr);
    // XZR — discard
}
// ── NZCV materialization to cpu.pstate ────────────────────────────────
// Stores ARM NZCV flags to pstate. ARM C is inverted from x86 CF for SUB
// (ARM C = NOT borrow). We also store a "from_sub" flag in bit 27 so
// emit_load_flags_from_pstate can un-invert C back to x86 CF.
//
// pstate layout: N=bit31, Z=bit30, C=bit29, V=bit28, from_sub=bit27.
// This is compatible with the interpreter's pstate format (it ignores
// bit 27).
void FrostJIT::emit_materialize_flags(bool from_sub) {
    emit_push(RAX);
    emit_pushfq();
    emit_byte(0x58);  // pop rax
    emit_xor_reg(RDX, RDX);
    // N = SF = (RAX >> 7) & 1, placed at bit 31
    emit_mov_reg(RCX, RAX);
    emit_shift_imm8(RCX, 5, 7);
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01);
    emit_shift_imm8(RCX, 4, 31);
    emit_or_reg(RDX, RCX);
    // Z = ZF = (RAX >> 6) & 1, placed at bit 30
    emit_mov_reg(RCX, RAX);
    emit_shift_imm8(RCX, 5, 6);
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01);
    emit_shift_imm8(RCX, 4, 30);
    emit_or_reg(RDX, RCX);
    // C = ARM C. For ADD: ARM C = x86 CF. For SUB: ARM C = NOT x86 CF.
    emit_mov_reg(RCX, RAX);
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01); // and ecx, 1 (x86 CF)
    if (from_sub) {
        emit_byte(0x83); emit_byte(0xF1); emit_byte(0x01); // xor ecx, 1 (invert)
    }
    emit_shift_imm8(RCX, 4, 29);
    emit_or_reg(RDX, RCX);
    // V = OF = (RAX >> 11) & 1, placed at bit 28
    emit_mov_reg(RCX, RAX);
    emit_shift_imm8(RCX, 5, 11);
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01);
    emit_shift_imm8(RCX, 4, 28);
    emit_or_reg(RDX, RCX);
    // Store from_sub flag in bit 27.
    if (from_sub) {
        emit_byte(0x81); emit_byte(0xCA); emit_u32(0x08000000); // or edx, 1<<27
    }
    emit_store32(CPU_REG, PSTATE_OFF, RDX);
    emit_pop(RAX);
}
// Load NZCV from cpu.pstate into host flags.
// Converts ARM C back to x86 CF: if from_sub (bit 27), x86 CF = NOT ARM C.
// Otherwise x86 CF = ARM C.
void FrostJIT::emit_load_flags_from_pstate() {
    emit_load32(RAX, CPU_REG, PSTATE_OFF);  // eax = pstate
    emit_mov_reg(RCX, RAX);  // rcx = pstate
    // Check from_sub flag (bit 27)
    // (always emit the invert check — the from_sub bit may or may not be set)
    // RAX = 0x02 (reserved EFLAGS bit)
    emit_mov_imm32(RAX, 0x02);
    // N → SF (bit 7): (pstate >> 24) & 0x80
    emit_mov_reg(RDX, RCX);
    emit_shift_imm8(RDX, 5, 24);
    emit_byte(0x81); emit_byte(0xE2); emit_u32(0x00000080);
    emit_or_reg(RAX, RDX);
    // Z → ZF (bit 6): (pstate >> 24) & 0x40
    emit_mov_reg(RDX, RCX);
    emit_shift_imm8(RDX, 5, 24);
    emit_byte(0x81); emit_byte(0xE2); emit_u32(0x00000040);
    emit_or_reg(RAX, RDX);
    // C → CF (bit 0): extract ARM C from pstate bit 29.
    // If from_sub (bit 27 set), invert: x86 CF = NOT ARM C.
    // If not from_sub, x86 CF = ARM C.
    emit_mov_reg(RDX, RCX);
    emit_shift_imm8(RDX, 5, 29);
    emit_byte(0x83); emit_byte(0xE2); emit_byte(0x01); // and edx, 1 (ARM C)
    // Test bit 27 (from_sub)
    emit_byte(0xF7); emit_byte(0xC1); emit_u32(0x08000000); // test ecx, 1<<27
    // If from_sub (ZF=0 after test), invert C.
    // We use CMOV: if NOT ZF (from_sub), edx = NOT edx.
    // Simpler: XOR edx with (pstate >> 27) & 1.
    emit_mov_reg(R8, RCX);  // r8 = pstate
    emit_shift_imm8(R8, 5, 27);
    emit_byte(0x41); emit_byte(0x83); emit_byte(0xE0); emit_byte(0x01); // and r8d, 1
    emit_xor_reg(RDX, R8);  // if from_sub, flip C
    emit_or_reg(RAX, RDX);
    // V → OF (bit 11): (pstate >> 17) & 0x800
    emit_mov_reg(RDX, RCX);
    emit_shift_imm8(RDX, 5, 17);
    emit_byte(0x81); emit_byte(0xE2); emit_u32(0x00000800);
    emit_or_reg(RAX, RDX);
    emit_push(RAX);
    emit_popfq();
}
// Normalize x86 CF to SUB convention (x86 CF = NOT ARM C) after
// emit_load_flags_from_pstate. After loading, x86 CF = ARM C XOR from_sub:
//   from_sub=1: x86 CF = NOT ARM C (already SUB convention — no change)
//   from_sub=0: x86 CF = ARM C (ADD convention — need to invert)
// We read the from_sub bit from pstate at runtime and invert CF if needed.
// After this call, the default arm_cond_to_x86() mapping is correct.
// Uses RAX/RCX as scratch; caller must flush/invalidate them first.
void FrostJIT::emit_normalize_cf_to_sub_convention() {
    // Save current flags (including the loaded CF) to RAX.
    emit_pushfq();            // pushfq
    emit_byte(0x58);          // pop rax  (RAX = saved RFLAGS, CF is bit 0)
    // Read pstate and extract from_sub bit (bit 27).
    emit_load32(RCX, CPU_REG, PSTATE_OFF);  // mov ecx, [rbx+PSTATE_OFF]
    emit_shift_imm8(RCX, 5, 27);            // shr ecx, 27
    emit_byte(0x83); emit_byte(0xE1); emit_byte(0x01);  // and ecx, 1
    // Compute mask = NOT from_sub = 1 XOR from_sub.
    // If from_sub=0: mask=1 (need to flip CF).
    // If from_sub=1: mask=0 (CF already correct).
    emit_byte(0x83); emit_byte(0xF1); emit_byte(0x01);  // xor ecx, 1
    // XOR RAX bit 0 (CF) with the mask. This flips CF iff from_sub=0.
    emit_xor_reg(RAX, RCX);  // xor rax, rcx
    // Restore flags from RAX (CF is now normalized to SUB convention).
    emit_byte(0x50);          // push rax
    emit_popfq();             // popfq
}
// ── Condition code mapping ─────────────────────────────────────────────
// Maps ARM condition codes to x86 Jcc condition codes.
// Since emit_load_flags_from_pstate correctly restores x86 CF (un-inverting
// ARM C for SUB), we use the STANDARD x86 condition code mapping.
// ARM and x86 condition codes are identical once the flags are correctly
// loaded.
//
// x86 cc: 0=JO, 1=JNO, 2=JB/JC, 3=JAE/JNC, 4=JE/JZ, 5=JNE/JNZ,
//         6=JBE, 7=JA, 8=JS, 9=JNS, 10=JP, 11=JNP, 12=JL, 13=JGE,
//         14=JLE, 15=JG
uint8_t FrostJIT::arm_cond_to_x86(uint8_t arm_cond) const {
    // Default mapping assumes flags came from SUBS (the common case for
    // CMP/B.cc). After SUB: x86 CF = borrow = (a < b), ARM C = NOT borrow.
    // So ARM C=1 ↔ x86 CF=0.
    //   ARM CS (C=1) → x86 CF=0 → JAE(3)
    //   ARM CC (C=0) → x86 CF=1 → JB(2)
    //   ARM HI (C=1,Z=0) → x86 CF=0,ZF=0 → JA(7)
    //   ARM LS (C=0|ZF=1) → x86 CF=1|ZF=1 → JBE(6)
    switch (arm_cond & 0xE) {
        case 0x0: return (arm_cond & 1) ? 5 : 4;   // EQ→JE(4) / NE→JNE(5)
        case 0x2: return (arm_cond & 1) ? 2 : 3;   // CS→JAE(3) / CC→JB(2)
        case 0x4: return (arm_cond & 1) ? 9 : 8;   // MI→JS(8) / PL→JNS(9)
        case 0x6: return (arm_cond & 1) ? 1 : 0;   // VS→JO(0) / VC→JNO(1)
        case 0x8: return (arm_cond & 1) ? 6 : 7;   // HI→JA(7) / LS→JBE(6)
        case 0xA: return (arm_cond & 1) ? 12 : 13; // GE→JGE(13) / LT→JL(12)
        case 0xC: return (arm_cond & 1) ? 14 : 15; // GT→JG(15) / LE→JLE(14)
        default:  return 4;
    }
}
// Resolve an ARM condition code to an x86 Jcc code, accounting for the
// carry-polarity difference between ADD/TST (direct CF) and SUB (inverted).
// After ADD/TST, ARM C and x86 CF agree (both = carry-out / both = 0).
// After SUB, they're inverted (ARM C = NOT borrow, x86 CF = borrow).
// arm_cond_to_x86() assumes the SUB convention, so for ADD/TST:
//   - CS/CC: swap the mapping (no cmc needed)
//   - HI/LS: emit `cmc` to invert CF, then use the default mapping
//   - GE/LT/GT/LE: depend on N,V,Z only — default works
uint8_t FrostJIT::resolve_arm_cond_with_carry(uint8_t arm_cond, bool& need_cmc) {
    need_cmc = false;
    bool carry_is_direct = flags_in_host_ && !flags_from_sub_;
    uint8_t base = arm_cond & 0xE;
    if (!carry_is_direct) {
        return arm_cond_to_x86(arm_cond);
    }
    switch (base) {
        case 0x2:  // CS/CC — swap mappings
            return (arm_cond & 1) ? 3 : 2;  // CS→JB(2), CC→JAE(3)
        case 0x8:  // HI/LS — no direct JCC, use cmc + default
            need_cmc = true;
            return arm_cond_to_x86(arm_cond);
        default:  // EQ/NE/MI/PL/VS/VC/GE/LT/GT/LE — default works
            return arm_cond_to_x86(arm_cond);
    }
}
// ── Memory access helpers (C-callable from JIT) ────────────────────────
// These are referenced by name from JIT-compiled code in frostjit.cpp
// (emit_load_mem / emit_store_mem slow paths). They MUST be non-static
// so the JIT call sites can take their address.
//
// CRITICAL: these are extern "C" — C++ exceptions cannot propagate
// through them back into JIT'd code (which has no DWARF unwind info).
// Catch UnmappedMemory here and deliver SIGSEGV directly to the guest
// via the CPU pointer (passed as the 4th arg from the JIT). If a
// handler is installed, deliver_signal sets up the handler frame and
// the JIT run loop resumes at the handler's PC. If not, it sets
// cpu->running = false and cpu->exit_code = 139.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, int width) {
        uint64_t val = 0;
        try {
            emu->mem().read(addr, &val, width);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
            return 0;  // cpu->running is now false (or handler installed)
        }
        // Cache the env lookup — this runs on every JIT slow-path memory access.
        static const bool mem_trace_ = (getenv("BIFROST_MEM_TRACE") != nullptr);
        if (mem_trace_) {
            fprintf(stderr, "    [load] addr=0x%llx w=%d → 0x%llx\n",
                    static_cast<unsigned long long>(addr), width, static_cast<unsigned long long>(val));
        }
        return val;
    }
    // 16-byte SIMD load slow path. Writes v_lo[dst]/v_hi[dst] directly so the
    // JIT fast path can do a single bounds-check + one movupd instead of two
    // 8-byte LOAD_MEM/STORE_MEM (each with its own check + window add).
    void jit_load_mem16_slow(Emulator* emu, CPU* cpu, uint64_t addr, int dst) {
        uint64_t buf[2] = {0, 0};
        try {
            emu->mem().read(addr, buf, 16);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
            return;
        }
        cpu->v_lo[dst & 31] = buf[0];
        cpu->v_hi[dst & 31] = buf[1];
    }
    void jit_store_mem16_slow(Emulator* emu, CPU* cpu, uint64_t addr, int src) {
        uint64_t buf[2] = {cpu->v_lo[src & 31], cpu->v_hi[src & 31]};
        try {
            emu->mem().write(addr, buf, 16);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
        }
    }
    void jit_store_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        static const bool mem_trace_ = (getenv("BIFROST_MEM_TRACE") != nullptr);
        if (mem_trace_) {
            fprintf(stderr, "    [store] addr=0x%llx val=0x%llx w=%d\n",
                    static_cast<unsigned long long>(addr), static_cast<unsigned long long>(val), width);
        }
        try {
            emu->mem().write(addr, &val, width);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
        }
    }
    // ── Fast LL/SC helpers (bypass interpreter decode) ──────────────
    // These are called directly from JIT-compiled code (like jit_load_mem_slow)
    // to avoid the full interpreter step overhead (decode cache → switch →
    // handler). They implement the same global exclusive monitor logic as
    // the interpreter's LDXR/STXR/STLR handlers, but skip decode entirely.
    //
    // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=width
    // Returns (LDXR): the loaded value in RAX.
    // Returns (STXR): 0=success, 1=failure in RAX.
    uint64_t jit_ldxr(Emulator* emu, CPU* cpu, uint64_t addr, int width) {
        auto shard = reinterpret_cast<Emulator::ExclMonitorShardAccess*>(emu->excl_monitor_shard_pub(addr));
        std::lock_guard<std::mutex> g(shard->mu);
        uint64_t v = 0;
        try {
            emu->mem().read(addr, &v, width);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
            return 0;
        }
        cpu->excl_mark(addr, width);
        auto& vec = shard->reservations[addr];
        bool found = false;
        for (auto*& p : vec) {
            if (p == cpu) { found = true; break; }
        }
        if (!found) vec.push_back(cpu);
        return v;
    }
    // STXR: returns 0=success, 1=failure. val is in R8 (passed as 5th arg).
    uint64_t jit_stxr(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        auto shard = reinterpret_cast<Emulator::ExclMonitorShardAccess*>(emu->excl_monitor_shard_pub(addr));
        std::lock_guard<std::mutex> g(shard->mu);
        bool ok = cpu->excl_check(addr, width);
        if (ok) {
            try {
                emu->mem().write(addr, &val, width);
            } catch (UnmappedMemory& e) {
                (void)e;
                deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
                return 1;
            }
            // Invalidate OTHER CPUs' reservations at this address.
            auto it = shard->reservations.find(addr);
            if (it != shard->reservations.end()) {
                for (CPU* p : it->second) {
                    if (p != cpu && p->excl_tag_valid) {
                        p->excl_tag_valid = false;
                    }
                }
                it->second.erase(
                    std::remove(it->second.begin(), it->second.end(), (CPU*)cpu),
                    it->second.end());
                if (it->second.empty()) {
                    shard->reservations.erase(it);
                }
            }
        }
        cpu->excl_clear();
        return ok ? 0 : 1;
    }
    // STLR: store-release (always succeeds, invalidates other CPUs).
    void jit_stlr(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        auto shard = reinterpret_cast<Emulator::ExclMonitorShardAccess*>(emu->excl_monitor_shard_pub(addr));
        std::lock_guard<std::mutex> g(shard->mu);
        try {
            emu->mem().write(addr, &val, width);
        } catch (UnmappedMemory& e) {
            (void)e;
            deliver_signal(*emu, *cpu, emu->signals(), BIFROST_SIGSEGV);
            return;
        }
        auto it = shard->reservations.find(addr);
        if (it != shard->reservations.end()) {
            for (CPU* p : it->second) {
                if (p != cpu && p->excl_tag_valid) {
                    p->excl_tag_valid = false;
                }
            }
            shard->reservations.erase(it);
        }
    }
}
// ── Register allocator ──────────────────────────────────────────────────
// Maps vregs to x86 registers for the duration of a block. Vregs 0-31
// are architectural (live in cpu.regs[]/sp); vregs 33+ are scratch
// (live on the stack at [RBP - 8*(v-30)]).
//
// Allocator state:
//   vreg_home_[v] = x86 reg holding v, or -1 if not cached.
//   reg_vreg_[r]  = vreg currently in x86 reg r, or -1.
//   vreg_dirty_[v] = true if the cached value differs from cpu.regs[]/stack.
//
// Available x86 regs (9 total):
//   Caller-saved (clobbered by C calls): RAX, RCX, RDX, R8, R9, R11
//   Callee-saved (preserved by C calls): R12, R13, R15
// Reserved: RBX=CPU, R14=EMU, R10=window, RBP=frame, RSP=stack.
// constexpr int FrostJIT::ALLOC_REGS[] = {RAX,RCX,RDX,R8,R9,R11,R12,R13,R15};
// (defined in frostjit.hpp; vregs cached in R12/R13/R15 survive CALL_INTERP)
// ── emit_load_mem / emit_store_mem ─────────────────────────────────────
// Memory access through the direct window (R10) when the address is in
// the low 4 GiB; falls back to the C helper (jit_load_mem_slow /
// jit_store_mem_slow, defined above) for high addresses.
// Move a 64-bit immediate into RAX. Uses the shorter mov imm32 + zext
// form when the value fits in 32 bits, otherwise the 10-byte mov imm64.
void FrostJIT::emit_mov_imm_to_rax(uint64_t val) {
    if (val <= 0xFFFFFFFFULL) {
        emit_mov_imm32_zext(RAX, static_cast<uint32_t>(val));
    } else {
        emit_mov_imm64(RAX, val);
    }
}
void FrostJIT::emit_load_mem(int dst, int addr_reg, int32_t off, int w,
                             bool sign_ext) {
    // dst = addr_reg + off
    if (addr_reg != dst) emit_mov_reg(dst, addr_reg);
    if (off != 0) {
        if (off >= -128 && off <= 127) {
            emit_byte(rex(true,false,false,dst>=8));
            emit_byte(0x83); emit_byte(modrm(3,0,dst&7)); emit_byte(static_cast<uint8_t>(off));
        } else {
            emit_byte(rex(true,false,false,dst>=8));
            emit_byte(0x81); emit_byte(modrm(3,0,dst&7)); emit_u32(static_cast<uint32_t>(off));
        }
    }
    // Check if addr + w <= 4GB. The limit is always < 2^32, so a 32-bit
    // zero-extending mov is 5-6 bytes instead of the 10-byte movabs.
    uint64_t limit = Memory::DIRECT_WINDOW_SIZE - w;
    int tmp = (dst != RDX) ? RDX : RCX;
    emit_mov_imm32_zext(tmp, static_cast<uint32_t>(limit));
    emit_cmp_reg(dst, tmp);
    size_t jbe_patch = emit_jcc_rel32_placeholder(6); // JBE
    // Slow path. RSP%16==8 at body entry; caller pushes R10 (1 push, ODD)
    // → emit_call_aligned handles the sub rsp,8 + pushfq + call + popfq +
    // add rsp,8 dance automatically. We just set up args and call.
    //
    // Args: jit_load_mem_slow(emu, cpu, addr, width)
    //   RDI = emu, RSI = cpu, RDX = addr, RCX = width
    emit_push(WIN_REG);                  // 1 push — ODD, helper will sub rsp,8
    emit_load(RDI, RBP, emu_slot_off()); // rdi = emu
    emit_mov_reg(RSI, CPU_REG);          // rsi = cpu (for SIGSEGV delivery)
    emit_mov_reg(RDX, dst);              // rdx = addr
    emit_mov_imm32(RCX, w);              // rcx = width
    emit_call_aligned(&jit_load_mem_slow, /*num_pushed=*/1);
    emit_pop(WIN_REG);                   // restore R10
    // RAX now has the return value (the loaded data).
    if (dst != RAX) emit_mov_reg(dst, RAX);
    size_t jmp_past = emit_jmp_rel32_placeholder();
    // Fast path.
    int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
    patch_jcc_rel32(jbe_patch, fast_rel);
    emit_add_reg(dst, WIN_REG);
    if (w == 8) {
        emit_byte(rex(true,dst>=8,false,false)); emit_byte(0x8B); emit_byte(modrm(0,dst&7,dst&7));
    } else if (w == 4) {
        if (sign_ext) emit_load32_sx(dst, dst, 0); else emit_load32(dst, dst, 0);
    } else if (w == 2) {
        if (sign_ext) emit_load16_sx(dst, dst, 0); else emit_load16(dst, dst, 0);
    } else if (w == 1) {
        if (sign_ext) emit_load8_sx(dst, dst, 0); else emit_load8(dst, dst, 0);
    }
    int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
    patch_jmp_rel32(jmp_past, end_rel);
}
void FrostJIT::emit_store_mem(int addr_reg, int32_t off, int src_reg, int w) {
    // We use R8 as the address scratch (NOT RDX/RCX, since the caller
    // passes addr in RAX and val in RCX, and we must not clobber either
    // before the limit check).
    //
    // R8 = addr_reg + off
    emit_mov_reg(R8, addr_reg);
    if (off != 0) {
        if (off >= -128 && off <= 127) {
            emit_byte(rex(true,false,false,R8>=8));
            emit_byte(0x83); emit_byte(modrm(3,0,R8&7)); emit_byte(static_cast<uint8_t>(off));
        } else {
            emit_byte(rex(true,false,false,R8>=8));
            emit_byte(0x81); emit_byte(modrm(3,0,R8&7)); emit_u32(static_cast<uint32_t>(off));
        }
    }
    // Limit check: R9 = limit. cmp R8, R9. Limit is < 2^32 → 32-bit mov.
    uint64_t limit = Memory::DIRECT_WINDOW_SIZE - w;
    emit_mov_imm32_zext(R9, static_cast<uint32_t>(limit));
    emit_cmp_reg(R8, R9);
    size_t jbe_patch = emit_jcc_rel32_placeholder(6);
    // Slow path: call jit_store_mem_slow(emu, cpu, addr, val, width).
    // 3 pushes (src, RAX, R10) — ODD, so emit_call_aligned handles the
    // sub rsp,8 + pushfq + call + popfq + add rsp,8 automatically.
    //
    // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=val, R8=width
    emit_push(src_reg);            // save val (RCX)  — 1 push
    emit_push(RAX);                // save RAX        — 2 pushes
    emit_push(WIN_REG);            // save R10        — 3 pushes (ODD)
    emit_load(RDI, RBP, emu_slot_off()); // rdi = emu
    emit_mov_reg(RSI, CPU_REG);    // rsi = cpu (for SIGSEGV delivery)
    emit_mov_reg(RDX, R8);         // rdx = addr (from R8)
    emit_mov_reg(RCX, src_reg);    // rcx = val (from src_reg=RCX)
    emit_mov_imm32(R8, w);         // r8 = width
    emit_call_aligned(&jit_store_mem_slow, /*num_pushed=*/3);
    emit_pop(WIN_REG);             // restore R10
    emit_pop(RAX);                 // restore RAX
    emit_pop(src_reg);             // restore val (RCX)
    size_t jmp_past = emit_jmp_rel32_placeholder();
    // Fast path: direct window store.
    int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
    patch_jcc_rel32(jbe_patch, fast_rel);
    // R8 = R10 + R8 (window_base + guest_addr)
    // add r8, r10: REX.W+R+B (0x4D), opcode 0x01, modrm(3, r10&7=2, r8&7=0)=0xD0
    emit_byte(0x4D); emit_byte(0x01); emit_byte(0xD0);
    // Store to [R8] with the right width.
    if (w == 8) {
        emit_byte(rex(true,src_reg>=8,false,R8>=8)); emit_byte(0x89); emit_byte(modrm(0,src_reg&7,R8&7));
    } else if (w == 4) emit_store32(R8, 0, src_reg);
    else if (w == 2) emit_store16(R8, 0, src_reg);
    else if (w == 1) emit_store8(R8, 0, src_reg);
    int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
    patch_jmp_rel32(jmp_past, end_rel);
}
} // namespace arm64emu
