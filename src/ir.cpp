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

        // ── SBFM/UBFM (bitfield extract) ───────────────────────────
        // These stay native in the JIT — pass through as single IR ops.
        // (The JIT has dedicated, well-tested codegen for them and the
        // constant folder in ir_optimize.cpp knows how to fold them
        // when the source is a known immediate.)
        case InstClass::SBFM: case InstClass::UBFM: {
            uint16_t a = load_arm_reg(block, d.rn);
            IROp op = (d.cls == InstClass::SBFM) ? IROp::SBFM
                    : IROp::UBFM;
            emit_bf(block, op, d.rd, a, 0, d.immr, d.imms, d.sf ? 1 : 0, cur_pc);
            return false;
        }

        // ── EXTR (bitfield extract from concat) ────────────────────
        // EXTR Rd, Rn, Rm, #imms:
        //   Rd = (Rn:Rm) >> imms    (imms in [0, width-1])
        //
        // Decomposed into primitive IR ops the JIT already compiles
        // natively (SHL, SHR, OR), mirroring the BFM decomposition
        // strategy. This eliminates the IROp::EXTR native codegen path
        // (~45 lines of x86 in frostjit.cpp) and lets the constant
        // folder / peephole optimize the result.
        //
        //   imms == 0       → Rd = Rm                       (1 MOV)
        //   otherwise       → Rd = (Rn << (W-imms)) | (Rm >> imms)
        //                                                  (SHL+SHR+OR)
        //
        // 32-bit EXTR additionally needs a ZEXT to clear the high 32
        // bits (handled by the trailing zext_if_32bit at the end).
        case InstClass::EXTR: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rm_v = load_arm_reg(block, d.rm);
            int width = d.sf ? 64 : 32;
            int lsb = d.imms;  // ARM encodes the extraction point in imms

            uint16_t result;
            if (lsb == 0) {
                // Rd = Rm (low 64 bits of the concatenation).
                result = rm_v;
            } else {
                // hi_part = Rn << (width - lsb)
                uint16_t sh_hi = load_imm(block, (uint64_t)(width - lsb));
                uint16_t hi = g_alloc.alloc();
                emit(block, IROp::SHL, hi, rn_v, sh_hi);
                // lo_part = Rm >> lsb
                uint16_t sh_lo = load_imm(block, (uint64_t)lsb);
                uint16_t lo = g_alloc.alloc();
                emit(block, IROp::SHR, lo, rm_v, sh_lo);
                // result = hi | lo
                result = g_alloc.alloc();
                emit(block, IROp::OR, result, hi, lo);
            }
            result = zext_if_32bit(block, result, d.sf);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── BFM (bitfield insert) ───────────────────────────────────
        // BFM Rd, Rn, #immr, #imms:
        //   mask = ROR(Ones(imms+1), immr, width)
        //   Rd = (Rd & ~mask) | (ROR(Rn, immr) & mask)
        // We compute mask and ~mask at translation time (they're immediates).
        // For the ROR(Rn, immr), we use SHL+SHR+OR which the JIT handles
        // natively for both 32 and 64-bit (avoids the ROR & 0x3F issue).
        case InstClass::BFM: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rd_v = load_arm_reg(block, d.rd);
            int width = d.sf ? 64 : 32;
            int immr = d.immr % width;
            int imms = d.imms;
            // Compute mask = ROR(Ones(imms+1), immr, width)
            uint64_t welem = (imms + 1 >= 64) ? ~0ULL : ((1ULL << (imms + 1)) - 1);
            if (width == 32) welem &= 0xFFFFFFFFULL;
            uint64_t mask;
            if (immr == 0) {
                mask = welem;
            } else {
                mask = (welem >> immr) | (welem << (width - immr));
                if (width == 32) mask &= 0xFFFFFFFFULL;
            }
            uint64_t notmask = ~mask & ((width == 32) ? 0xFFFFFFFFULL : ~0ULL);
            // rotated = (Rn << (width - immr)) | (Rn >> immr)
            // Use SHL and SHR with immediate amounts (JIT handles these natively).
            uint16_t rot_hi, rot_lo, rotated;
            if (immr == 0) {
                rotated = rn_v;  // no rotation needed
            } else {
                uint16_t sh_hi = load_imm(block, width - immr);
                uint16_t sh_lo = load_imm(block, immr);
                rot_hi = g_alloc.alloc();
                emit(block, IROp::SHL, rot_hi, rn_v, sh_hi);
                rot_lo = g_alloc.alloc();
                emit(block, IROp::SHR, rot_lo, rn_v, sh_lo);
                rotated = g_alloc.alloc();
                emit(block, IROp::OR, rotated, rot_hi, rot_lo);
            }
            // field = rotated & mask
            uint16_t mask_v = load_imm(block, mask);
            uint16_t field = g_alloc.alloc();
            emit(block, IROp::AND, field, rotated, mask_v);
            // cleared = Rd & ~mask
            uint16_t notmask_v = load_imm(block, notmask);
            uint16_t cleared = g_alloc.alloc();
            emit(block, IROp::AND, cleared, rd_v, notmask_v);
            // result = cleared | field
            uint16_t result = g_alloc.alloc();
            emit(block, IROp::OR, result, cleared, field);
            store_arm_reg(block, d.rd, result);
            return false;
        }

        // ── CSEL / CSINC / CSINV / CSNEG ─────────────────────────────
        // CSEL  Rd = cond ? Rn : Rm         → CSEL(Rn, Rm)
        // CSINC Rd = cond ? Rn : (Rm + 1)   → CSEL(Rn, ADD(Rm, 1))
        // CSINV Rd = cond ? Rn : ~Rm        → CSEL(Rn, NOT(Rm))
        // CSNEG Rd = cond ? Rn : -Rm        → CSEL(Rn, NEG(Rm))
        // Decompose into transform + CSEL so the JIT only needs native
        // CSEL (which it has) — no CALL_INTERP for CSINC/CSINV/CSNEG.
        case InstClass::CSEL: case InstClass::CSINC:
        case InstClass::CSINV: case InstClass::CSNEG: {
            uint16_t rn_v = load_arm_reg(block, d.rn);
            uint16_t rm_v = load_arm_reg(block, d.rm);
            uint16_t sel_src2 = rm_v;
            if (d.cls == InstClass::CSINC) {
                uint16_t one = load_imm(block, 1);
                uint16_t inc = g_alloc.alloc();
                emit(block, IROp::ADD, inc, rm_v, one);
                sel_src2 = inc;
            } else if (d.cls == InstClass::CSINV) {
                uint16_t inv = g_alloc.alloc();
                emit(block, IROp::NOT, inv, rm_v);
                sel_src2 = inv;
            } else if (d.cls == InstClass::CSNEG) {
                uint16_t neg = g_alloc.alloc();
                emit(block, IROp::NEG, neg, rm_v);
                sel_src2 = neg;
            }
            // CSEL: dest = cond ? rn : sel_src2
            uint16_t r = g_alloc.alloc();
            emit(block, IROp::CSEL, r, rn_v, sel_src2, 0, d.cond, 0, d.rd, cur_pc);
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
            // BUGFIX (alpha.4): vector loads/stores (LDR/STR Q/D/S/H/B with
            // is_vec=true) must fall back to the interpreter. The IR
            // translator's load/store code uses d.rt as a general-purpose
            // register index (cpu.regs[d.rt]), but for vector instructions
            // d.rt refers to a vector register (V0-V31). Treating a vector
            // load/store as an integer one corrupts the wrong register —
            // e.g. `str q0, [sp, #32]` would store X0's value instead of
            // Q0's, and `ldr q0, [sp, #32]` would load into X0 instead of
            // Q0. This broke musl's __fixunstfsi/__extenddftf2 which spill
            // 128-bit long doubles to the stack via `str q0` / `ldp x0,x1`.
            if (d.is_vec) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
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
        // Native IR translation for GPR pair load/store.
        // SIMD LDP/STP (is_vec=true) still falls back to interpreter.
        case InstClass::LDP: case InstClass::STP: {
            if (d.is_vec) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            // GPR LDP/STP: decode esize from opc (bits[31:30])
            uint8_t opc = (d.raw >> 30) & 3;
            int esize = (opc == 2) ? 8 : 4;
            int width = esize;
            bool is_load = d.is_load;
            uint16_t base = load_arm_reg(block, d.rn, true);  // rn=31 → SP
            // Compute address based on addressing mode.
            // d.mode: 1=post-index, 2=signed offset, 3=pre-index
            bool post_index = (d.mode == 1);
            bool pre_index = (d.mode == 3);
            uint16_t addr;
            if (post_index) {
                // Load/store from base (no offset), writeback base+disp
                addr = base;
            } else {
                // Offset or pre-index: addr = base + disp
                // Use LOAD_MEM/STORE_MEM imm field for the displacement.
                addr = base;
            }
            int64_t mem_off = post_index ? 0 : d.disp;
            if (is_load) {
                uint16_t val1 = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val1, addr, 0, (uint8_t)width,
                     0, 0, (uint64_t)mem_off);
                // Sign-extend or zero-extend if needed (for 32-bit)
                if (width < 8) {
                    uint16_t ext1 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext1, val1, 0, (uint8_t)(width * 8));
                    store_arm_reg(block, d.rt, ext1);
                } else {
                    store_arm_reg(block, d.rt, val1);
                }
                uint16_t val2 = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val2, addr, 0, (uint8_t)width,
                     0, 0, (uint64_t)(mem_off + esize));
                if (width < 8) {
                    uint16_t ext2 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext2, val2, 0, (uint8_t)(width * 8));
                    store_arm_reg(block, d.rt2, ext2);
                } else {
                    store_arm_reg(block, d.rt2, val2);
                }
            } else {
                // STP: store rt, rt2
                uint16_t val1 = load_arm_reg(block, d.rt);
                uint16_t val2 = load_arm_reg(block, d.rt2);
                emit(block, IROp::STORE_MEM, 0, addr, val1, (uint8_t)width,
                     0, 0, (uint64_t)mem_off);
                emit(block, IROp::STORE_MEM, 0, addr, val2, (uint8_t)width,
                     0, 0, (uint64_t)(mem_off + esize));
            }
            // Writeback
            if (d.writeback || post_index || pre_index) {
                bool rn_is_sp = (d.rn == 31);
                uint16_t off = load_imm(block, (uint64_t)d.disp);
                uint16_t new_base = g_alloc.alloc();
                emit(block, IROp::ADD, new_base, base, off);
                store_arm_reg(block, d.rn, new_base, rn_is_sp);
            }
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

        // ── FMOV (general ↔ FP) — native IR ─────────────────────────
        case InstClass::FMOV: {
            uint32_t op = d.raw;
            // FMOV (general → FP, 64-bit): mask 0xFFE0FC00 == 0x9E600000
            if ((op & 0xFFE0FC00) == 0x9E600000) {
                bool to_fp = (op >> 16) & 1;
                uint8_t rd = op & 0x1F;
                uint8_t rn = (op >> 5) & 0x1F;
                if (to_fp) {
                    // v_lo[rd] = regs[rn]; v_hi[rd] = 0
                    uint16_t val = load_arm_reg(block, rn);
                    emit(block, IROp::FMOV_G2F, rd, val, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    // regs[rd] = v_lo[rn]
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    store_arm_reg(block, rd, v);
                }
                return false;
            }
            // FMOV (general → FP, 32-bit): mask 0xFFE0FC00 == 0x1E200000
            if ((op & 0xFFE0FC00) == 0x1E200000) {
                // 32-bit form — fall back to interpreter for now
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            // FMOV (scalar, immediate): fall back to interpreter
            // FMOV (FP↔FP register): fall back to interpreter
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        case InstClass::FMOV_VD1: {
            // FMOV Vd.D[1], Rn → v_hi[Vd] = regs[Rn]
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::FMOV_G2FHI, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        case InstClass::FMOV_RVD1: {
            // FMOV Rn, Vm.D[1] → regs[Rn] = v_hi[Vm]
            uint16_t v = g_alloc.alloc();
            emit(block, IROp::FMOV_FHI2G, v, d.rn, 0, 0, 0, 0, 0, cur_pc);
            store_arm_reg(block, d.rd, v);
            return false;
        }

        // ── FP_SCALAR — native FP arithmetic ────────────────────────
        // Decode specific FP op from raw bits and emit native IR ops.
        // Falls back to CALL_INTERP for ops we don't handle natively.
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            uint8_t ftype = (op >> 22) & 3;
            uint8_t rd = op & 0x1F;
            uint8_t rn = (op >> 5) & 0x1F;
            uint8_t rm = (op >> 16) & 0x1F;
            uint8_t opcode = (op >> 12) & 0xF;

            // FMOV (general ↔ FP, 64-bit): handled by InstClass::FMOV case
            // but decoder may classify it as FP_SCALAR. Check first.
            if ((op & 0xFFE0FC00) == 0x9E600000) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) {
                    uint16_t val = load_arm_reg(block, rn);
                    emit(block, IROp::FMOV_G2F, rd, val, 0, 0, 0, 0, 0, cur_pc);
                } else {
                    uint16_t v = g_alloc.alloc();
                    emit(block, IROp::FMOV_F2G, v, rn, 0, 0, 0, 0, 0, cur_pc);
                    store_arm_reg(block, rd, v);
                }
                return false;
            }
            // FMOV (general ↔ FP, 32-bit): fall back to interpreter
            if ((op & 0xFFE0FC00) == 0x1E200000) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }
            // FMOV (FP↔FP register): fall back to interpreter
            if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return false;
            }

            // FP arithmetic (2-source): bit[21]=1, bits[15:10] != 0b000100 (FMOV imm)
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3F) != 0x04) {
                // FADD=0x2, FSUB=0x3, FMUL=0x0, FDIV=0x1, FMAX=0x4, FMIN=0x5, FNMUL=0x6
                if (opcode <= 6 && ftype <= 1) {
                    emit(block, IROp::FP_BINOP, rd, rn, rm, ftype, 0, 0, opcode, cur_pc);
                    return false;
                }
            }
            // FP 1-source: bit[21]=1, bits[15:10]=0b010000
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3F) == 0x10) {
                // FMOV=0x0, FABS=0x1, FNEG=0x2, FSQRT=0x3
                if (opcode <= 3 && ftype <= 1) {
                    emit(block, IROp::FP_UNOP, rd, rn, 0, ftype, 0, 0, opcode, cur_pc);
                    return false;
                }
            }
            // FCVTZS/FCVTZU: FP→int (toward zero)
            // Encoding: (op & 0x7F3F0000) == 0x1E380000, rmode=3 (toward zero)
            if ((op & 0x7F3F0000) == 0x1E380000) {
                bool is_unsigned = (op >> 16) & 1;
                if (ftype <= 1) {
                    emit(block, IROp::FP_F2I, rd, rn, 0, ftype, 0, 0, is_unsigned, cur_pc);
                    return false;
                }
            }
            // SCVTF/UCVTF: int→FP
            // Encoding: (op & 0x7F3F0000) == 0x1E220000
            if ((op & 0x7F3F0000) == 0x1E220000) {
                bool is_unsigned = (op >> 16) & 1;
                if (ftype <= 1) {
                    emit(block, IROp::FP_I2F, rd, rn, 0, ftype, 0, 0, is_unsigned, cur_pc);
                    return false;
                }
            }
            // FCMP/FCMPE: FP compare
            // Encoding: (op & 0xFF20FC1F) == 0x1E202000 (with Rm≠31)
            //           (op & 0xFF20FC1F) == 0x1E202008 (with #0.0)
            if ((op & 0xFF200000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0x08) {
                bool with_zero = (rm == 31);
                if (with_zero) {
                    // FCMP Dn, #0.0 — compare against zero
                    emit(block, IROp::FP_CMP, 0, rn, 0, ftype, 0, 0, 0, cur_pc);
                } else {
                    emit(block, IROp::FP_CMP, 0, rn, rm, ftype, 0, 0, 0, cur_pc);
                }
                return false;
            }
            // FMOV (scalar, immediate): (op & 0xFFE0001F) == 0x1E600000
            // Already handled above for FP↔FP and general. The immediate
            // form has bits[15:10] = 0b000100.
            // We decode the 8-bit FP immediate here and emit FP_MOVI.
            if ((op & 0xFFE0001F) == 0x1E600000 && ((op >> 5) & 0x1F) == 0) {
                uint8_t imm8 = (op >> 13) & 0xFF;
                // VFPExpandImm for double (ftype=1):
                //   sign = imm8[7], exp = NOT(imm8[6]) : imm8[5:4] : 1000 (4 bits)
                //   mantissa = imm8[3:0] : 0000...0000 (48 bits)
                uint64_t sign = (imm8 >> 7) & 1;
                uint64_t exp, mant;
                if (ftype == 1) {
                    // Double: exp = (NOT(imm8[6]) << 10) | (imm8[5:4] << 8) | 0x3F0
                    exp = (~(imm8 >> 6) & 1);
                    exp = (exp << 10) | ((imm8 & 0x30) << 4) | 0x3F0;
                    mant = (uint64_t)(imm8 & 0x0F) << 48;
                    uint64_t bits = (sign << 63) | (exp << 52) | mant;
                    emit(block, IROp::FP_MOVI, rd, 0, 0, ftype, 0, 0, bits, cur_pc);
                } else {
                    // Single: exp = (NOT(imm8[6]) << 6) | (imm8[5:4] << 4) | 0x1C
                    exp = (~(imm8 >> 6) & 1);
                    exp = (exp << 6) | ((imm8 & 0x30) << 0) | 0x1C;
                    mant = (uint32_t)(imm8 & 0x0F) << 19;
                    uint64_t bits = (sign << 31) | (exp << 23) | mant;
                    emit(block, IROp::FP_MOVI, rd, 0, 0, ftype, 0, 0, bits, cur_pc);
                }
                return false;
            }
            // Everything else (FCVT, FRINT, FMADD, etc.)
            // falls back to interpreter.
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── SIMD LOGICAL (AND/ORR/EOR/BIC/ORN/EON) — native ────────
        case InstClass::SIMD_LOGICAL: {
            uint32_t op = d.raw;
            uint8_t opcode = (op >> 12) & 0xF;
            // ARM SIMD logical opcodes: AND=3, BIC=0, ORR=1, ORN=2,
            //                           EOR=7, EON=6, BIF=8, BIT=9, BSL=10
            // Map to our SIMD_LOGICAL imm: 0=and,1=orr,2=xor,3=bic,4=orn,5=eon
            uint8_t simd_op;
            switch (opcode) {
                case 0x3: simd_op = 0; break; // AND
                case 0x1: simd_op = 1; break; // ORR
                case 0x7: simd_op = 2; break; // EOR
                case 0x0: simd_op = 3; break; // BIC
                case 0x2: simd_op = 4; break; // ORN
                case 0x6: simd_op = 5; break; // EON
                default:
                    emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                    return false;
            }
            emit(block, IROp::SIMD_LOGICAL, d.rd, d.rn, d.rm, 0, 0, 0, simd_op, cur_pc);
            return false;
        }

        // ── SIMD DUP — native ──────────────────────────────────────
        case InstClass::SIMD_DUP: {
            // dup Vd.2d, Rn → broadcast Rn to both halves
            uint16_t val = load_arm_reg(block, d.rn);
            emit(block, IROp::SIMD_DUP, d.rd, val, 0, 0, 0, 0, 0, cur_pc);
            return false;
        }

        // ── SIMD LD1/ST1 — native (128-bit load/store) ─────────────
        case InstClass::SIMD_LD1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            // Load 16 bytes: v_lo[rt] = mem[base], v_hi[rt] = mem[base+8]
            uint16_t lo = g_alloc.alloc();
            emit(block, IROp::LOAD_MEM, lo, base, 0, 8, 0, 0, 0);
            uint16_t hi = g_alloc.alloc();
            emit(block, IROp::LOAD_MEM, hi, base, 0, 8, 0, 0, 8);
            // Store to v_lo/v_hi via SIMD_LDST (width=1 = load)
            emit(block, IROp::SIMD_LDST, d.rt, lo, hi, 1, 0, 0, 0, cur_pc);
            return false;
        }

        case InstClass::SIMD_ST1: {
            uint16_t base = load_arm_reg(block, d.rn, true);
            // Store 16 bytes: mem[base] = v_lo[rt], mem[base+8] = v_hi[rt]
            // Use SIMD_LDST with width=0 to read v_lo/v_hi into vregs
            uint16_t lo = g_alloc.alloc();
            uint16_t hi = g_alloc.alloc();
            emit(block, IROp::SIMD_LDST, d.rt, lo, hi, 0, 0, 0, 0, cur_pc);
            emit(block, IROp::STORE_MEM, 0, base, lo, 8, 0, 0, 0);
            emit(block, IROp::STORE_MEM, 0, base, hi, 8, 0, 0, 8);
            return false;
        }

        // ── SIMD / FP — fall back to interpreter ──
        case InstClass::SIMD_SHIFT:
        case InstClass::SIMD_CNT:
        case InstClass::SIMD_REV: case InstClass::SIMD_DP:
        case InstClass::FMOV_IMM:
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
        case InstClass::FCSEL:
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
