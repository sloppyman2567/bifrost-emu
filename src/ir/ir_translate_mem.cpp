// ir_translate_mem.cpp — memory load/store translation cases for the
// ARM64 → IR translator.
//
// Extracted from ir_translate.cpp to keep that file under 1000 lines.
// translate_to_ir() delegates InstClass::LDR_*, STR_*, LDP, STP, LDXR,
// STXR, LDAXR, STLXR, LDAR, STLR, and LSE_ATOMIC to translate_mem().
//
// All cases handled here are non-terminating (none of them set
// `block.ends_with_branch` or return true from translate_to_ir), so
// translate_mem() returns `true` (= "handled") to signal that
// translate_to_ir() should itself return `false` (= "block continues").
// Returning `false` from translate_mem() means "InstClass not handled
// here; let the main switch in ir_translate.cpp deal with it".
//
// See ir_translate.cpp for the header comment covering translator-wide
// design rules (vreg mapping, ZEXT-after-32-bit-ops, etc.).

#include "ir/ir.h"        // emit/load_imm/swar helpers + g_alloc
#include "ir/ir.hpp"      // public IR types
#include "core/emulator.h"  // for cond_true() (used by executor only)

namespace arm64emu {

// Returns `true` if `d.cls` was one of the memory load/store cases
// handled here (in which case translate_to_ir() returns `false` — none
// of the extracted cases terminate a block). Returns `false` to let the
// caller handle the InstClass itself.
bool translate_mem(IRBlock& block, const DecodedInst& d, uint64_t cur_pc) {
    switch (d.cls) {
        // ── LDR/STR (all forms) ──────────────────────────────────────
        case InstClass::LDR_IMM: case InstClass::LDR_UNS: case InstClass::LDR_REG:
        case InstClass::LDRSW: case InstClass::LDRSB: case InstClass::LDRSH:
        case InstClass::STR_IMM: case InstClass::STR_UNS: case InstClass::STR_REG: {
            // vector loads/stores (LDR/STR Q/D/S/H/B with
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
                return true;
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
                // Apply extend (UXTB..SXTX). For LDR/STR register-offset,
                // the shift is d.size if d.shift&1 is set, else 0.
                uint8_t shift_amt = (d.shift & 1) ? d.size : 0;
                uint16_t ext = apply_extend(block, idx, d.extend, shift_amt);
                addr = g_alloc.alloc();
                emit(block, IROp::ADD, addr, base, ext);
            } else if (post_index) {
                // Post-index: load/store from base (no offset).
                addr = base;
            } else {
                // for offset and pre-index
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
                emit(block, IROp::LOAD_MEM, val, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                // Sign-extend check: for non-vector loads, opc_ls bit 2
                // (i.e. opc_ls & 2) indicates LDRSW/LDRSB/LDRSH (sign-
                // extending loads). The decoder does NOT set d.cls to
                // LDRSW/LDRSB/LDRSH — it leaves the class as LDR_IMM/
                // LDR_UNS/LDR_REG and uses d.opc_ls to distinguish
                // sign-extended loads. (Matching the interpreter, which
                // checks `opc_ls & 2` directly.)
                bool sign_ext = !d.is_vec && (d.opc_ls & 2);
                if (d.is_vec) {
                    // BUGFIX (Turn 57): FP registers live in cpu.v_lo[],
                    // NOT cpu.regs[]. Use store_fp_reg to write to the
                    // correct array. For 32-bit FP loads (width=4), the
                    // upper 32 bits of v_lo are already zeroed by the
                    // ZEXT below — but we skip ZEXT for FP and rely on
                    // LOAD_MEM loading the right number of bytes + the
                    // store_fp_reg writing the full 64-bit vreg to v_lo.
                    if (width < 8) {
                        // Zero-extend to 64 bits (upper bytes of v_lo = 0).
                        uint16_t ext = g_alloc.alloc();
                        emit(block, IROp::ZEXT, ext, val, 0, static_cast<uint8_t>(width * 8));
                        store_fp_reg(block, d.rt, ext);
                    } else {
                        store_fp_reg(block, d.rt, val);
                    }
                } else if (sign_ext) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::SEXT, ext, val, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else if (width < 8) {
                    uint16_t ext = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext, val, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext);
                } else {
                    store_arm_reg(block, d.rt, val);
                }
            } else {
                // BUGFIX (Turn 57): for FP stores, load from v_lo[] not regs[].
                uint16_t val = d.is_vec ? load_fp_reg(block, d.rt) : load_arm_reg(block, d.rt);
                emit(block, IROp::STORE_MEM, 0, addr, val, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
            }
            // Writeback.
            if (d.writeback) {
                // For load/store, rn=31 means SP (not XZR).
                bool rn_is_sp = (d.rn == 31);
                if (post_index) {
                    // rn = base + disp
                    uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                } else {
                    // Pre-index: rn = base + disp.
                    uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                    uint16_t new_base = g_alloc.alloc();
                    emit(block, IROp::ADD, new_base, base, off);
                    store_arm_reg(block, d.rn, new_base, rn_is_sp);
                }
            }
            return true;
        }

        // ── LDP/STP ──────────────────────────────────────────────────
        // Native IR translation for GPR pair load/store.
        // SIMD LDP/STP (is_vec=true) still falls back to interpreter.
        case InstClass::LDP: case InstClass::STP: {
            if (d.is_vec) {
                emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
                return true;
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
                emit(block, IROp::LOAD_MEM, val1, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                // Sign-extend or zero-extend if needed (for 32-bit)
                if (width < 8) {
                    uint16_t ext1 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext1, val1, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt, ext1);
                } else {
                    store_arm_reg(block, d.rt, val1);
                }
                uint16_t val2 = g_alloc.alloc();
                emit(block, IROp::LOAD_MEM, val2, addr, 0, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off + esize));
                if (width < 8) {
                    uint16_t ext2 = g_alloc.alloc();
                    emit(block, IROp::ZEXT, ext2, val2, 0, static_cast<uint8_t>(width * 8));
                    store_arm_reg(block, d.rt2, ext2);
                } else {
                    store_arm_reg(block, d.rt2, val2);
                }
            } else {
                // STP: store rt, rt2
                uint16_t val1 = load_arm_reg(block, d.rt);
                uint16_t val2 = load_arm_reg(block, d.rt2);
                emit(block, IROp::STORE_MEM, 0, addr, val1, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off));
                emit(block, IROp::STORE_MEM, 0, addr, val2, static_cast<uint8_t>(width),
                     0, 0, static_cast<uint64_t>(mem_off + esize));
            }
            // Writeback
            if (d.writeback || post_index || pre_index) {
                bool rn_is_sp = (d.rn == 31);
                uint16_t off = load_imm(block, static_cast<uint64_t>(d.disp));
                uint16_t new_base = g_alloc.alloc();
                emit(block, IROp::ADD, new_base, base, off);
                store_arm_reg(block, d.rn, new_base, rn_is_sp);
            }
            return true;
        }

        // ── Atomics (LDXR/STXR/LDAR/STLR/LSE_ATOMIC) ───────────────
        // LDXR/STXR/STLR use CALL_INTERP for now — the fast C helper path
        // (jit_ldxr/jit_stxr/jit_stlr) is defined but needs more testing
        // before enabling in a stable release. LSE atomics get native JIT.
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
            emit(block, IROp::CALL_INTERP, 0, 0, 0, 0, 0, 0, 0, cur_pc);
            return true;

        case InstClass::LSE_ATOMIC: {
            // LSE atomics: native x86 lock-prefixed instructions.
            // Fields:
            //   d.atom_op: 0=LDADD,1=LDCLR,2=LDEOR,3=LDSET,4=SMAX,5=SMIN,
            //               6=UMAX,7=UMIN,8=SWP,0xC-0xF=CAS
            //   d.rs: source operand register
            //   d.rt: destination register (old value, if is_load)
            //   d.rn: base address register
            //   d.is_load: 1=LD variant (return old value), 0=ST variant
            //   d.size: 0=byte,1=half,2=word,3=double
            uint16_t base = load_arm_reg(block, d.rn, true);  // rn=31 → SP
            uint16_t src = load_arm_reg(block, d.rs);
            int width_bytes = 1 << d.size;
            if (d.atom_op >= 0xC) {
                // CAS: old=[Xn]; if old==Ws(rs), [Xn]=Wt(rt); Ws=old.
                // IR: src1=base, src2=desired(rt), imm=rs (expected input
                // AND old-value output). The JIT loads expected from
                // cpu.regs[rs] via emit_load_arm(inst.imm).
                uint16_t desired = load_arm_reg(block, d.rt);
                uint16_t dest = g_alloc.alloc();
                IRInst inst{};
                inst.op = IROp::ATOMIC;
                inst.dest = dest;
                inst.src1 = base;
                inst.src2 = desired;  // desired (rt) → [mem] on match
                inst.width = static_cast<uint8_t>(width_bytes);
                inst.cond = d.atom_op;
                inst.flags_op = 1;  // CAS always returns old
                inst.imm = d.rs;    // ARM reg: expected (in) + old (out)
                inst.arm_pc = cur_pc;
                block.insts.push_back(inst);
                store_arm_reg(block, d.rs, dest);  // rs = old value
            } else {
                // Non-CAS LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SWP/MAX/MIN).
                // is_load = (rt != 31): LD* variants return old value to rt;
                // ST* variants (rt=31/XZR) discard it. This lets the JIT
                // use faster codegen for ST* (e.g., STADD → lock add instead
                // of lock xadd, STSET → lock or instead of CAS-loop).
                uint16_t dest = g_alloc.alloc();
                emit(block, IROp::ATOMIC, dest, base, src,
                     static_cast<uint8_t>(width_bytes),
                     d.atom_op, (d.rt != 31) ? 1 : 0, d.rt, cur_pc);
                store_arm_reg(block, d.rt, dest);
            }
            return true;
        }

        default:
            // Not a memory load/store case — let the main translator handle it.
            return false;
    }
}

} // namespace arm64emu
