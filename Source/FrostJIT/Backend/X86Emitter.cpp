// X86Emitter.cpp — low-level x86-64 instruction emitters.
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
//   - Memory load/store via the guest reservation + slow-path C helper fallback
//
// These emitters have NO knowledge of the IR or the register allocator —
// they are pure code generators that take x86 register numbers and
// offsets. The regalloc and IR compiler layer on top.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/Emulator.hpp"  // Emulator complete type (for slow-path helpers)
#include "System/Core/ExclusiveMonitor.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace arm64emu {
bool FrostJIT::ensure_code_space(size_t bytes) {
    if (bytes > code_buf_limit_ - code_buf_used_) return false;
    const size_t needed = code_buf_used_ + bytes;
    if (needed > code_capacity_) {
        code_capacity_ = std::min(code_buf_limit_, std::max(needed, code_capacity_ * 2));
        code_cache_growths.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
void FrostJIT::emit_byte(uint8_t b) {
    if (ensure_code_space(1))
        code_buf_[code_buf_used_++] = b;
    else
        code_buf_overflow_ = true;
}
void FrostJIT::emit_u32(uint32_t v) {
    if (ensure_code_space(4))
        memcpy(code_buf_ + code_buf_used_, &v, 4), code_buf_used_ += 4;
    else
        code_buf_overflow_ = true;
}
void FrostJIT::emit_u64(uint64_t v) {
    if (recording_)
        imm64_fields_.push_back({static_cast<uint32_t>(code_buf_used_), v, HostReloc::None, 0});
    if (ensure_code_space(8))
        memcpy(code_buf_ + code_buf_used_, &v, 8), code_buf_used_ += 8;
    else
        code_buf_overflow_ = true;
}
// ── x86 helpers ────────────────────────────────────────────────────────
uint8_t FrostJIT::rex(bool w, bool r, bool x, bool b) {
    return 0x40 | (w?8:0) | (r?4:0) | (x?2:0) | (b?1:0);
}
void FrostJIT::emit_rex_if_needed(bool r, bool x, bool b) {
    const uint8_t prefix = rex(false, r, x, b);
    if (prefix != 0x40) emit_byte(prefix);
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
void FrostJIT::emit_mov_const(int dst, uint64_t imm) {
    if (imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(dst, static_cast<uint32_t>(imm));
    else if (static_cast<int64_t>(imm) < 0 && static_cast<int64_t>(imm) >= INT32_MIN)
        emit_mov_imm32(dst, static_cast<uint32_t>(imm));  // sign-extended
    else emit_mov_imm64(dst, imm);
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
    emit_rex_if_needed(dst>=8,false,base>=8);
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
    emit_rex_if_needed(src>=8,false,base>=8);
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
// ModRM+SIB+disp addressing [WIN_REG + index*1 + disp] for the direct-
// window fast paths (emit_load_mem / emit_store_mem). WIN_REG is an
// extension register (R10), so the base is only expressible through a SIB;
// the index rides in the SIB index field, which is what lets the guest
// address register be used WITHOUT being copied or destroyed. Offset 0 needs
// no displacement: only SIB base=101 (RBP/R13) requires one with mod=00.
void FrostJIT::emit_modrm_sib_win(int reg, int index, int32_t off) {
    static_assert(WIN_REG != RBP && WIN_REG != R13, "mod=00 with SIB base=101 has a disp32");
    if (off == 0) {
        emit_byte(modrm(0,reg&7,4));
        emit_byte(sib(0,index&7,WIN_REG&7));
    } else if (off>=-128&&off<=127) {
        emit_byte(modrm(1,reg&7,4));
        emit_byte(sib(0,index&7,WIN_REG&7));
        emit_byte(static_cast<uint8_t>(off));
    } else {
        emit_byte(modrm(2,reg&7,4));
        emit_byte(sib(0,index&7,WIN_REG&7));
        emit_u32(static_cast<uint32_t>(off));
    }
}
void FrostJIT::emit_load16(int dst, int base, int32_t off) {
    emit_rex_if_needed(dst>=8,false,base>=8);
    emit_byte(0x0F); emit_byte(0xB7);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load8(int dst, int base, int32_t off) {
    emit_rex_if_needed(dst>=8,false,base>=8);
    emit_byte(0x0F); emit_byte(0xB6);
    emit_modrm_disp(dst, base, off);
}
// cmp byte [WIN_REG + JIT_SOFTWARE_CHECKS_OFF], 0 — the sticky software
// guard flag lives in the header page just below the guest window base.
// Only flags change; valid wherever WIN_REG holds the window base.
void FrostJIT::emit_test_software_checks() {
    emit_byte(rex(false, false, false, WIN_REG >= 8));
    emit_byte(0x80);
    emit_modrm_disp(7, WIN_REG, Memory::JIT_SOFTWARE_CHECKS_OFF);
    emit_byte(0);
}
void FrostJIT::emit_load32_sx(int dst, int base, int32_t off) {
    emit_byte(rex(true,dst>=8,false,base>=8));
    emit_byte(0x63);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load16_sx(int dst, int base, int32_t off) {
    emit_rex_if_needed(dst>=8,false,base>=8);
    emit_byte(0x0F); emit_byte(0xBF);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_load8_sx(int dst, int base, int32_t off) {
    emit_rex_if_needed(dst>=8,false,base>=8);
    emit_byte(0x0F); emit_byte(0xBE);
    emit_modrm_disp(dst, base, off);
}
void FrostJIT::emit_store16(int base, int32_t off, int src) {
    emit_byte(0x66);
    emit_rex_if_needed(src>=8,false,base>=8);
    emit_byte(0x89);
    emit_modrm_disp(src, base, off);
}
void FrostJIT::emit_store8(int base, int32_t off, int src) {
    emit_byte(rex(false,src>=8,false,base>=8));
    emit_byte(0x88);
    emit_modrm_disp(src, base, off);
}
void FrostJIT::emit_add_reg(int dst, int src) {
    emit_alu_rex(src>=8,false,dst>=8); emit_byte(0x01); emit_byte(modrm(3,src&7,dst&7));
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
    // emit_add_reg_imm; imm32 sign-extends to 64 bits (32 under alu_w32_).
    if (imm >= -128 && imm <= 127) {
        emit_alu_rex(false,false,dst>=8);
        emit_byte(0x83); emit_byte(modrm(3,kind,dst&7)); emit_byte(static_cast<uint8_t>(imm));
    } else {
        emit_alu_rex(false,false,dst>=8);
        emit_byte(0x81); emit_byte(modrm(3,kind,dst&7)); emit_u32(static_cast<uint32_t>(imm));
    }
}
// REX for the ALU emitters: REX.W unless alu_w32_ selects the 32-bit form,
// whose result zero-extends bits 63:32. A bare 0x40 prefix is omitted.
void FrostJIT::emit_alu_rex(bool r, bool x, bool b) {
    const uint8_t prefix = rex(!alu_w32_, r, x, b);
    if (prefix != 0x40) emit_byte(prefix);
}
void FrostJIT::emit_sub_reg(int dst, int src) {
    emit_alu_rex(src>=8,false,dst>=8); emit_byte(0x29); emit_byte(modrm(3,src&7,dst&7));
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
    emit_alu_rex(src>=8,false,dst>=8); emit_byte(0x21); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_or_reg(int dst, int src) {
    emit_alu_rex(src>=8,false,dst>=8); emit_byte(0x09); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_xor_reg(int dst, int src) {
    emit_alu_rex(src>=8,false,dst>=8); emit_byte(0x31); emit_byte(modrm(3,src&7,dst&7));
}
void FrostJIT::emit_imul_reg(int dst, int src) {
    emit_alu_rex(dst>=8,false,src>=8); emit_byte(0x0F); emit_byte(0xAF); emit_byte(modrm(3,dst&7,src&7));
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
// Leading zeros of a 64-bit register, in place; clobbers RFLAGS. Without
// ABM, F3 0F BD runs as BSR, so spell that out: BSR sets ZF and leaves the
// destination undefined for zero. 127 ^ 63 = 64, and n ^ 63 = 63 - n for 0..63.
void FrostJIT::emit_clz64_reg(int reg) {
    if (has_lzcnt()) {
        emit_lzcnt_reg(reg, reg);
        return;
    }
    emit_byte(rex(true,reg>=8,false,reg>=8)); emit_byte(0x0F); emit_byte(0xBD); emit_byte(modrm(3,reg&7,reg&7));
    emit_byte(0x75); emit_byte(0x07);  // jnz over the 7-byte mov
    emit_byte(rex(true,false,false,reg>=8)); emit_byte(0xC7); emit_byte(modrm(3,0,reg&7)); emit_u32(127);
    emit_byte(rex(true,false,false,reg>=8)); emit_byte(0x83); emit_byte(modrm(3,6,reg&7)); emit_byte(63);
}
void FrostJIT::emit_bswap_reg(int dst) {
    emit_byte(rex(true,false,false,dst>=8)); emit_byte(0x0F); emit_byte(0xC8 + (dst&7));
}
void FrostJIT::emit_call_abs(void* target) {
    block_calls_out_ = true;  // see chain_entry_pc_off_
    emit_mov_imm64(RAX, reinterpret_cast<uint64_t>(target));
    note_host_reloc(HostReloc::Image);
    emit_byte(0xFF); emit_byte(0xD0);
}
void FrostJIT::emit_frame_return(bool cold) {
    // The tail is the same everywhere and only an unchained exit reaches it.
    if (use_cold_stubs() && (!cold || frame_return_off_ == SIZE_MAX)) {
        const size_t jump = emit_jmp_rel32_placeholder();
        patch_jmp_rel32(jump, static_cast<int32_t>(static_cast<int64_t>(frame_exit_stub_) - static_cast<int64_t>(jump + 5)));
        return;
    }
    if (cold && frame_return_off_ != SIZE_MAX) {
        const int64_t rel8 = static_cast<int64_t>(frame_return_off_) - static_cast<int64_t>(code_buf_used_ + 2);
        if (rel8 >= -128) { emit_byte(0xEB); emit_byte(static_cast<uint8_t>(rel8)); return; }
        const size_t jump = emit_jmp_rel32_placeholder();
        patch_jmp_rel32(jump, static_cast<int32_t>(frame_return_off_ - (jump + 5)));
        return;
    }
    if (frame_return_off_ == SIZE_MAX) frame_return_off_ = code_buf_used_;
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    emit_ret();
}
void FrostJIT::emit_ret() { emit_byte(0xC3); }
void FrostJIT::emit_nop() { emit_byte(0x90); }
// One decoded instruction instead of five single-byte NOPs on every
// unchained exit and indirect-branch probe.
void FrostJIT::emit_slot_nop5() { for (uint8_t b : kSlotNop5) emit_byte(b); }
void FrostJIT::write_unpatched_slot(size_t off) {
    if (chain_skip_enabled()) {
        std::memcpy(code_buf_ + off + 1, kSlotNop5 + 1, 4);
        std::atomic_thread_fence(std::memory_order_release);
        code_buf_[off] = kSlotNop5[0];
    } else {
        code_buf_[off] = 0xC3;  // ret; the following bytes never execute
    }
}
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
    if (code_buf_overflow_ || off > code_buf_limit_ || 5 > code_buf_limit_ - off) {
        code_buf_overflow_ = true;
        return;
    }
    memcpy(code_buf_+off+1, &rel, 4);
}
size_t FrostJIT::emit_call_rel32_placeholder() {
    block_calls_out_ = true;  // a native callee: see chain_entry_pc_off_
    size_t off = code_buf_used_; emit_byte(0xE8); emit_u32(0); return off;
}
void FrostJIT::patch_call_rel32(size_t off, const uint8_t* target) {
    if (code_buf_overflow_ || off > code_buf_limit_ || 5 > code_buf_limit_ - off) {
        code_buf_overflow_ = true;
        return;
    }
    int64_t rel = target - (code_buf_ + off + 5);
    int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(code_buf_+off+1, &rel32, 4);
}
size_t FrostJIT::emit_jcc_rel32_placeholder(uint8_t cc) {
    size_t off = code_buf_used_; emit_byte(0x0F); emit_byte(0x80+cc); emit_u32(0); return off;
}
void FrostJIT::patch_jcc_rel32(size_t off, int32_t rel) {
    if (code_buf_overflow_ || off > code_buf_limit_ || 6 > code_buf_limit_ - off) {
        code_buf_overflow_ = true;
        return;
    }
    memcpy(code_buf_+off+2, &rel, 4);
}
// rel8 jumps: jcc rel8 = 0x70+cc <rel8> (2 bytes)
size_t FrostJIT::emit_jcc_rel8_placeholder(uint8_t cc) {
    size_t off = code_buf_used_; emit_byte(0x70 + cc); emit_byte(0); return off;
}
void FrostJIT::patch_jcc_rel8(size_t off, int8_t rel) {
    if (code_buf_overflow_ || off > code_buf_limit_ || 2 > code_buf_limit_ - off) {
        code_buf_overflow_ = true;
        return;
    }
    code_buf_[off+1] = static_cast<uint8_t>(rel);
}
// sub rsp, imm8 / add rsp, imm8 (REX.W 83 EC NN / REX.W 83 C4 NN)
void FrostJIT::emit_sub_rsp_imm8(uint8_t n) {
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xEC); emit_byte(n);
}
void FrostJIT::emit_add_rsp_imm8(uint8_t n) {
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte(n);
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
void FrostJIT::emit_call_aligned(void* target, int num_pushed, bool preserve_flags) {
    if (!preserve_flags) {
        // NZCV must already be materialized and flags_in_host_ invalidated.
        // With no PUSHFQ, an odd caller push count is already ABI-aligned.
        const bool pad = (num_pushed & 1) == 0;
        if (pad) emit_sub_rsp_imm8(8);
        emit_call_abs(target);
        if (pad) emit_add_rsp_imm8(8);
        return;
    }
    bool need_align = (num_pushed & 1) != 0;  // ODD → misaligned
    if (need_align) emit_sub_rsp_imm8(8);
    emit_pushfq();
    emit_call_abs(target);
    emit_popfq();
    if (need_align) emit_add_rsp_imm8(8);
}
void FrostJIT::emit_load_emu(int dst) { emit_load(dst, CPU_REG, offsetof(CPU, jit_emu)); }
void FrostJIT::emit_store_emu(int src) { emit_store(CPU_REG, offsetof(CPU, jit_emu), src); }
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
// ── NZCV materialization to cpu.nzcv_host ─────────────────────────────
// Scratch: clobbers RAX unless keep_rax saves it (epilogues with
// rax_holds_next_pc_ commit flags after RAX already holds the next PC).
// LAHF/SAHF in 64-bit mode (CPUID LAHF-SAHF) exists on every x86-64 CPU
// except the first Athlon 64 / Pentium 4 steppings; it is not gated.
void FrostJIT::emit_materialize_flags(bool from_sub, bool keep_rax) {
    // cpu.nzcv_host is the `lahf; seto al` image with x86 CF = NOT ARM C, so a
    // subtraction's flags store as they are and an addition's (or a logical
    // op's, CF = 0 for C = 0) take one `cmc` first. Host CF is left flipped:
    // callers that keep using the host flags wrap this in pushfq/popfq.
    if (keep_rax) emit_push(RAX);
    if (!from_sub) emit_byte(0xF5);                     // cmc
    emit_byte(0x9F);                                    // lahf
    emit_byte(0x0F); emit_byte(0x90); emit_byte(0xC0);  // seto al
    emit_byte(0x66); emit_byte(0x89); emit_modrm_disp(RAX, CPU_REG, NZCV_OFF);  // mov [nzcv], ax
    if (keep_rax) emit_pop(RAX);
}
// Load NZCV from cpu.nzcv_host into host flags in SUB convention:
// SF=N, ZF=Z, OF=V and x86 CF = NOT ARM C, so arm_cond_to_x86() applies
// directly. Callers set flags_from_sub_ = true.
// Clobbers only RAX (and RFLAGS); callers that flush FLAGS3 around it are
// over-cautious but correct. SAHF loads SF/ZF/CF from AH and leaves OF
// alone; OF comes from `add al, 0x7F` with AL = V (0x7F+1 overflows,
// 0x7F+0 does not), which only writes AL, so AH survives for the SAHF.
void FrostJIT::emit_load_flags_from_pstate() {
    emit_load32(RAX, CPU_REG, NZCV_OFF);                // eax = nzcv_host
    emit_byte(0x04); emit_byte(0x7F);                    // add al, 0x7F → OF = V
    emit_byte(0x9E);                                     // sahf
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
// These are referenced by name from JIT-compiled code in FrostJIT.cpp
// (emit_load_mem / emit_store_mem slow paths). They MUST be non-static
// so the JIT call sites can take their address.
//
// CRITICAL: these are extern "C" — C++ exceptions cannot propagate
// through them back into JIT'd code (which has no DWARF unwind info).
// Catch UnmappedMemory here and deliver SIGSEGV directly to the guest
// via the CPU pointer (passed as the 4th arg from the JIT). If a
// handler is installed, force_signal sets up the handler frame and
// the JIT run loop resumes at the handler's PC. Otherwise (or when the
// signal is blocked/ignored) the default action ends the process.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, int width) {
        uint64_t val = 0;
        try {
            emu->mem().read(addr, &val, width);
        } catch (UnmappedMemory& e) {
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
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
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
            return;
        }
        cpu->v_lo[dst & 31] = buf[0];
        cpu->v_hi[dst & 31] = buf[1];
    }
    void jit_store_mem16_slow(Emulator* emu, CPU* cpu, uint64_t addr, int src) {
        uint64_t buf[2] = {cpu->v_lo[src & 31], cpu->v_hi[src & 31]};
        try {
            Memory::note_interp_pc(cpu->pc);  // BIFROST_WRITE_TRACE attribution
            emu->mem().write(addr, buf, 16);
        } catch (UnmappedMemory& e) {
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
        }
    }
    void jit_store_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        static const bool mem_trace_ = (getenv("BIFROST_MEM_TRACE") != nullptr);
        if (mem_trace_) {
            fprintf(stderr, "    [store] addr=0x%llx val=0x%llx w=%d\n",
                    static_cast<unsigned long long>(addr), static_cast<unsigned long long>(val), width);
        }
        try {
            Memory::note_interp_pc(cpu->pc);  // BIFROST_WRITE_TRACE attribution
            emu->mem().write(addr, &val, width);
        } catch (UnmappedMemory& e) {
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
        }
    }
    // ── LL/SC helpers (bypass interpreter decode) ───────────────────
    // Called from JIT-compiled code when it does not inline the access (the
    // slow path of the native sequences, and configurations that force the
    // software guards). They follow the same protocol as the interpreter's
    // LDXR/STXR/STLR handlers: see System/Core/ExclusiveMonitor.hpp.
    //
    // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=width
    // Returns (LDXR): the loaded value in RAX.
    // Returns (STXR): 0=success, 1=failure in RAX.
    uint64_t jit_ldxr(Emulator* emu, CPU* cpu, uint64_t addr, int width) {
        bool exclusive = !(width & 0x100);
        width &= 0xFF;
        // The version is read before the value it pairs with.
        const uint64_t ver = exclusive ? g_exclusive_monitor.version(addr) : 0;
        uint64_t v = 0;
        try {
            emu->mem().read(addr, &v, width);
        } catch (UnmappedMemory& e) {
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
            return 0;
        }
        if (exclusive) cpu->excl_mark(addr, width, v, ver);
        return v;
    }
    // STXR: returns 0=success, 1=failure. val is in R8 (passed as 5th arg).
    uint64_t jit_stxr(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        ExclusiveMonitor::Hold hold(g_exclusive_monitor.bucket(addr));
        bool ok = cpu->excl_check(addr, width) && hold.version() == cpu->excl_ver;
        if (ok) {
            try {
                // Attribute the store to the current guest pc for BIFROST_WATCH
                // (the JIT normally bypasses Memory::write; forced-slow stores
                // should still carry a pc).
                Memory::note_interp_pc(cpu->pc);
                ok = emu->mem().compare_exchange(addr, cpu->excl_value, val, width);
                if (ok) hold.commit();
            } catch (UnmappedMemory& e) {
                int si_code = e.segv_code();
                force_signal(*emu, *cpu, emu->signals(), e.signo(),
                               si_code, e.addr);
                return 1;
            }
        }
        cpu->excl_clear();
        return ok ? 0 : 1;
    }
    // STLR: store-release (always succeeds, invalidates other CPUs).
    void jit_stlr(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width) {
        ExclusiveMonitor::Hold hold(g_exclusive_monitor.bucket(addr));
        try {
            Memory::note_interp_pc(cpu->pc);  // BIFROST_WRITE_TRACE attribution
            emu->mem().write(addr, &val, width);
            hold.commit();
        } catch (UnmappedMemory& e) {
            int si_code = e.segv_code();
            force_signal(*emu, *cpu, emu->signals(), e.signo(),
                           si_code, e.addr);
        }
    }
    // Slow path of the inline bucket lock (CodeGen/Integer/Atomics.cpp): the
    // `lock bts` found the lock held. Returns holding it.
    void jit_excl_lock_slow(std::atomic<uint64_t>* word) {
        for (unsigned spins = 0;; ) {
            if (!(word->fetch_or(1, std::memory_order_acquire) & 1)) return;
            while (word->load(std::memory_order_relaxed) & 1) {
                __builtin_ia32_pause();
                if (++spins >= 2048) { spins = 0; sched_yield(); }
            }
        }
    }
}
// ── Register allocator ──────────────────────────────────────────────────
// Maps vregs to x86 registers for the duration of a block. Vregs 0-30
// are arch GPRs (cpu.regs[]), 31=SP, 32=XZR, 33+ are scratch (stack
// slots pre-assigned -8*(v-32), lazy slots past that; chain-skip uses
// the unified 0x8000 frame).
//
// Allocator state:
//   vreg_home_[v] = x86 reg holding v, or -1 if not cached.
//   reg_vreg_[r]  = vreg currently in x86 reg r, or -1.
//   vreg_dirty_[v] = true if the cached value differs from cpu.regs[]/stack.
//
// Available x86 regs (10 total):
//   Caller-saved (clobbered by C calls): RAX, RCX, RDX, R8, R9, R11
//   Callee-saved (preserved by C calls): R12, R13, R14, R15
// Reserved: RBX=CPU, R10=window, RBP=frame, RSP=stack. Emu lives in the
// frame slot, not R14.
// constexpr int FrostJIT::ALLOC_REGS[] = {RAX,RCX,RDX,R8,R9,R11,R12,R13,R15,R14};
// (defined in FrostJIT.hpp; vregs cached in R12-R15 survive CALL_INTERP)
// ── emit_load_mem / emit_store_mem ─────────────────────────────────────
// Memory access through the guest reservation (R10 = its base) when the
// access lies inside it; falls back to the C helper (jit_load_mem_slow /
// jit_store_mem_slow, defined above) otherwise, which raises the fault.
// Move a 64-bit immediate into RAX. Uses the shorter mov imm32 + zext
// form when the value fits in 32 bits, otherwise the 10-byte mov imm64.
void FrostJIT::emit_mov_imm_to_rax(uint64_t val) {
    if (val <= 0xFFFFFFFFULL && !wide_guest_pc(val)) {
        emit_mov_imm32_zext(RAX, static_cast<uint32_t>(val));
    } else {
        emit_mov_imm64(RAX, val);
    }
}
void FrostJIT::emit_window_limit(int dst, uint64_t width) {
    const uint64_t size = guest_memory_ ? guest_memory_->window_size() : 0;
    const uint64_t limit = size > width ? size - width : 0;
    if (limit <= 0xFFFFFFFFULL) emit_mov_imm32_zext(dst, static_cast<uint32_t>(limit));
    else emit_mov_imm64(dst, limit);
}
void FrostJIT::emit_untag_data_address(int reg) {
    if (!data_tbi()) return;
    emit_shift_imm8(reg, 4, 8);  // shl reg, 8
    emit_shift_imm8(reg, 5, 8);  // shr reg, 8
}
int FrostJIT::emit_window_bound_cmp(int idx, uint64_t width, int scratch, bool gate) {
    if (guest_memory_ && width && width <= Memory::JIT_WINDOW_LIMIT_MAX_WIDTH) {
        // cmp r64, r/m64 (REX.W 3B /r): flags of idx - bound, as emit_cmp_reg.
        emit_byte(rex(true, idx >= 8, false, WIN_REG >= 8));
        emit_byte(0x3B);
        emit_modrm_disp(idx, WIN_REG, gate ? Memory::jit_window_limit_off(width)
                                           : Memory::jit_window_bound_off(width));
        return kJccAE;
    }
    emit_window_limit(scratch, width);
    emit_cmp_reg(idx, scratch);
    return kJccA;
}
void FrostJIT::emit_load_mem(int dst, int addr_reg, int32_t off, int w,
                             bool sign_ext, bool untagged) {
    // Indexed window addressing (2026-08-21): the guest address is used
    // DIRECTLY as the SIB index into [WIN_REG + idx + disp] — the offset
    // folds into the displacement, and the old base copy + `add WIN_REG`
    // are gone from the fast path. The window limit check must cover the
    // SUM addr+off (the old sequence added the offset before checking),
    // so for off != 0 the sum is materialized once into a scratch (LEA)
    // and THAT register is both the cmp operand and the SIB index;
    // off == 0 uses addr_reg itself as the index (never copied, never
    // destroyed — dst may even equal it, the load reads the index first).
    // Slow path: jit_load_mem_slow(emu, cpu, addr, width) with the
    // materialized sum in RDX.
    // idx = off ? lea-scratch : addr_reg; must differ from dst (the load
    // writes dst). lim = the limit constant register; must differ from idx.
    // Data TBI untags the sum in the scratch index too, never addr_reg.
    int idx = addr_reg;
    if (off != 0 || (data_tbi() && !untagged)) {
        idx = (dst != RDX && addr_reg != RDX) ? RDX
            : ((dst != RCX && addr_reg != RCX) ? RCX : R8);
        emit_byte(rex(true,idx>=8,false,addr_reg>=8)); // lea off(%addr),%idx
        emit_byte(0x8D);
        emit_modrm_disp(idx, addr_reg, off);
        if (!untagged) emit_untag_data_address(idx);
    }
    size_t jmp_past = 0;
    if (!memory_fault_stub_) {
        // The slow helper checks everything, so a closed gate only costs time.
        const int outside = emit_window_bound_cmp(idx, w, (idx != R9) ? R9 : ((idx != R8) ? R8 : RCX), true);
        size_t jbe_patch = emit_jcc_rel32_placeholder(outside ^ 1); // inside: JB or JBE
        // Slow path. RSP%16==8 at body entry; caller pushes R10 (1 push, ODD)
        // → emit_call_aligned handles the sub rsp,8 + pushfq + call + popfq +
        // add rsp,8 dance automatically.
        //
        // Args: RDI = emu, RSI = cpu, RDX = addr, RCX = width
        emit_push(WIN_REG);                  // 1 push — ODD, helper will sub rsp,8
        emit_load_emu(RDI); // rdi = emu
        emit_mov_reg(RSI, CPU_REG);          // rsi = cpu (for SIGSEGV delivery)
        if (idx != RDX) emit_mov_reg(RDX, idx); // rdx = addr (+off already in idx)
        emit_mov_imm32(RCX, w);              // rcx = width
        emit_call_aligned(&jit_load_mem_slow, /*num_pushed=*/1);
        emit_pop(WIN_REG);                   // restore R10
        // RAX now has the return value (the loaded data).
        if (dst != RAX) emit_mov_reg(dst, RAX);
        jmp_past = emit_jmp_rel32_placeholder();
        // Fast path: load [WIN_REG + idx + off?0] through the SIB index.
        // For off != 0 the sum already includes it — index with NO disp.
        // REX: W for 64-bit forms, R=dst, X=idx, B=WIN_REG (R10, always).
        int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
        patch_jcc_rel32(jbe_patch, fast_rel);
    }
    register_memory_fault_site();
    if (w == 8) {
        emit_byte(rex(true,dst>=8,idx>=8,true)); emit_byte(0x8B);
    } else if (w == 4) {
        if (sign_ext) { emit_byte(rex(true,dst>=8,idx>=8,true)); emit_byte(0x63); }
        else          { emit_byte(rex(false,dst>=8,idx>=8,true)); emit_byte(0x8B); }
    } else if (w == 2) {
        emit_byte(rex(false,dst>=8,idx>=8,true));
        if (sign_ext) { emit_byte(0x0F); emit_byte(0xBF); }
        else          { emit_byte(0x0F); emit_byte(0xB7); }
    } else if (w == 1) {
        emit_byte(rex(false,dst>=8,idx>=8,true));
        if (sign_ext) { emit_byte(0x0F); emit_byte(0xBE); }
        else          { emit_byte(0x0F); emit_byte(0xB6); }
    }
    emit_modrm_sib_win(dst, idx, 0);
    if (jmp_past) {
        int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
        patch_jmp_rel32(jmp_past, end_rel);
    }
}
void FrostJIT::emit_store_mem(int addr_reg, int32_t off, int src_reg, int w, bool untagged) {
    // Indexed window addressing (2026-08-21): the address (or its LEA sum
    // when off != 0) is used DIRECTLY as the SIB index into
    // [WIN_REG + idx] — no R8 copy, no `add WIN_REG`. addr_reg and src_reg
    // are BOTH preserved on every path (src pushed/popped around the slow
    // call; addr only read). The limit check covers the SUM addr+off,
    // matching the old add-first semantics. Contract: src_reg must not be
    // R8/R9 (the scratch pair), addr_reg must not be RDX.
    // idx = off ? lea-scratch : addr_reg.
    int idx = addr_reg;
    if (off != 0 || (data_tbi() && !untagged)) {
        idx = (addr_reg != R8) ? R8 : R9;
        emit_byte(rex(true,idx>=8,false,addr_reg>=8)); // lea off(%addr),%idx
        emit_byte(0x8D);
        emit_modrm_disp(idx, addr_reg, off);
        if (!untagged) emit_untag_data_address(idx);
    }
    // BIFROST_JIT_SLOW_STORES=1: route EVERY store through the C slow path
    // (jit_store_mem_slow → Memory::write) so BIFROST_WATCH can observe JIT
    // stores with a pc (diagnostic; very slow).
    static const bool slow_stores_ = getenv("BIFROST_JIT_SLOW_STORES") != nullptr;
    size_t jbe_patch = 0;
    if (!slow_stores_ && !memory_fault_stub_) {
        // Limit check: idx <= window_size - w (the slow helper checks the rest).
        const int outside = emit_window_bound_cmp(idx, w, (idx != R9) ? R9 : R8, true);
        jbe_patch = emit_jcc_rel32_placeholder(outside ^ 1);
    }
    size_t jmp_past = 0;
    if (!memory_fault_stub_ || slow_stores_) {
        // Slow path: call jit_store_mem_slow(emu, cpu, addr, val, width).
        // 2 pushes (src, R10) — EVEN, so emit_call_aligned needs no sub/add.
        //
        // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=val, R8=width
        emit_push(src_reg);            // save val — 1 push
        emit_push(WIN_REG);            // save R10      — 2 pushes (EVEN)
        emit_load_emu(RDI); // rdi = emu
        emit_mov_reg(RSI, CPU_REG);    // rsi = cpu (for SIGSEGV delivery)
        if (idx != RDX) emit_mov_reg(RDX, idx); // rdx = addr (sum in idx)
        if (src_reg != RCX) emit_mov_reg(RCX, src_reg);  // rcx = val
        emit_mov_imm32(R8, w);         // r8 = width (after idx → RDX)
        emit_call_aligned(&jit_store_mem_slow, /*num_pushed=*/2);
        emit_pop(WIN_REG);             // restore R10
        emit_pop(src_reg);             // restore val
        if (slow_stores_) return;      // forced-slow: no fast path
        jmp_past = emit_jmp_rel32_placeholder();
        // Fast path: direct window store, indexed by the address (sum).
        int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
        patch_jcc_rel32(jbe_patch, fast_rel);
    }
    register_memory_fault_site();
    if (w == 8) {
        emit_byte(rex(true,src_reg>=8,idx>=8,true)); emit_byte(0x89);
    } else if (w == 4) {
        emit_byte(rex(false,src_reg>=8,idx>=8,true)); emit_byte(0x89);
    } else if (w == 2) {
        emit_byte(0x66);
        emit_byte(rex(false,src_reg>=8,idx>=8,true)); emit_byte(0x89);
    } else if (w == 1) {
        emit_byte(rex(false,src_reg>=8,idx>=8,true)); emit_byte(0x88);
    }
    emit_modrm_sib_win(src_reg, idx, 0);
    if (jmp_past) {
        int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
        patch_jmp_rel32(jmp_past, end_rel);
    }
}
} // namespace arm64emu
