// ir.cpp — ARM64 → IR translator for bifrost-emu (v1.4.0-alpha.3)
//
// Translates each ARM64 instruction (DecodedInst) into 1+ IR micro-ops.
// The IR is then optimized (optimize_ir) and either executed by ops.cpp
// (debug) or compiled to native x86-64 code by frostjit.cpp (the JIT).
//
// Vreg mapping:
//   0-30  = ARM64 X0-X30
//   31    = SP
//   32    = XZR (always 0, writes discarded)
//   33+   = scratch temporaries (allocated per-block, reset every block)
//
// Translation rules:
//   - Each ARM64 instruction emits IR ops whose net effect on the
//     architectural state (cpu.regs[], cpu.sp, cpu.pstate, memory)
//     matches the ARM64 semantics.
//   - Side-effecting ops (LOAD_MEM, STORE_MEM, CALL_INTERP, SVC, BR*)
//     are not optimized away.
//   - The translator never reads or writes cpu.regs[] directly — it
//     emits LOAD_REG / STORE_REG ops. This is what lets the optimizer
//     cache values across instructions.
//   - 32-bit ARM64 ops (sf=0) emit an explicit ZEXT after the ALU op
//     so the high 32 bits are zeroed. The optimizer peephole removes
//     redundant ZEXTs after ops that already zero-extend (ADD with
//     32-bit dest on x86, etc.) when generating x86.

#include "ir.hpp"
#include "arm64_emu.hpp"  // for cond_true() (used by executor only)

namespace arm64emu {

// ── Scratch vreg allocator ──────────────────────────────────────────────
// Per-block resetable. The translator allocates a fresh vreg for every
// intermediate value; the optimizer later reuses them.
//
// (v1.4.0-alpha.5): widened from uint8_t to uint16_t to prevent
// wrap-around. Arch regs use 0-32, scratch starts at 33. With uint8_t,
// large blocks (82+ ARM instructions) exhausted the 256-vreg space and
// wrapped to 0, colliding with arch regs (vreg 31 = SP). This caused
// test_float's "decode error at pc=0x0" crash. uint16_t gives 65535
// vregs — effectively unlimited.
struct VregAlloc {
    uint16_t next = 33;
    uint16_t alloc() { return next++; }
    void reset() { next = 33; }
};

static thread_local VregAlloc g_alloc;

// Reset the per-block allocator. Called at the start of each block.
void ir_reset_vreg_alloc() { g_alloc.reset(); }

// Helper: emit a single IR op.
static inline void emit(IRBlock& b, IROp op, uint16_t dest = 0,
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

// Helper: emit an IRInst with the extra bitfield fields.
static inline void emit_bf(IRBlock& b, IROp op, uint16_t dest,
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

// Helper: emit an immediate into a fresh vreg.
static inline uint16_t load_imm(IRBlock& b, uint64_t val) {
    uint16_t v = g_alloc.alloc();
    emit(b, IROp::IMM, v, 0, 0, 0, 0, 0, val);
    return v;
}

// Helper: read an ARM64 reg into a fresh vreg.
// `is_sp` controls the reg-31 mapping:
//   - For ADD/SUB immediate: reg 31 = SP (is_sp=true)
//   - For load/store data regs: reg 31 = XZR (is_sp=false)
//   - For data processing (logical/shift): reg 31 = XZR (is_sp=false)
static inline uint16_t load_arm_reg(IRBlock& b, uint8_t ar, bool is_sp = false) {
    if (ar == 31 && !is_sp) ar = 32;  // XZR
    // (v1.4.0-alpha.5): XZR (vreg 32) is always 0 — emit IMM 0 instead
    // of LOAD_REG. This eliminates a memory read and lets the constant
    // folder propagate it.
    if (ar == 32) {
        return load_imm(b, 0);
    }
    uint16_t v = g_alloc.alloc();
    emit(b, IROp::LOAD_REG, v, ar);
    return v;
}

// Helper: write a vreg to an ARM64 reg.
// `is_sp` controls reg-31 mapping (same as load_arm_reg).
static inline void store_arm_reg(IRBlock& b, uint8_t ar, uint8_t v, bool is_sp = false) {
    if (ar == 31 && !is_sp) return;  // XZR — discard
    emit(b, IROp::STORE_REG, ar, v);
}

// Helper: zero-extend a value to 32 bits (sf=0) or pass-through (sf=1).
// We always emit the op; the optimizer peephole removes redundant ZEXTs
// after ops whose x86 encoding already zero-extends.
static inline uint16_t zext_if_32bit(IRBlock& b, uint16_t v, bool sf) {
    if (sf) return v;
    uint16_t r = g_alloc.alloc();
    emit(b, IROp::ZEXT, r, v, 0, 32);
    return r;
}

// ── Translator ──────────────────────────────────────────────────────────
bool translate_to_ir(IRBlock& block, const DecodedInst& d, uint64_t cur_pc) {
    switch (d.cls) {
        case InstClass::HINT:
            emit(block, IROp::NOP);
            return false;

        // ── MOVZ / MOVN / MOVK ───────────────────────────────────────
        case InstClass::MOVZ: {
            uint64_t val = (uint64_t)d.imm16 << (d.hw * 16);
            if (!d.sf) val &= 0xFFFFFFFF;
            uint16_t v = load_imm(block, val);
            store_arm_reg(block, d.rd, v);
            return false;
        }
        case InstClass::MOVN: {
            uint64_t val = ~(uint64_t)((uint64_t)d.imm16 << (d.hw * 16));
            if (!d.sf) val &= 0xFFFFFFFF;
            uint16_t v = load_imm(block, val);
            store_arm_reg(block, d.rd, v);
            return false;
        }
        case InstClass::MOVK: {
            uint16_t cur = load_arm_reg(block, d.rd);
            uint64_t mask = ~((uint64_t)0xFFFF << (d.hw * 16));
            if (!d.sf) mask |= 0xFFFFFFFF00000000ULL;
            uint16_t mv = load_imm(block, mask);
            uint16_t bits = load_imm(block, (uint64_t)d.imm16 << (d.hw * 16));
            uint16_t masked = g_alloc.alloc();
            emit(block, IROp::AND, masked, cur, mv);
            uint16_t result = g_alloc.alloc();
            emit(block, IROp::OR, result, masked, bits);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── ADD/SUB (register, immediate) ────────────────────────────
        case InstClass::ADD_REG: case InstClass::ADD_IMM:
        case InstClass::SUB_REG: case InstClass::SUB_IMM: {
            // For immediate AND extended-register forms, rn=31/rd=31 can
            // mean SP (the decoder sets d.reads_sp/d.writes_sp). For the
            // shifted-register form (bit21=0), rd=31 means XZR (the
            // decoder leaves d.writes_sp=false).
            //
            // BUGFIX (alpha.4): the old code only checked ADD_IMM/SUB_IMM
            // for SP mapping. ADD_REG/SUB_REG (extended register form)
            // was excluded, so `add sp, sp, x12` (very common in
            // function epilogues) was computed as `add xzr, xzr, x12`
            // and the result was discarded. This corrupted the stack
            // pointer, causing crashes in musl's mallocng during free()
            // (get_meta would see a non-16-byte-aligned pointer because
            // SP was never restored after the function's stack frame
            // allocation).
            bool rn_is_sp = (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM ||
                             d.cls == InstClass::ADD_REG || d.cls == InstClass::SUB_REG) && d.reads_sp;
            uint16_t a = load_arm_reg(block, d.rn, rn_is_sp);
            uint8_t b;
            if (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM) {
                // BUGFIX (alpha.4): the decoder sets d.imm_u to the raw
                // 12-bit immediate and d.shift to 0 or 12 (the optional
                // left-shift by 12 for the "add/sub imm, lsl #12" form).
                // The previous code passed d.imm_u through unshifted, so
                // e.g. `add x0, x0, #0x1, lsl #12` (= add x0, x0, #0x1000)
                // was computed as `add x0, x0, #1`. This broke musl's
                // `cmn x0, #0x1, lsl #12` in __syscall_ret — the function
                // uses it to test whether a syscall return value is in
                // the [-4095, -1] error range. Without the shift, every
                // successful syscall looked like an error, and every
                // syscall was reported as -1 with bogus errno.
                b = load_imm(block, (uint64_t)d.imm_u << d.shift);
            } else {
                // Register form. Two sub-cases:
                //   (a) Extended register (bit21=1): apply extend type
                //       (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) then
                //       optional shift (0-4).
                //   (b) Shifted register (bit21=0): apply shift_type
                //       (LSL/LSR/ASR/ROR) by d.shift (0-63).
                // The decoder sets d.extend for case (a) and d.shift_type
                // for case (b). We must NOT apply extend logic in case (b)
                // even though d.shift != 0 — the shift there is a shifted-
                // register shift, not an extend shift.
                b = load_arm_reg(block, d.rm);
                if (d.extend != 0) {
                    // Extended register form — apply extend, then shift.
                    switch (d.extend & 7) {
                        case 0: { // UXTB
                            uint16_t m = load_imm(block, 0xFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 1: { // UXTH
                            uint16_t m = load_imm(block, 0xFFFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 2: { // UXTW
                            uint16_t m = load_imm(block, 0xFFFFFFFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 3: break; // UXTX — no extend
                        case 4: { // SXTB
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 8);
                            b = s;
                            break;
                        }
                        case 5: { // SXTH
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 16);
                            b = s;
                            break;
                        }
                        case 6: { // SXTW
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 32);
                            b = s;
                            break;
                        }
                        case 7: break; // SXTX — no extend
                    }
                    // Apply shift (for extended register, shift is 0-4).
                    if (d.shift != 0) {
                        uint16_t sh = load_imm(block, (uint64_t)d.shift);
                        uint16_t shifted = g_alloc.alloc();
                        emit(block, IROp::SHL, shifted, b, sh);
                        b = shifted;
                    }
                } else if (d.shift != 0 || d.shift_type != 0) {
                    // Shifted register form — apply shift_type by d.shift.
                    uint16_t sh = load_imm(block, (uint64_t)d.shift);
                    uint16_t shifted = g_alloc.alloc();
                    IROp shop = (d.shift_type == 0) ? IROp::SHL
                              : (d.shift_type == 1) ? IROp::SHR
                              : (d.shift_type == 2) ? IROp::SAR
                              : IROp::ROR;
                    emit(block, shop, shifted, b, sh);
                    b = shifted;
                }
            }
            IROp op = (d.cls == InstClass::ADD_REG || d.cls == InstClass::ADD_IMM)
                      ? IROp::ADD : IROp::SUB;
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, b);
            r = zext_if_32bit(block, r, d.sf);
            // For immediate AND extended-register forms, rd=31 writes SP
            // (when !set_flags). The decoder sets d.writes_sp accordingly.
            bool rd_is_sp = (d.cls == InstClass::ADD_IMM || d.cls == InstClass::SUB_IMM ||
                             d.cls == InstClass::ADD_REG || d.cls == InstClass::SUB_REG) && d.writes_sp;
            store_arm_reg(block, d.rd, r, rd_is_sp);
            return false;
        }

        // ── ADDS/SUBS (flag-setting) ─────────────────────────────────
        case InstClass::ADDS_REG: case InstClass::ADDS_IMM:
        case InstClass::SUBS_REG: case InstClass::SUBS_IMM: {
            // For immediate forms with !set_flags, rn=31 reads SP.
            // But ADDS/SUBS always set flags (set_flags=true), so rn=31 = XZR.
            // (CMP = SUBS XZR — rn=31 = XZR here too.)
            uint16_t a = load_arm_reg(block, d.rn, false);
            uint8_t b;
            if (d.cls == InstClass::ADDS_IMM || d.cls == InstClass::SUBS_IMM) {
                // BUGFIX (alpha.4): apply d.shift (0 or 12) to d.imm_u,
                // matching the interpreter. See ADD_IMM/SUB_IMM above
                // for the full rationale.
                b = load_imm(block, (uint64_t)d.imm_u << d.shift);
            } else {
                // Register form. Two sub-cases (mirrors ADD_REG/SUB_REG):
                //   (a) Extended register (bit21=1): apply extend type
                //       (UXTB/SXTB/UXTH/SXTH/UXTW/SXTW/UXTX/SXTX) then
                //       optional shift (0-4).
                //   (b) Shifted register (bit21=0): apply shift_type
                //       (LSL/LSR/ASR/ROR) by d.shift (0-63).
                //
                // BUGFIX (alpha.4): the previous code ignored d.extend
                // and d.shift_type for ADDS/SUBS, so e.g.
                //   cmp x0, w24, sxtw
                // was computed as `x0 - w24` (treating w24 as unsigned
                // 32-bit, NOT sign-extended). This caused `csel x24, x0,
                //   x1, ge` in musl's vfprintf %d-zero handling to pick
                //   the wrong source, producing "" instead of "0" for
                //   printf("%d", 0). Many other CSEL-after-CMP paths in
                //   musl were similarly broken.
                b = load_arm_reg(block, d.rm);
                if (d.extend != 0) {
                    // Extended register form — apply extend, then shift.
                    switch (d.extend & 7) {
                        case 0: { // UXTB
                            uint16_t m = load_imm(block, 0xFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 1: { // UXTH
                            uint16_t m = load_imm(block, 0xFFFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 2: { // UXTW
                            uint16_t m = load_imm(block, 0xFFFFFFFF);
                            uint16_t r = g_alloc.alloc();
                            emit(block, IROp::AND, r, b, m);
                            b = r;
                            break;
                        }
                        case 3: break; // UXTX — no extend
                        case 4: { // SXTB
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 8);
                            b = s;
                            break;
                        }
                        case 5: { // SXTH
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 16);
                            b = s;
                            break;
                        }
                        case 6: { // SXTW
                            uint16_t s = g_alloc.alloc();
                            emit(block, IROp::SEXT, s, b, 0, 32);
                            b = s;
                            break;
                        }
                        case 7: break; // SXTX — no extend
                    }
                    // Apply shift (for extended register, shift is 0-4).
                    if (d.shift != 0) {
                        uint16_t sh = load_imm(block, (uint64_t)d.shift);
                        uint16_t shifted = g_alloc.alloc();
                        emit(block, IROp::SHL, shifted, b, sh);
                        b = shifted;
                    }
                } else if (d.shift != 0 || d.shift_type != 0) {
                    // Shifted register form — apply shift_type by d.shift.
                    uint16_t sh = load_imm(block, (uint64_t)d.shift);
                    uint16_t shifted = g_alloc.alloc();
                    IROp shop = (d.shift_type == 0) ? IROp::SHL
                              : (d.shift_type == 1) ? IROp::SHR
                              : (d.shift_type == 2) ? IROp::SAR
                              : IROp::ROR;
                    emit(block, shop, shifted, b, sh);
                    b = shifted;
                }
            }
            bool is_add = (d.cls == InstClass::ADDS_REG || d.cls == InstClass::ADDS_IMM);
            uint16_t r = g_alloc.alloc();
            emit(block, is_add ? IROp::ADDS : IROp::SUBS, r, a, b,
                 d.sf ? 64 : 32, 0, is_add ? 0 : 1);  // width = 32 or 64
            // CMP (SUBS XZR, ...) doesn't write Rd.
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── ADC/ADCS/SBC/SBCS (with carry) ───────────────────────────
        case InstClass::ADC_REG: case InstClass::ADCS_REG:
        case InstClass::SBC_REG: case InstClass::SBCS_REG: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t b = load_arm_reg(block, d.rm);
            bool is_sub = (d.cls == InstClass::SBC_REG || d.cls == InstClass::SBCS_REG);
            bool set_flags = (d.cls == InstClass::ADCS_REG || d.cls == InstClass::SBCS_REG);
            uint16_t r = g_alloc.alloc();
            if (set_flags) {
                emit(block, is_sub ? IROp::SBCS : IROp::ADCS, r, a, b,
                     0, 0, is_sub ? 1 : 0);
            } else {
                // No-flag form: emulate as a + b + C (or a - b - 1 + C).
                // We model this with a CALL_INTERP to keep the IR small.
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── AND/ORR/EOR/ANDS (register, immediate) ──────────────────
        // The register form also covers BIC/ORN/EON/BICS via the N bit
        // (bit 21). When N=1 the second operand is inverted before the
        // logical op. The IR translator must apply both the optional
        // shift (d.shift_type, d.shift) and the optional inversion (d.N)
        // to the register operand. Immediate forms have neither — d.N
        // there is part of the bitmask immediate encoding, not an invert.
        case InstClass::AND_REG: case InstClass::AND_IMM:
        case InstClass::ORR_REG: case InstClass::ORR_IMM:
        case InstClass::EOR_REG: case InstClass::EOR_IMM:
        case InstClass::ANDS_REG: case InstClass::ANDS_IMM: {
            uint16_t a = load_arm_reg(block, (d.rn == 31) ? 32 : d.rn);
            uint8_t b;
            bool is_imm = (d.cls == InstClass::AND_IMM || d.cls == InstClass::ORR_IMM ||
                           d.cls == InstClass::EOR_IMM || d.cls == InstClass::ANDS_IMM);
            if (is_imm) {
                b = load_imm(block, d.imm_u);
            } else {
                b = load_arm_reg(block, (d.rm == 31) ? 32 : d.rm);
                // Apply shift to the register operand.
                if (d.shift != 0 || d.shift_type != 0) {
                    uint16_t shift_amt = load_imm(block, (uint64_t)d.shift);
                    uint16_t shifted = g_alloc.alloc();
                    IROp shop = (d.shift_type == 0) ? IROp::SHL
                              : (d.shift_type == 1) ? IROp::SHR
                              : (d.shift_type == 2) ? IROp::SAR
                              : IROp::ROR;
                    emit(block, shop, shifted, b, shift_amt);
                    b = shifted;
                }
                // N=1 inverts the register operand (BIC/ORN/EON/BICS).
                if (d.N) {
                    uint16_t inverted = g_alloc.alloc();
                    emit(block, IROp::NOT, inverted, b);
                    b = inverted;
                }
            }
            IROp op;
            bool is_ands = false;
            switch (d.cls) {
                case InstClass::AND_REG: case InstClass::AND_IMM: op = IROp::AND; break;
                case InstClass::ORR_REG: case InstClass::ORR_IMM: op = IROp::OR;  break;
                case InstClass::EOR_REG: case InstClass::EOR_IMM: op = IROp::XOR; break;
                case InstClass::ANDS_REG: case InstClass::ANDS_IMM: op = IROp::AND; is_ands = true; break;
                default: op = IROp::AND; break;
            }
            if (is_ands) {
                // TST sets flags from a & b. ANDS also writes Rd.
                emit(block, IROp::TST, 0, a, b);
            }
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, b);
            if (d.rd != 31) {
                r = zext_if_32bit(block, r, d.sf);
                store_arm_reg(block, d.rd, r);
            }
            return false;
        }

        // ── MADD / MSUB ──────────────────────────────────────────────
        case InstClass::MADD: case InstClass::MSUB: {
            uint16_t rn = load_arm_reg(block, (d.rn == 31) ? 32 : d.rn);
            uint16_t rm = load_arm_reg(block, (d.rm == 31) ? 32 : d.rm);
            uint16_t prod = g_alloc.alloc();
            emit(block, IROp::MUL, prod, rn, rm);
            uint16_t ra = load_arm_reg(block, (d.ra == 31) ? 32 : d.ra);
            uint16_t result = g_alloc.alloc();
            emit(block, d.cls == InstClass::MADD ? IROp::ADD : IROp::SUB,
                 result, ra, prod);
            result = zext_if_32bit(block, result, d.sf);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── LSL/LSR/ASR/ROR (register) ───────────────────────────────
        case InstClass::LSL: case InstClass::LSR:
        case InstClass::ASR: case InstClass::ROR: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint16_t s = load_arm_reg(block, d.rm);
            IROp op;
            switch (d.cls) {
                case InstClass::LSL: op = IROp::SHL; break;
                case InstClass::LSR: op = IROp::SHR; break;
                case InstClass::ASR: op = IROp::SAR; break;
                case InstClass::ROR: op = IROp::ROR; break;
                default: op = IROp::SHL; break;
            }
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, a, s);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SBFM/UBFM/BFM/EXTR (bitfield) ────────────────────────────
        // Decoded by the decoder into immr/imms; we pass them through to
        // the IR executor / codegen which compute the actual mask.
        case InstClass::SBFM: case InstClass::UBFM: case InstClass::BFM:
        case InstClass::EXTR: {
            uint16_t a = load_arm_reg(block, d.rn);
            uint8_t b = (d.cls == InstClass::BFM || d.cls == InstClass::EXTR)
                        ? load_arm_reg(block, d.rm) : 0;
            IROp op = (d.cls == InstClass::SBFM) ? IROp::SBFM
                    : (d.cls == InstClass::UBFM) ? IROp::UBFM
                    : (d.cls == InstClass::BFM)  ? IROp::BFM
                    : IROp::EXTR;
            emit_bf(block, op, d.rd, a, b, d.immr, d.imms, d.sf ? 1 : 0, cur_pc);
            return false;
        }

        // ── CSEL / CSINC / CSINV / CSNEG ─────────────────────────────
        case InstClass::CSEL: case InstClass::CSINC:
        case InstClass::CSINV: case InstClass::CSNEG: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rm_v = load_arm_reg(block, d.rm);
            IROp op = (d.cls == InstClass::CSEL)  ? IROp::CSEL
                    : (d.cls == InstClass::CSINC) ? IROp::CSINC
                    : (d.cls == InstClass::CSINV) ? IROp::CSINV
                    : IROp::CSNEG;
            uint16_t r = g_alloc.alloc();
            emit(block, op, r, rn_v, rm_v, 0, d.cond, 0, d.rd, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── CCMP / CCMN ──────────────────────────────────────────────
        case InstClass::CCMP: case InstClass::CCMN: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint8_t rm_v;
            if (d.is_register) {
                rm_v = load_arm_reg(block, d.rm);
            } else {
                rm_v = load_imm(block, d.imm_u);
            }
            bool is_sub = (d.cls == InstClass::CCMP);
            // CCMP: if cond then set flags from rn - rm else set imm nzcv.
            emit(block, IROp::CCMP, 0, rn_v, rm_v, d.nzcv_field,
                 d.cond, is_sub ? 1 : 0, 0, cur_pc);
            return false;
        }

        // ── 1-source data processing: CLZ/CLS/RBIT/REV* ──────────────
        case InstClass::CLZ: case InstClass::CLS:
        case InstClass::RBIT: case InstClass::REV16:
        case InstClass::REV32: case InstClass::REV: {
            uint16_t a = load_arm_reg(block, d.rn);
            IROp op = (d.cls == InstClass::CLZ)   ? IROp::CLZ
                    : (d.cls == InstClass::CLS)   ? IROp::CLS
                    : (d.cls == InstClass::RBIT)  ? IROp::RBIT
                    : (d.cls == InstClass::REV16) ? IROp::REV16
                    : (d.cls == InstClass::REV32) ? IROp::REV32
                    : IROp::REV64;
            uint16_t r = g_alloc.alloc();
            // Store rd (in imm) and arm_pc for JIT fallback to CALL_INTERP.
            // Pass width=32 for 32-bit ops (sf=0) for CLZ 32-bit handling.
            emit(block, op, r, a, 0, d.sf ? 64 : 32, 0, 0, d.rd, cur_pc);
            r = zext_if_32bit(block, r, d.sf);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── UDIV / SDIV ──────────────────────────────────────────────
        case InstClass::UDIV: case InstClass::SDIV: {
            // Division is rare; fall back to the interpreter to keep
            // the IR codegen small. Block does not split — the
            // interpreter call is inline.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── LDR/STR (all forms) ──────────────────────────────────────
        case InstClass::LDR_IMM: case InstClass::LDR_UNS: case InstClass::LDR_REG:
        case InstClass::LDRSW: case InstClass::LDRSB: case InstClass::LDRSH:
        case InstClass::STR_IMM: case InstClass::STR_UNS: case InstClass::STR_REG: {
            bool is_load = (d.cls == InstClass::LDR_IMM || d.cls == InstClass::LDR_UNS ||
                            d.cls == InstClass::LDR_REG || d.cls == InstClass::LDRSW ||
                            d.cls == InstClass::LDRSB || d.cls == InstClass::LDRSH);
            int width = 1 << d.size;
            uint16_t base = load_arm_reg(block, d.rn, true);  // base reg 31 = SP
            // ── Compute the load/store address ──
            // For post-index (mode=1): load/store from `base`, writeback `base + disp`.
            // For pre-index  (mode=2): load/store from `base + disp`, writeback `base + disp`.
            // For offset     (mode=0): load/store from `base + disp`, no writeback.
            uint8_t addr;
            bool post_index = (d.mode == 1 && d.writeback);
            if (d.cls == InstClass::LDR_REG || d.cls == InstClass::STR_REG) {
                // Register offset: addr = base + extend_reg(rm, option, S ? size : 0)
                uint16_t idx = load_arm_reg(block, d.rm);
                uint8_t ext = idx;
                switch (d.extend & 7) {
                    case 0: { // UXTB
                        uint16_t m = load_imm(block, 0xFF);
                        ext = g_alloc.alloc();
                        emit(block, IROp::AND, ext, idx, m);
                        break;
                    }
                    case 1: { // UXTH
                        uint16_t m = load_imm(block, 0xFFFF);
                        ext = g_alloc.alloc();
                        emit(block, IROp::AND, ext, idx, m);
                        break;
                    }
                    case 2: { // UXTW
                        uint16_t m = load_imm(block, 0xFFFFFFFF);
                        ext = g_alloc.alloc();
                        emit(block, IROp::AND, ext, idx, m);
                        break;
                    }
                    case 3: // UXTX — no extend
                        break;
                    case 4: { // SXTB
                        uint16_t s = g_alloc.alloc();
                        emit(block, IROp::SEXT, s, idx, 0, 8);
                        ext = s;
                        break;
                    }
                    case 5: { // SXTH
                        uint16_t s = g_alloc.alloc();
                        emit(block, IROp::SEXT, s, idx, 0, 16);
                        ext = s;
                        break;
                    }
                    case 6: { // SXTW
                        uint16_t s = g_alloc.alloc();
                        emit(block, IROp::SEXT, s, idx, 0, 32);
                        ext = s;
                        break;
                    }
                    case 7: // SXTX — no extend
                        break;
                }
                if (d.shift & 1) {
                    uint16_t sh = load_imm(block, (uint64_t)d.size);
                    uint16_t shifted = g_alloc.alloc();
                    emit(block, IROp::SHL, shifted, ext, sh);
                    ext = shifted;
                }
                addr = g_alloc.alloc();
                emit(block, IROp::ADD, addr, base, ext);
            } else if (post_index) {
                // Post-index: load/store from base (no offset).
                addr = base;
            } else {
                // (v1.4.0-alpha.3 optimization): for offset and pre-index
                // modes, fold the displacement into the LOAD_MEM/STORE_MEM
                // imm field instead of emitting a separate IMM+ADD. The
                // executor and JIT both handle `mem[base + imm]` directly.
                // This cuts 2 IR ops per load/store and lets the optimizer
                // skip the address computation entirely.
                addr = base;
            }
            // Compute the mem op's immediate offset.
            // For post-index: 0 (load from base, writeback handles disp).
            // For offset/pre-index: disp (load from base+disp).
            int64_t mem_off = post_index ? 0 : d.disp;
            if (is_load) {
                uint16_t val = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val, addr, 0, (uint8_t)width,
                     0, 0, (uint64_t)mem_off);
                // Sign-extend check: for non-vector loads, opc_ls bit 2
                // (i.e. opc_ls & 2) indicates LDRSW/LDRSB/LDRSH (sign-
                // extending loads). The decoder does NOT set d.cls to
                // LDRSW/LDRSB/LDRSH — it leaves the class as LDR_IMM/
                // LDR_UNS/LDR_REG and uses d.opc_ls to distinguish
                // sign-extended loads. (Matching the interpreter, which
                // checks `opc_ls & 2` directly.)
                bool sign_ext = !d.is_vec && (d.opc_ls & 2);
                if (sign_ext) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::SEXT, ext, val, 0, (uint8_t)(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else if (width < 8) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext, val, 0, (uint8_t)(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else {
                    store_arm_reg(block, d.rt, val);
                }
            } else {
                uint16_t val = load_arm_reg(block, d.rt);
                emit(block, IROp::STORE_MEM, 0, addr, val, (uint8_t)width,
                     0, 0, (uint64_t)mem_off);
            }
            // Writeback.
            if (d.writeback) {
                // For load/store, rn=31 means SP (not XZR).
                bool rn_is_sp = (d.rn == 31);
                if (post_index) {
                    // rn = base + disp
                    uint16_t off = load_imm(block, (uint64_t)d.disp);
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                } else {
                    // Pre-index: rn = base + disp.
                    uint16_t off = load_imm(block, (uint64_t)d.disp);
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                }
            }
            return false;
        }

        // ── LDP/STP ──────────────────────────────────────────────────
        // Fall back to interpreter for now — the pair load/store with
        // writeback has complex addressing that needs careful codegen.
        // TODO: implement direct IR translation for LDP/STP.
        case InstClass::LDP: case InstClass::STP: {
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── B / BL ───────────────────────────────────────────────────
        case InstClass::B: case InstClass::BL: {
            if (d.cls == InstClass::BL) {
                uint16_t lr = load_imm(block, cur_pc + 4);
                store_arm_reg(block, 30, lr);
            }
            uint64_t target = cur_pc + d.imm;
            // B is unconditional — encode as BRCOND with cond=AL (always).
            // Using BRCOND_FALLTHRU signals to the executor/codegen that
            // the branch is unconditional and there's no fall-through.
            emit(block, IROp::BRCOND_FALLTHRU, 0, 0, 0, 0, 14 /*AL*/, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── BR / BLR ────────────────────────────────────────────────
        case InstClass::BR: case InstClass::BLR: {
            if (d.cls == InstClass::BLR) {
                uint16_t lr = load_imm(block, cur_pc + 4);
                store_arm_reg(block, 30, lr);
            }
            uint16_t target = load_arm_reg(block, d.rn);
            emit(block, IROp::BR, 0, target);
            block.ends_with_branch = true;
            return true;
        }

        // ── RET ─────────────────────────────────────────────────────
        case InstClass::RET: {
            uint16_t target = load_arm_reg(block, d.rn);
            emit(block, IROp::BR, 0, target);
            block.ends_with_branch = true;
            return true;
        }

        // ── Bcond ───────────────────────────────────────────────────
        case InstClass::Bcond: {
            uint64_t target = cur_pc + d.imm;
            emit(block, IROp::BRCOND, 0, 0, 0, 0, d.cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── CBZ / CBNZ ──────────────────────────────────────────────
        // Per ARM ARM, CBZ/CBNZ do NOT modify any flags. They branch
        // based on whether the register is zero. We model this with a
        // dedicated BRCOND_ZERO op that branches directly on (val == 0)
        // without touching the flag state. cond=0 (EQ) for CBZ,
        // cond=1 (NE) for CBNZ.
        case InstClass::CBZ: case InstClass::CBNZ: {
            uint16_t val = load_arm_reg(block, d.rt);
            uint64_t target = cur_pc + d.imm;
            uint8_t cond = (d.cls == InstClass::CBZ) ? 0 /*EQ*/ : 1 /*NE*/;
            emit(block, IROp::BRCOND_ZERO, 0, val, 0, 0, cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── TBZ / TBNZ ──────────────────────────────────────────────
        // Per ARM ARM, TBZ/TBNZ do NOT modify flags. They branch based
        // on whether bit `bit` of `rt` is zero (TBZ) or one (TBNZ).
        // We use BRCOND_BIT which tests the bit directly without
        // touching flags. cond=0 (EQ) for TBZ, cond=1 (NE) for TBNZ.
        // imm = branch target, width = bit number (0-63).
        case InstClass::TBZ: case InstClass::TBNZ: {
            uint16_t val = load_arm_reg(block, d.rt);
            uint64_t target = cur_pc + d.imm;
            uint8_t cond = (d.cls == InstClass::TBZ) ? 0 /*EQ*/ : 1 /*NE*/;
            uint8_t bit = (uint8_t)(d.imm_u & 0x3F);
            emit(block, IROp::BRCOND_BIT, 0, val, 0, bit, cond, 0, target, cur_pc);
            block.ends_with_branch = true;
            return true;
        }

        // ── ADR / ADRP ──────────────────────────────────────────────
        case InstClass::ADR: {
            uint16_t r = load_imm(block, cur_pc + d.imm);
            store_arm_reg(block, d.rd, r);
            return false;
        }
        case InstClass::ADRP: {
            uint16_t r = load_imm(block, (cur_pc & ~0xFFFULL) + d.imm);
            store_arm_reg(block, d.rd, r);
            return false;
        }

        // ── SVC ─────────────────────────────────────────────────────
        case InstClass::SVC:
        case InstClass::SVC_IMM:
            emit(block, IROp::SVC, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;  // ends block (syscall may modify PC)

        // ── MSR / MRS (system reg access) ───────────────────────────
        case InstClass::MSR: case InstClass::MSR_SYS:
        case InstClass::MRS: case InstClass::MRS_SYS:
            // Route to interpreter for now — TPIDR_EL0/NZCV/FPCR/FPSR
            // handling is fiddly and these are infrequent.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;

        // ── Atomics (LDXR/STXR/LDAR/STLR/LSE_ATOMIC) ───────────────
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
        case InstClass::LSE_ATOMIC:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;

        // ── BRK / HLT (terminators) ─────────────────────────────────
        case InstClass::BRK: case InstClass::BRK_IMM:
        case InstClass::HLT: case InstClass::HLT_IMM:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;

        // ── CLREX / BARRIER ─────────────────────────────────────────
        case InstClass::CLREX: case InstClass::CLREX_INST:
        case InstClass::BARRIER:
            emit(block, IROp::NOP);
            return false;

        // ── SIMD / FP — fall back to interpreter (single-instruction) ──
        // We don't model the vector register file in IR. Block does NOT
        // split — the interpreter call is inline.
        case InstClass::SIMD_LD1: case InstClass::SIMD_ST1:
        case InstClass::SIMD_LOGICAL: case InstClass::SIMD_SHIFT:
        case InstClass::SIMD_DUP: case InstClass::SIMD_CNT:
        case InstClass::SIMD_REV: case InstClass::SIMD_DP:
        case InstClass::FMOV: case InstClass::FMOV_IMM:
        case InstClass::FMOV_VD1: case InstClass::FMOV_RVD1:
        case InstClass::FADD: case InstClass::FSUB:
        case InstClass::FMUL: case InstClass::FDIV:
        case InstClass::FMAX: case InstClass::FMIN:
        case InstClass::FNMUL: case InstClass::FMADD:
        case InstClass::FMSUB: case InstClass::FABS:
        case InstClass::FNEG: case InstClass::FSQRT:
        case InstClass::FCMP: case InstClass::FCMPE:
        case InstClass::FCVT: case InstClass::FCVTZS:
        case InstClass::FCVTZU: case InstClass::SCVTF:
        case InstClass::UCVTF: case InstClass::FRINT:
        case InstClass::FCSEL: case InstClass::FP_SCALAR:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;

        // ── SMADDL / SMSUBL / UMADDL / UMSUBL / SMULH / UMULH ──────
        // Rare; route to interpreter.
        case InstClass::SMADDL: case InstClass::SMSUBL:
        case InstClass::UMADDL: case InstClass::UMSUBL:
        case InstClass::SMULH: case InstClass::UMULH:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;

        // ── Everything else: inline interpreter call (no block split) ──
        default:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
    }
}

} // namespace arm64emu
