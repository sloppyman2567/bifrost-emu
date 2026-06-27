// ir/ir.h — internal IR helpers shared by the translator, lowerer, and
// optimizer. NOT a public header — only included from src/ir/*.cpp.
//
// Public IR types live in include/ir/ir.hpp.
#pragma once

#include "ir/ir.hpp"

#include <cstdint>

namespace arm64emu {

// ── Scratch vreg allocator ──────────────────────────────────────────────
// Per-block resetable. The translator allocates a fresh vreg for every
// intermediate value; the optimizer later reuses them.
//
// widened from uint8_t to uint16_t to prevent
// wrap-around. Arch regs use 0-32, scratch starts at 33. With uint8_t,
// large blocks (82+ ARM instructions) exhausted the 256-vreg space and
// wrapped to 0, colliding with arch regs (vreg 31 = SP).
struct VregAlloc {
    uint16_t next = 33;
    uint16_t alloc() { return next++; }
    void reset() { next = 33; }
};

// Per-thread allocator (each JIT thread has its own to avoid locking).
extern thread_local VregAlloc g_alloc;

// Reset the per-block allocator. Called at the start of each block.
void ir_reset_vreg_alloc();

// ── IR emit helpers ─────────────────────────────────────────────────────
// Emit a single IR op.
inline void emit(IRBlock& b, IROp op, uint16_t dest = 0,
                 uint16_t src1 = 0, uint16_t src2 = 0,
                 uint8_t width = 0, uint8_t cond = 0,
                 uint8_t flags_op = 0, uint64_t imm = 0,
                 uint64_t arm_pc = 0) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.width = width;
    inst.cond = cond;
    inst.flags_op = flags_op;
    inst.imm = imm;
    inst.arm_pc = arm_pc;
    b.insts.push_back(inst);
}

// Emit an IRInst with the extra bitfield fields (immr/imms/sf).
inline void emit_bf(IRBlock& b, IROp op, uint16_t dest,
                    uint16_t src1, uint16_t src2,
                    uint8_t immr, uint8_t imms, uint8_t sf,
                    uint64_t arm_pc) {
    IRInst inst{};
    inst.op = op;
    inst.dest = dest;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.immr = immr;
    inst.imms = imms;
    inst.sf = sf;
    inst.arm_pc = arm_pc;
    b.insts.push_back(inst);
}

// Emit an immediate into a fresh vreg.
inline uint16_t load_imm(IRBlock& b, uint64_t val) {
    uint16_t v = g_alloc.alloc();
    emit(b, IROp::IMM, v, 0, 0, 0, 0, 0, val);
    return v;
}

// Read an ARM64 reg into a fresh vreg.
// `is_sp` controls the reg-31 mapping:
//   - For ADD/SUB immediate: reg 31 = SP (is_sp=true)
//   - For load/store data regs: reg 31 = XZR (is_sp=false)
//   - For data processing (logical/shift): reg 31 = XZR (is_sp=false)
inline uint16_t load_arm_reg(IRBlock& b, uint8_t ar, bool is_sp = false) {
    if (ar == 31 && !is_sp) ar = 32;  // XZR
    // XZR (vreg 32) is always 0 — emit IMM 0 instead of LOAD_REG.
    // Eliminates a memory read and lets the constant folder propagate it.
    if (ar == 32) {
        return load_imm(b, 0);
    }
    uint16_t v = g_alloc.alloc();
    emit(b, IROp::LOAD_REG, v, ar);
    return v;
}

// Write a vreg to an ARM64 reg.
// `is_sp` controls reg-31 mapping (same as load_arm_reg).
inline void store_arm_reg(IRBlock& b, uint8_t ar, uint8_t v, bool is_sp = false) {
    if (ar == 31 && !is_sp) return;  // XZR — discard
    emit(b, IROp::STORE_REG, ar, v);
}

// Zero-extend a value to 32 bits (sf=0) or pass-through (sf=1).
// We always emit the op; the optimizer peephole removes redundant ZEXTs
// after ops whose x86 encoding already zero-extends.
inline uint16_t zext_if_32bit(IRBlock& b, uint16_t v, bool sf) {
    if (sf) return v;
    uint16_t r = g_alloc.alloc();
    emit(b, IROp::ZEXT, r, v, 0, 32);
    return r;
}

// Apply an extend operation (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) to
// a vreg, returning a new vreg holding the result. Used by ADD_REG/SUB_REG,
// ADDS_REG/SUBS_REG, and LDR_REG/STR_REG for the extended-register form.
// extend: bits[2:0] = extend type (0=UXTB..7=SXTX).
// shift:  shift amount to apply after extend (0-4 for extended form).
inline uint16_t apply_extend(IRBlock& b, uint16_t v, uint8_t extend, uint8_t shift) {
    switch (extend & 7) {
        case 0: { // UXTB
            uint16_t m = load_imm(b, 0xFF);
            uint16_t r = g_alloc.alloc();
            emit(b, IROp::AND, r, v, m);
            v = r;
            break;
        }
        case 1: { // UXTH
            uint16_t m = load_imm(b, 0xFFFF);
            uint16_t r = g_alloc.alloc();
            emit(b, IROp::AND, r, v, m);
            v = r;
            break;
        }
        case 2: { // UXTW
            uint16_t m = load_imm(b, 0xFFFFFFFF);
            uint16_t r = g_alloc.alloc();
            emit(b, IROp::AND, r, v, m);
            v = r;
            break;
        }
        case 3: break; // UXTX — no extend
        case 4: { // SXTB
            uint16_t s = g_alloc.alloc();
            emit(b, IROp::SEXT, s, v, 0, 8);
            v = s;
            break;
        }
        case 5: { // SXTH
            uint16_t s = g_alloc.alloc();
            emit(b, IROp::SEXT, s, v, 0, 16);
            v = s;
            break;
        }
        case 6: { // SXTW
            uint16_t s = g_alloc.alloc();
            emit(b, IROp::SEXT, s, v, 0, 32);
            v = s;
            break;
        }
        case 7: break; // SXTX — no extend
    }
    if (shift != 0) {
        uint16_t sh = load_imm(b, static_cast<uint64_t>(shift));
        uint16_t shifted = g_alloc.alloc();
        emit(b, IROp::SHL, shifted, v, sh);
        v = shifted;
    }
    return v;
}

// Apply a shift-type operation (LSL/LSR/ASR/ROR) to a vreg.
// shift_type: 0=LSL, 1=LSR, 2=ASR, 3=ROR.
// shift:       shift amount.
// sf:          0 = 32-bit, 1 = 64-bit. For 32-bit ASR, the operand is
//              sign-extended from bit 31 before the SAR, because the
//              JIT's SAR uses x86's 64-bit sar which looks at bit 63.
inline uint16_t apply_shift(IRBlock& b, uint16_t v, uint8_t shift_type, uint8_t shift, bool sf = true) {
    if (shift == 0 && shift_type == 0) return v;
    // 32-bit ASR: sign-extend first so the 64-bit SAR sees the sign bit.
    if (shift_type == 2 && !sf) {
        uint16_t sext = g_alloc.alloc();
        emit(b, IROp::SEXT, sext, v, 0, 32);
        v = sext;
    }
    uint16_t sh = load_imm(b, static_cast<uint64_t>(shift));
    uint16_t shifted = g_alloc.alloc();
    IROp shop = (shift_type == 0) ? IROp::SHL
              : (shift_type == 1) ? IROp::SHR
              : (shift_type == 2) ? IROp::SAR
              : IROp::ROR;
    // For 32-bit ROR: set width=32 so the JIT uses a 32-bit rotation
    // (64-bit ROR on a zero-extended 32-bit value loses wrap bits).
    uint8_t width = (shift_type == 3 && !sf) ? 32 : 0;
    emit(b, shop, shifted, v, sh, width);
    return shifted;
}

// ── SWAR lowering helpers (defined in ir_lower.cpp) ─────────────────────
// Decompose RBIT/REV16/REV32 into primitive IR ops (AND/OR/SHL/SHR) so
// the JIT can compile them natively instead of falling back to CALL_INTERP.
uint16_t swar_swap(IRBlock& b, uint16_t v, uint64_t mask, int n);
uint16_t rbit64_ir(IRBlock& b, uint16_t v);
uint16_t rbit32_ir(IRBlock& b, uint16_t v);
uint16_t rev16_64_ir(IRBlock& b, uint16_t v);
uint16_t rev32_64_ir(IRBlock& b, uint16_t v);

} // namespace arm64emu
