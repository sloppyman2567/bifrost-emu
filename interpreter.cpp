// interpreter.cpp — ARM64 instruction interpreter.
//
// This file implements Emulator::execute(), which decodes and executes
// a single ARM64 instruction. The decode logic is shared with the
// future JIT via decoder.hpp.
//
// When adding a new instruction:
//   1. Add the decode in decoder.cpp (sets InstClass)
//   2. Add the execute case here
//   3. (Future) Add JIT codegen in jit.cpp

#include "arm64_emu.hpp"
#include "decoder.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace arm64emu {

// (decode_bitmask_imm was moved to the logical immediate switch case
//  inline — no longer needed as a separate function.)

// Set NZCV from a 64-bit add-with-carry result.
static uint64_t set_add_flags(CPU& cpu, uint64_t a, uint64_t b, uint64_t carry_in,
                              int width, bool set_flags) {
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    uint64_t a_w = a & mask;
    uint64_t b_w = b & mask;
    uint64_t sum = a_w + b_w + carry_in;
    uint64_t res = sum & mask;
    if (set_flags) {
        bool n = (res >> (width - 1)) & 1;
        bool z = (res == 0);
        bool c = (sum > mask);
        bool sa = (a_w >> (width - 1)) & 1;
        bool sb = (b_w >> (width - 1)) & 1;
        bool sr = (res >> (width - 1)) & 1;
        bool v = (sa == sb) && (sa != sr);
        cpu.set_flag_n(n); cpu.set_flag_z(z); cpu.set_flag_c(c); cpu.set_flag_v(v);
    }
    return res;
}

// Set NZCV from a subtraction: a - b = a + ~b + 1
static uint64_t set_sub_flags(CPU& cpu, uint64_t a, uint64_t b, int width,
                              bool set_flags) {
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    uint64_t a_w = a & mask;
    uint64_t b_w = b & mask;
    uint64_t sum = a_w + (~b_w & mask) + 1;
    uint64_t res = sum & mask;
    if (set_flags) {
        bool n = (res >> (width - 1)) & 1;
        bool z = (res == 0);
        bool c = (a_w >= b_w);
        bool sa = (a_w >> (width - 1)) & 1;
        bool sb = (b_w >> (width - 1)) & 1;
        bool sr = (res >> (width - 1)) & 1;
        bool v = (sa != sb) && (sa != sr);
        cpu.set_flag_n(n); cpu.set_flag_z(z); cpu.set_flag_c(c); cpu.set_flag_v(v);
    }
    return res;
}

void Emulator::execute(uint32_t inst, uint64_t& next_pc, CPU& cpu) {
    uint32_t op = inst;

    // ── Decode via the shared decoder (v1.3.0 architecture) ──────────
    // The decoder is the single source of truth for instruction decode.
    // We call decode() once, then dispatch on d.cls. Instructions that
    // the decoder handles cleanly are executed here in the switch. For
    // instructions not yet migrated to the decoder (or that need complex
    // execution logic), we fall through to the legacy if-chain below.
    //
    // This hybrid approach lets us incrementally migrate handlers from
    // the if-chain to the switch without breaking anything. Eventually
    // (v2.0), the if-chain will be deleted entirely and the switch will
    // be the only dispatch path — shared with the JIT.
    {
        // ── Instruction decode cache ──────────────────────────────
        // Since guest code is not self-modifying (static binaries only),
        // each PC always decodes to the same instruction. Cache the
        // DecodedInst by PC to skip the decode() if-chain on repeated
        // executions of the same PC (e.g. tight loops).
        DecodedInst d;
        auto cache_it = decode_cache_.find(cpu.pc);
        if (cache_it != decode_cache_.end()) {
            d = cache_it->second;
            decode_cache_hits_++;
        } else {
            decode(d, inst);
            decode_cache_[cpu.pc] = d;
            decode_cache_misses_++;
        }
        switch (d.cls) {
            // ── ADC/ADCS/SBC/SBCS (add/subtract with carry) ──────────
            // These were previously unimplemented in the if-chain and
            // caused decode errors. Now handled via the decoder.
            case InstClass::ADC_REG:
            case InstClass::ADCS_REG:
            case InstClass::SBC_REG:
            case InstClass::SBCS_REG: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t carry_in = cpu.flag_c() ? 1 : 0;
                uint64_t res;
                bool is_sub = (d.cls == InstClass::SBC_REG || d.cls == InstClass::SBCS_REG);
                if (is_sub) {
                    uint64_t not_b = ~b & (width == 64 ? ~0ULL : 0xFFFFFFFF);
                    res = set_add_flags(cpu, a, not_b, carry_in, width, d.set_flags);
                } else {
                    res = set_add_flags(cpu, a, b, carry_in, width, d.set_flags);
                }
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] ──────────────────
            // Move 64-bit GPR to/from HIGH 64 bits of vector register.
            // Used by musl's 128-bit softfloat routines.
            case InstClass::FMOV_VD1:
                cpu.v_hi[d.rd] = cpu.regs[d.rn];
                return;
            case InstClass::FMOV_RVD1:
                cpu.regs[d.rd] = cpu.v_hi[d.rn];
                return;

            // ── Branches (clean decode, no field extraction needed) ──
            case InstClass::B:
            case InstClass::BL: {
                bool link = (d.cls == InstClass::BL);
                if (link) cpu.regs[30] = cpu.pc + 4;
                next_pc = cpu.pc + d.imm;
                
                return;
            }
            case InstClass::Bcond: {
                if (cond_true(d.cond, cpu.pstate))
                    next_pc = cpu.pc + d.imm;
                
                return;
            }
            case InstClass::CBZ:
            case InstClass::CBNZ: {
                uint64_t v = d.sf ? cpu.regs[d.rt] : (uint32_t)cpu.regs[d.rt];
                bool is_zero = (v == 0);
                bool taken = (d.cls == InstClass::CBZ) ? is_zero : !is_zero;
                if (taken) next_pc = cpu.pc + d.imm;
                
                return;
            }
            case InstClass::TBZ:
            case InstClass::TBNZ: {
                uint64_t v = cpu.regs[d.rt];
                bool bit_set = (v >> d.imm_u) & 1;
                bool taken = (d.cls == InstClass::TBZ) ? !bit_set : bit_set;
                if (taken) next_pc = cpu.pc + d.imm;
                
                return;
            }
            case InstClass::BR:
                next_pc = cpu.regs[d.rn];
                
                return;
            case InstClass::BLR:
                cpu.regs[30] = cpu.pc + 4;
                next_pc = cpu.regs[d.rn];
                
                return;
            case InstClass::RET:
                next_pc = cpu.regs[d.rn];
                if (next_pc == 0) next_pc = cpu.regs[30];  // RET with XZR
                
                return;

            // ── System ────────────────────────────────────────────────
            case InstClass::SVC_IMM:
                // Supervisor call: invoke the Linux AArch64 syscall layer.
                // The syscall number is in x8; args are in x0..x5; result
                // goes back into x0. The SVC immediate is ignored (Linux
                // doesn't use it).
                syscall(cpu);
                return;

            case InstClass::BRK_IMM: {
                // BRK #imm16 → deliver SIGTRAP. We follow the shell
                // convention for signal-terminated processes: exit code
                // = 128 + signal. BRK raises SIGTRAP (signal 5), so exit
                // 133. This matches real Linux behavior and prevents
                // libc's abort() from falling through into unrelated
                // code paths (was happening with glibc 2.36+ static
                // binaries hitting the getrandom vDSO assertion).
                uint16_t imm = (d.raw >> 5) & 0xFFFF;
                if (brk_verbose_) {
                    fprintf(stderr, "[emu] BRK #%u at pc=0x%llx (terminating)\n",
                            imm, (unsigned long long)cpu.pc);
                }
                cpu.running = false;
                cpu.exit_code = 128 + 5;  // SIGTRAP
                return;
            }

            case InstClass::HLT_IMM: {
                // HLT #imm16 — used by some baremetal demos as an exit
                // instruction. We treat the immediate as the exit code.
                uint16_t imm = (d.raw >> 5) & 0xFFFF;
                cpu.running = false;
                cpu.exit_code = imm;
                return;
            }

            case InstClass::CLREX_INST:
                // Clear the local exclusive monitor. (Per ARM ARM, only
                // STXR and CLREX clear the monitor. Branches used to do
                // it in earlier versions of this emulator but that was
                // a bug — see the beta.1 changelog entry.)
                cpu.excl_clear();
                return;

            case InstClass::HINT:
                // Hint space: NOP / YIELD / WFE / WFI / SEV / SEVL /
                // DSB / DMB / ISB. All are no-ops for a single-threaded
                // user-mode emulator.
                return;

            case InstClass::MRS_SYS: {
                // MRS Xt, <sysreg> — read a system register.
                // We model the small subset that glibc/musl probe during
                // startup: TPIDR_EL0, TPIDRRO_EL0, CTR_EL0, DCZID_EL0,
                // NZCV, FPCR, FPSR, plus a few ID registers.
                uint8_t op0 = d.sys_op0, op1 = d.sys_op1;
                uint8_t crn = d.sys_crn, crm = d.sys_crm, op2 = d.sys_op2;
                uint8_t rt  = d.rt;
                uint64_t val = 0;
                bool handled = false;
                if (op0 == 3 && op1 == 3) {
                    if (crn == 13 && crm == 0 && op2 == 2) {
                        val = cpu.tpidr_el0; handled = true;            // TPIDR_EL0
                    } else if (crn == 13 && crm == 0 && op2 == 3) {
                        val = cpu.tpidrro_el0; handled = true;          // TPIDRRO_EL0
                    } else if (crn == 0 && crm == 0 && op2 == 1) {
                        // CTR_EL0 — DIC=1, IDC=0, Erg=4, CWG=4, DminLine=12, IminLine=4
                        val = 0x8444C004; handled = true;
                    } else if (crn == 0 && crm == 0 && op2 == 7) {
                        val = (1u << 4); handled = true;                // DCZID_EL0 (no DC ZVA)
                    } else if (crn == 4 && crm == 2 && op2 == 0) {
                        // NZCV
                        uint32_t nzcv = 0;
                        if (cpu.flag_n()) nzcv |= (1u << 31);
                        if (cpu.flag_z()) nzcv |= (1u << 30);
                        if (cpu.flag_c()) nzcv |= (1u << 29);
                        if (cpu.flag_v()) nzcv |= (1u << 28);
                        val = nzcv; handled = true;
                    } else if (crn == 4 && crm == 4 && op2 == 0) {
                        val = cpu.fpcr; handled = true;                 // FPCR
                    } else if (crn == 4 && crm == 4 && op2 == 1) {
                        val = cpu.fpsr; handled = true;                 // FPSR
                    } else if (crn == 0 && crm == 0 && op2 == 0) {
                        val = 0x410FD080; handled = true;               // MIDR_EL1 (Cortex-A72)
                    } else if (crn == 0 && crm == 0 && op2 == 5) {
                        val = 0x10110222; handled = true;               // MVFR0_EL1
                    } else if (crn == 0 && crm == 0 && op2 == 6) {
                        val = 0x12122211; handled = true;               // MVFR1_EL1
                    } else if (crn == 0 && crm == 0 && op2 == 7) {
                        val = 0x00000043; handled = true;               // MVFR2_EL1
                    } else if (crn == 0 && crm == 2 && op2 == 0) {
                        val = 0x00000022; handled = true;               // ID_AA64PFR0_EL1
                    } else if (crn == 0 && crm == 2 && op2 == 2) {
                        val = 0x00000000; handled = true;               // ID_AA64MMFR0_EL1
                    } else if (crn == 0 && crm == 2 && op2 == 4) {
                        val = 0x00000000; handled = true;               // ID_AA64ISAR0_EL1
                    }
                }
                if (handled) {
                    if (rt != 31) cpu.regs[rt] = val;
                    return;
                }
                // Unknown sysreg: return 0 (treat as NOP).
                if (rt != 31) cpu.regs[rt] = 0;
                return;
            }

            case InstClass::MSR_SYS: {
                // MSR <sysreg>, Xt — write a system register.
                uint8_t op0 = d.sys_op0, op1 = d.sys_op1;
                uint8_t crn = d.sys_crn, crm = d.sys_crm, op2 = d.sys_op2;
                uint8_t rt  = d.rt;
                uint64_t v = (rt == 31) ? 0 : cpu.regs[rt];
                if (op0 == 3 && op1 == 3) {
                    if (crn == 4 && crm == 2 && op2 == 0) {
                        // NZCV
                        cpu.set_flag_n(v & (1u << 31));
                        cpu.set_flag_z(v & (1u << 30));
                        cpu.set_flag_c(v & (1u << 29));
                        cpu.set_flag_v(v & (1u << 28));
                        return;
                    }
                    if (crn == 13 && crm == 0 && op2 == 2) {
                        cpu.tpidr_el0 = v; return;                      // TPIDR_EL0
                    }
                    if (crn == 13 && crm == 0 && op2 == 3) {
                        cpu.tpidrro_el0 = v; return;                    // TPIDRRO_EL0
                    }
                    if (crn == 4 && crm == 4 && op2 == 0) {
                        cpu.fpcr = (uint32_t)v; return;                 // FPCR
                    }
                    if (crn == 4 && crm == 4 && op2 == 1) {
                        cpu.fpsr = (uint32_t)v; return;                 // FPSR
                    }
                    // Other EL0-accessible sysregs we don't model: NOP.
                    return;
                }
                // EL1+ sysregs: NOP in user mode.
                return;
            }

            // ── ADR / ADRP ────────────────────────────────────────────
            case InstClass::ADR:
            case InstClass::ADRP: {
                bool adrp = (d.cls == InstClass::ADRP);
                uint32_t immlo = (d.raw >> 29) & 3;
                uint32_t immhi = (d.raw >> 5) & 0x7FFFF;
                uint64_t imm = (immhi << 2) | immlo;
                if (adrp) {
                    uint64_t base = cpu.pc & ~0xFFFULL;
                    uint64_t v = sign_extend(imm, 21) << 12;
                    if (d.rd != 31) cpu.regs[d.rd] = base + v;
                } else {
                    uint64_t v = sign_extend(imm, 21);
                    if (d.rd != 31) cpu.regs[d.rd] = cpu.pc + v;
                }
                return;
            }

            // ── MOVN / MOVZ / MOVK ────────────────────────────────────
            case InstClass::MOVN:
            case InstClass::MOVZ:
            case InstClass::MOVK: {
                uint8_t hw = (d.raw >> 21) & 3;
                uint16_t imm16 = (d.raw >> 5) & 0xFFFF;
                int width = d.sf ? 64 : 32;
                int shift = hw * 16;
                uint64_t v;
                if (d.cls == InstClass::MOVN) {
                    v = ~((uint64_t)imm16 << shift);
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else if (d.cls == InstClass::MOVZ) {
                    v = (uint64_t)imm16 << shift;
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else { // MOVK
                    uint64_t mask = (width == 64)
                        ? (0xFFFFULL << shift)
                        : (0xFFFFULL << shift) & 0xFFFFFFFFULL;
                    uint64_t cur = cpu.regs[d.rd];
                    if (!d.sf) cur &= 0xFFFFFFFF;
                    v = (cur & ~mask) | ((uint64_t)imm16 << shift);
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                }
                return;
            }

            // ── ADD/SUB immediate ─────────────────────────────────────
            case InstClass::ADD_IMM:
            case InstClass::ADDS_IMM:
            case InstClass::SUB_IMM:
            case InstClass::SUBS_IMM: {
                bool sh = (d.raw >> 22) & 1;
                uint16_t imm12 = (d.raw >> 10) & 0xFFF;
                int width = d.sf ? 64 : 32;
                bool set_flags = d.set_flags;
                bool is_sub = d.is_sub;
                uint64_t a = (d.rn == 31 && !set_flags) ? cpu.sp : cpu.regs[d.rn];
                if (!d.sf && !(d.rn == 31 && !set_flags)) a &= 0xFFFFFFFF;
                uint64_t b = (uint64_t)imm12 << (sh ? 12 : 0);
                uint64_t res;
                if (is_sub) res = set_sub_flags(cpu, a, b, width, set_flags);
                else        res = set_add_flags(cpu, a, b, 0, width, set_flags);
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd == 31) {
                    if (!set_flags) cpu.sp = res;
                } else {
                    cpu.regs[d.rd] = res;
                }
                return;
            }

            // ── Bitfield (SBFM/BFM/UBFM) ─────────────────────────────
            case InstClass::SBFM:
            case InstClass::BFM:
            case InstClass::UBFM: {
                uint8_t opc = (d.raw >> 29) & 3;
                uint8_t immr = (d.raw >> 16) & 0x3F;
                uint8_t imms = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                if (!d.sf && (immr & 0x20 || imms & 0x20))
                    throw DecodeError(cpu.pc, inst);
                uint64_t src = cpu.regs[d.rn];
                if (!d.sf) src &= 0xFFFFFFFF;
                int datasize = width;
                if (imms >= immr) {
                    int len = imms - immr + 1;
                    uint64_t mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);
                    uint64_t extracted = (src >> immr) & mask;
                    if (opc == 0) {
                        uint64_t m = (1ULL << (len - 1));
                        if (extracted & m) {
                            uint64_t high = ~mask & (datasize == 64 ? ~0ULL : (1ULL<<datasize)-1);
                            extracted |= high;
                        }
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else if (opc == 2) {
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else {
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t dst_mask = mask << immr;
                        uint64_t keep = cur & ~dst_mask;
                        if (d.rd != 31) cpu.regs[d.rd] = keep | ((extracted << immr) & dst_mask);
                    }
                } else {
                    int len = imms + 1;
                    uint64_t mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);
                    // For 32-bit, rotate within 32 bits — ror64 would put
                    // wrapped bits above bit 32 where the (uint32_t) cast
                    // drops them. This was the root cause of the mallocng
                    // hang: `lsl w24, w26, #4` (= ubfm w24, w26, #28, #27)
                    // produced 0 instead of 32, making the stride 0, which
                    // caused alloc_slot to recurse infinitely.
                    uint64_t rotated;
                    if (width == 64) {
                        rotated = ror64(src, immr);
                    } else {
                        uint32_t s32 = (uint32_t)src;
                        uint8_t r = immr & 31;
                        rotated = (r == 0) ? s32 : ((s32 >> r) | (s32 << (32 - r)));
                    }
                    uint64_t extracted = rotated & mask;
                    if (opc == 0) {
                        uint64_t m = (1ULL << (len - 1));
                        if (extracted & m) {
                            uint64_t high = ~mask & (datasize == 64 ? ~0ULL : (1ULL<<datasize)-1);
                            extracted |= high;
                        }
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else if (opc == 2) {
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else {
                        uint64_t hi_mask = ~((1ULL << immr) - 1);
                        if (datasize == 32) hi_mask &= 0xFFFFFFFF;
                        uint64_t field_mask = mask | hi_mask;
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t rotated_w = (datasize == 64) ? rotated : (uint32_t)rotated;
                        if (d.rd != 31) cpu.regs[d.rd] = (cur & ~field_mask) | (rotated_w & field_mask);
                    }
                }
                if (!d.sf && d.rd != 31) cpu.regs[d.rd] &= 0xFFFFFFFF;
                return;
            }

            // ── EXTR ─────────────────────────────────────────────────
            case InstClass::EXTR: {
                uint8_t immr = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                uint64_t lo = cpu.regs[d.rn];
                uint64_t hi = cpu.regs[d.rm];
                if (!d.sf) { lo &= 0xFFFFFFFF; hi &= 0xFFFFFFFF; }
                // EXTR: concatenate hi:lo (128-bit), shift right by immr, take lower width bits
                uint64_t combined = (hi << width) | lo;
                uint64_t v = (combined >> immr) & (width == 64 ? ~0ULL : 0xFFFFFFFFULL);
                if (!d.sf) v &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = v;
                return;
            }

            // ── Logical immediate (AND/ORR/EOR/ANDS) ────────────────
            case InstClass::AND_IMM:
            case InstClass::ORR_IMM:
            case InstClass::EOR_IMM:
            case InstClass::ANDS_IMM: {
                uint8_t opc = (d.raw >> 29) & 3;
                bool Nbit = (d.raw >> 22) & 1;
                uint8_t immr = (d.raw >> 16) & 0x3F;
                uint8_t imms = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                // Use the decoder's pre-decoded bitmask (d.imm_u).
                // The decoder's decode_bitmask_imm is now correct (fixed
                // the esize==64 case to use width instead of ~0).
                uint64_t imm_val = d.imm_u;
                uint64_t a = cpu.regs[d.rn];
                if (!d.sf) a &= 0xFFFFFFFF;
                uint64_t res;
                bool set_flags = false;
                switch (opc) {
                    case 0: res = a & imm_val; break;
                    case 1: res = a | imm_val; break;
                    case 2: res = a ^ imm_val; break;
                    case 3: res = a & imm_val; set_flags = true; break;
                    default: throw DecodeError(cpu.pc, inst);
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                else if (opc == 1) cpu.sp = res;  // ORR to SP
                if (set_flags) {
                    cpu.set_flag_n((res >> (width - 1)) & 1);
                    cpu.set_flag_z(res == 0);
                    cpu.set_flag_c(false);
                    cpu.set_flag_v(false);
                }
                return;
            }

            // ── ADD/SUB shifted/extended register ─────────────────────
            case InstClass::ADD_REG:
            case InstClass::ADDS_REG:
            case InstClass::SUB_REG:
            case InstClass::SUBS_REG: {
                // Covers both shifted-register (mask 0x1F200000 == 0x0B000000)
                // and extended-register (mask 0x1FE00000 == 0x0B200000) forms.
                // The decoder collapsed both into the same InstClass; we
                // re-check the encoding to disambiguate.
                bool extended = ((d.raw & 0x1FE00000) == 0x0B200000);
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b;
                if (extended) {
                    b = extend_reg(cpu.regs[d.rm], d.extend, d.shift, d.sf);
                } else {
                    b = cpu.regs[d.rm];
                    if (!d.sf) b &= 0xFFFFFFFF;
                    switch (d.shift_type) {
                        case 0: b = b << d.shift; break;
                        case 1: b = (width == 64) ? (b >> d.shift) : ((uint32_t)b >> d.shift); break;
                        case 2: b = ((int64_t)b) >> d.shift; break;
                        case 3: b = ror64(b, d.shift) & (width == 64 ? ~0ULL : 0xFFFFFFFF); break;
                    }
                }
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res;
                if (d.is_sub) res = set_sub_flags(cpu, a, b, width, d.set_flags);
                else          res = set_add_flags(cpu, a, b, 0, width, d.set_flags);
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd == 31) {
                    if (!d.set_flags) cpu.sp = res;
                } else {
                    cpu.regs[d.rd] = res;
                }
                return;
            }

            // ── Logical shifted register (AND/ORR/EOR/ANDS) ──────────
            case InstClass::AND_REG:
            case InstClass::ORR_REG:
            case InstClass::EOR_REG:
            case InstClass::ANDS_REG: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                switch (d.shift_type) {
                    case 0: b = b << d.shift; break;
                    case 1: b = (width == 64) ? (b >> d.shift) : ((uint32_t)b >> d.shift); break;
                    case 2: b = ((int64_t)b) >> d.shift; break;
                    case 3: b = ror64(b, d.shift) & (width == 64 ? ~0ULL : 0xFFFFFFFF); break;
                }
                if (!d.sf) b &= 0xFFFFFFFF;
                uint8_t opc = (d.raw >> 29) & 3;
                bool N_bit = (d.raw >> 21) & 1;  // N bit: inverts b (BIC/ORN/EON/BICS)
                if (N_bit) b = ~b;
                uint64_t res;
                bool set_flags = false;
                switch (opc) {
                    case 0: res = a & b; break;   // AND (N=0) or BIC (N=1)
                    case 1: res = a | b; break;   // ORR (N=0) or ORN (N=1)
                    case 2: res = a ^ b; break;   // EOR (N=0) or EON (N=1)
                    case 3: res = a & b; set_flags = true; break; // ANDS (N=0) or BICS (N=1)
                    default: throw DecodeError(cpu.pc, inst);
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                else if (opc == 1) cpu.sp = res;
                if (set_flags) {
                    cpu.set_flag_n((res >> (width - 1)) & 1);
                    cpu.set_flag_z(res == 0);
                    cpu.set_flag_c(false);
                    cpu.set_flag_v(false);
                }
                return;
            }

            // ── Conditional select (CSEL/CSINC/CSINV/CSNEG) ──────────
            case InstClass::CSEL:
            case InstClass::CSINC:
            case InstClass::CSINV:
            case InstClass::CSNEG: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res;
                if (cond_true(d.cond, cpu.pstate)) {
                    res = a;
                } else {
                    switch (d.cls) {
                        case InstClass::CSEL:  res = b;     break;
                        case InstClass::CSINC: res = b + 1; break;
                        case InstClass::CSINV: res = ~b;    break;
                        case InstClass::CSNEG: res = -b;    break;
                        default: throw DecodeError(cpu.pc, inst);
                    }
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Conditional compare (CCMP/CCMN) ──────────────────────
            case InstClass::CCMP:
            case InstClass::CCMN: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                if (!d.sf) a &= 0xFFFFFFFF;
                uint64_t operand;
                if (d.is_register) {
                    operand = cpu.regs[d.rm];
                    if (!d.sf) operand &= 0xFFFFFFFF;
                } else {
                    operand = (d.raw >> 16) & 0x1F;  // imm5
                }
                if (cond_true(d.cond, cpu.pstate)) {
                    if (d.cls == InstClass::CCMP) {
                        set_sub_flags(cpu, a, operand, width, true);
                    } else {
                        set_add_flags(cpu, a, operand, 0, width, true);
                    }
                } else {
                    cpu.set_flag_n(d.nzcv_field & 8);
                    cpu.set_flag_z(d.nzcv_field & 4);
                    cpu.set_flag_c(d.nzcv_field & 2);
                    cpu.set_flag_v(d.nzcv_field & 1);
                }
                return;
            }

            // ── Data processing (1-source): RBIT/REV/REV16/REV32/CLZ/CLS ──
            case InstClass::RBIT:
            case InstClass::REV16:
            case InstClass::REV32:
            case InstClass::REV:
            case InstClass::CLZ:
            case InstClass::CLS: {
                int width = d.sf ? 64 : 32;
                uint64_t v = cpu.regs[d.rn];
                if (!d.sf) v &= 0xFFFFFFFF;
                switch (d.cls) {
                    case InstClass::RBIT: {
                        uint64_t r = 0;
                        for (int i = 0; i < width; i++)
                            if (v & (1ULL << i)) r |= (1ULL << (width - 1 - i));
                        v = r;
                        break;
                    }
                    case InstClass::REV16:
                        if (width == 64)
                            v = ((v & 0xFF00FF00FF00FF00ULL) >> 8) |
                                ((v & 0x00FF00FF00FF00FFULL) << 8);
                        else
                            v = ((v & 0xFF00FF00ULL) >> 8) | ((v & 0x00FF00FFULL) << 8);
                        break;
                    case InstClass::REV32:
                        if (width == 64)
                            v = ((v & 0xFFFF0000FFFF0000ULL) >> 16) |
                                ((v & 0x0000FFFF0000FFFFULL) << 16);
                        else
                            v = ((v & 0xFFFF0000ULL) >> 16) | ((v & 0x0000FFFFULL) << 16);
                        break;
                    case InstClass::REV:
                        v = (width == 64) ? __builtin_bswap64(v)
                                          : __builtin_bswap32((uint32_t)v);
                        break;
                    case InstClass::CLZ:
                        if (v == 0) v = width;
                        else v = (width == 64) ? __builtin_clzll(v)
                                               : __builtin_clz((uint32_t)v);
                        break;
                    case InstClass::CLS:
                        if (width == 64)
                            v = (v >> 63) ? __builtin_clzll(~v) : __builtin_clzll(v);
                        else
                            v = (v >> 31) ? __builtin_clz((uint32_t)~v) : __builtin_clz((uint32_t)v);
                        break;
                    default: break;
                }
                if (!d.sf) v &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = v;
                return;
            }

            // ── Data processing (2-source): UDIV/SDIV/LSL/LSR/ASR/ROR ──
            case InstClass::UDIV:
            case InstClass::SDIV:
            case InstClass::LSL:
            case InstClass::LSR:
            case InstClass::ASR:
            case InstClass::ROR: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res = 0;
                switch (d.cls) {
                    case InstClass::UDIV:
                        if (b != 0)
                            res = (width == 64) ? a / b : (uint32_t)a / (uint32_t)b;
                        break;
                    case InstClass::SDIV:
                        if (b != 0)
                            res = (width == 64) ? (uint64_t)((int64_t)a / (int64_t)b)
                                                : (uint64_t)((int32_t)a / (int32_t)b);
                        break;
                    case InstClass::LSL:
                        res = (width == 64) ? (a << (b & 63)) : ((uint32_t)a << (b & 31));
                        break;
                    case InstClass::LSR:
                        res = (width == 64) ? (a >> (b & 63)) : ((uint32_t)a >> (b & 31));
                        break;
                    case InstClass::ASR:
                        res = (width == 64) ? (uint64_t)((int64_t)a >> (b & 63))
                                            : (uint64_t)((int32_t)a >> (b & 31));
                        break;
                    case InstClass::ROR:
                        res = (width == 64) ? ror64(a, b & 63)
                                            : (uint32_t)ror64(a, b & 31);
                        break;
                    default: break;
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Data processing (3-source): MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH ──
            case InstClass::MADD:
            case InstClass::MSUB:
            case InstClass::SMADDL:
            case InstClass::SMSUBL:
            case InstClass::UMADDL:
            case InstClass::UMSUBL:
            case InstClass::UMULH:
            case InstClass::SMULH: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                uint64_t c = cpu.regs[d.ra];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; c &= 0xFFFFFFFF; }
                uint64_t res = 0;
                switch (d.cls) {
                    case InstClass::MADD:
                        res = (width == 64) ? (a * b) : ((uint32_t)a * (uint32_t)b);
                        res = c + res;
                        break;
                    case InstClass::MSUB:
                        res = (width == 64) ? (a * b) : ((uint32_t)a * (uint32_t)b);
                        res = c - res;
                        break;
                    case InstClass::SMADDL:
                        res = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
                        res = c + res;
                        break;
                    case InstClass::SMSUBL:
                        res = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
                        res = c - res;
                        break;
                    case InstClass::UMADDL:
                        res = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
                        res = c + res;
                        break;
                    case InstClass::UMSUBL:
                        res = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
                        res = c - res;
                        break;
                    case InstClass::UMULH: {
                        unsigned __int128 prod = (unsigned __int128)a * (unsigned __int128)b;
                        res = (uint64_t)(prod >> 64);
                        break;
                    }
                    case InstClass::SMULH: {
                        unsigned __int128 prod = (unsigned __int128)((int64_t)a * (int64_t)b);
                        res = (uint64_t)(prod >> 64);
                        break;
                    }
                    default: break;
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Load/store pair (STP/LDP, all modes) ──────────────────
            // Encoding: opc 101 V mode L imm7 Rt2 Rn Rt
            //   mode (bits 24:23): 01=post, 10=offset, 11=pre (bit 25 is 0
            //   for post/offset; bit 25 = 1 for pre, but pre-index STP/LDP
            //   currently gets misclassified as ORR — known issue, same as
            //   in the legacy if-chain, planned for v2.0 hierarchical decoder.)
            case InstClass::STP:
            case InstClass::LDP: {
                uint8_t opc = (d.raw >> 30) & 3;
                int esize;
                if (d.is_vec) {
                    esize = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
                } else {
                    esize = (opc == 2) ? 8 : 4;
                }
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                int64_t disp = d.disp;  // already scaled by esize in decoder
                uint64_t addr;
                if (d.mode == 1) {            // post-index
                    addr = base;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else if (d.mode == 3) {     // pre-index
                    addr = base + disp;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else {                       // signed offset (mode == 2 or 0)
                    addr = base + disp;
                }
                if (d.is_vec) {
                    if (d.is_load) {
                        uint64_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
                        mem_.read(addr, &lo1, 8);
                        if (esize >= 16) mem_.read(addr + 8, &hi1, 8);
                        mem_.read(addr + esize, &lo2, 8);
                        if (esize >= 16) mem_.read(addr + esize + 8, &hi2, 8);
                        cpu.v_lo[d.rt] = lo1; cpu.v_hi[d.rt] = hi1;
                        cpu.v_lo[d.rt2] = lo2; cpu.v_hi[d.rt2] = hi2;
                    } else {
                        mem_.write(addr, &cpu.v_lo[d.rt], 8);
                        if (esize >= 16) mem_.write(addr + 8, &cpu.v_hi[d.rt], 8);
                        mem_.write(addr + esize, &cpu.v_lo[d.rt2], 8);
                        if (esize >= 16) mem_.write(addr + esize + 8, &cpu.v_hi[d.rt2], 8);
                    }
                } else {
                    if (d.is_load) {
                        uint64_t v1 = 0, v2 = 0;
                        mem_.read(addr, &v1, esize);
                        mem_.read(addr + esize, &v2, esize);
                        if (d.rt  != 31) cpu.regs[d.rt]  = v1;
                        if (d.rt2 != 31) cpu.regs[d.rt2] = v2;
                    } else {
                        uint64_t v1 = (d.rt  == 31) ? 0 : cpu.regs[d.rt];
                        uint64_t v2 = (d.rt2 == 31) ? 0 : cpu.regs[d.rt2];
                        mem_.write(addr, &v1, esize);
                        mem_.write(addr + esize, &v2, esize);
                    }
                }
                return;
            }

            // ── LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS) ──
            // Decoder classifies any bit-21=1 encoding in the 111000 group
            // as LSE_ATOMIC. We check has_lse_ here: if the binary didn't
            // declare LSE via PT_NOTE, fall through to LDUR/STUR handling.
            case InstClass::LSE_ATOMIC: {
                if (!has_lse_) {
                    // Binary doesn't declare LSE — this encoding is LDUR/STUR.
                    // Fall through to the LDUR/STUR handler by re-dispatching
                    // on the (synthetic) LDR_UNS/STR_UNS class. We do this by
                    // re-decoding the instruction with the LSE bit ignored.
                    // Simplest: handle it inline as LDUR/STUR.
                    int width_bytes = 1 << d.size;
                    uint8_t opc_ls = (d.raw >> 22) & 3;
                    bool is_vec = (d.raw >> 26) & 1;
                    bool is_load = is_vec ? (opc_ls & 1)
                                          : ((opc_ls & 2) || (opc_ls & 1));
                    int16_t imm9 = sign_extend((d.raw >> 12) & 0x1FF, 9);
                    uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                    uint64_t addr = base + imm9;
                    if (is_vec) {
                        bool is_q = (opc_ls & 2) && d.size == 0;
                        int nbytes = is_q ? 16 : (1 << d.size);
                        if (is_load) {
                            uint64_t lo = 0, hi = 0;
                            mem_.read(addr, &lo, std::min(nbytes, 8));
                            if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                            cpu.v_lo[d.rt] = lo;
                            cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                        } else {
                            uint64_t lo = cpu.v_lo[d.rt];
                            mem_.write(addr, &lo, std::min(nbytes, 8));
                            if (nbytes > 8) {
                                uint64_t hi = cpu.v_hi[d.rt];
                                mem_.write(addr + 8, &hi, nbytes - 8);
                            }
                        }
                    } else {
                        if (is_load) {
                            uint64_t v = 0;
                            mem_.read(addr, &v, width_bytes);
                            if (d.rt != 31) {
                                if (opc_ls & 2) v = sign_extend(v, width_bytes * 8);
                                cpu.regs[d.rt] = v;
                            }
                        } else {
                            uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                            uint64_t mask = (width_bytes == 8) ? ~0ULL
                                          : ((1ULL << (width_bytes * 8)) - 1);
                            v &= mask;
                            mem_.write(addr, &v, width_bytes);
                        }
                    }
                    return;
                }
                // has_lse_ is true — execute as an LSE atomic.
                int width_bytes = 1 << d.size;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t mask = (width_bytes == 8) ? ~0ULL
                              : ((1ULL << (width_bytes * 8)) - 1);

                // CAS family (atom_op >= 0xC): compare-and-swap.
                if (d.atom_op >= 0xC) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes);
                    uint64_t cmp = cpu.regs[d.rs] & mask;
                    old &= mask;
                    if (old == cmp) {
                        uint64_t newv = cpu.regs[d.rt] & mask;
                        mem_.write(base, &newv, width_bytes);
                    }
                    if (d.rt != 31) cpu.regs[d.rt] = old;
                    return;
                }

                // SWP (atom_op == 0x8): atomic swap.
                if (d.atom_op == 0x8) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes);
                    old &= mask;
                    uint64_t newv = cpu.regs[d.rs] & mask;
                    mem_.write(base, &newv, width_bytes);
                    if (d.is_load && d.rt != 31) cpu.regs[d.rt] = old;
                    return;
                }

                // Other LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN).
                uint64_t a = 0, b = cpu.regs[d.rs];
                mem_.read(base, &a, width_bytes);
                a &= mask;
                b &= mask;
                uint64_t newv = 0;
                switch (d.atom_op) {
                    case 0x0: newv = (a + b) & mask; break;            // LDADD
                    case 0x1: newv = (a & ~b) & mask; break;           // LDCLR
                    case 0x2: newv = (a ^ b) & mask; break;            // LDEOR
                    case 0x3: newv = (a | b) & mask; break;            // LDSET
                    case 0x4: { // SMAX
                        int64_t sa = (int64_t)(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = (int64_t)(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa > sb ? sa : sb) & mask; break;
                    }
                    case 0x5: { // SMIN
                        int64_t sa = (int64_t)(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = (int64_t)(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa < sb ? sa : sb) & mask; break;
                    }
                    case 0x6: newv = (a > b ? a : b) & mask; break;    // UMAX
                    case 0x7: newv = (a < b ? a : b) & mask; break;    // UMIN
                    default:  newv = a & mask; break;
                }
                mem_.write(base, &newv, width_bytes);
                if (d.is_load && d.rt != 31) cpu.regs[d.rt] = a;
                return;
            }

            // ── Load/store (unsigned immediate offset) ───────────────
            case InstClass::LDR_IMM:
            case InstClass::STR_IMM: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint16_t imm12 = (d.raw >> 10) & 0xFFF;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    uint64_t scale = is_q ? 4 : size;
                    uint64_t addr = base + ((uint64_t)imm12 << scale);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        uint64_t hi = cpu.v_hi[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) mem_.write(addr + 8, &hi, nbytes - 8);
                    }
                    return;
                }
                uint64_t addr = base + (imm12 << size);
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes);
                }
                return;
            }

            // ── Load/store (unscaled / post-index / pre-index) ───────
            // LDUR/STUR (mode=0), post-index (mode=1), pre-index (mode=2)
            case InstClass::LDR_UNS:
            case InstClass::STR_UNS: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                int64_t disp = d.disp;  // sign-extended imm9
                uint64_t addr;
                if (d.mode == 1) {            // post-index
                    addr = base;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else {                       // unscaled (mode=0) or pre-index (mode=2)
                    addr = base + disp;
                    if (d.mode == 2) {         // pre-index: writeback
                        uint64_t nb = base + disp;
                        if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                    }
                }
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes);
                }
                return;
            }

            // ── Load/store (register offset) ──────────────────────────
            case InstClass::LDR_REG:
            case InstClass::STR_REG: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint8_t option = d.extend;
                uint8_t S = d.shift;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t off = extend_reg(cpu.regs[d.rm], option, S ? size : 0, true);
                uint64_t addr = base + off;
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8));
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes);
                }
                return;
            }

            // ── Load/store exclusive (LDXR/STXR/LDAXR/STLXR/STLR/LDAR) ──
            // The decoder distinguishes these by excl_low6 + acquire bits:
            //   0x0F = STXR/LDXR family (with Rs)
            //   0x1F = STLXR/LDAXR family (with Rs)
            //   0x3F = STLR/LDAR family (no Rs) — but LDXR/LDAXR with no Rs
            //          is also 0x3F; o0 (acquire) distinguishes them.
            case InstClass::LDXR:
            case InstClass::STXR:
            case InstClass::LDAXR:
            case InstClass::STLXR:
            case InstClass::LDAR:
            case InstClass::STLR: {
                int width_bytes = 1 << d.size;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                bool o0 = d.acquire;
                bool use_monitor = (o0 == 0) || (d.excl_low6 != 0x3F);

                if (!d.is_load) {
                    // Store-exclusive (STXR/STLXR) or store-release (STLR).
                    if (use_monitor) {
                        bool ok = cpu.excl_check(base, width_bytes);
                        if (ok) {
                            uint64_t v = cpu.regs[d.rt];
                            uint64_t mask = (width_bytes == 8) ? ~0ULL
                                          : ((1ULL << (width_bytes * 8)) - 1);
                            v &= mask;
                            mem_.write(base, &v, width_bytes);
                        }
                        if (d.rs != 31) cpu.regs[d.rs] = ok ? 0 : 1;
                        cpu.excl_clear();
                    } else {
                        // STLR: store-release (no monitor check, always succeeds)
                        uint64_t v = cpu.regs[d.rt];
                        uint64_t mask = (width_bytes == 8) ? ~0ULL
                                      : ((1ULL << (width_bytes * 8)) - 1);
                        v &= mask;
                        mem_.write(base, &v, width_bytes);
                    }
                } else {
                    // Load-exclusive (LDXR/LDAXR) or load-acquire (LDAR).
                    uint64_t v = 0;
                    mem_.read(base, &v, width_bytes);
                    cpu.regs[d.rt] = v;
                    if (use_monitor) {
                        cpu.excl_mark(base, width_bytes);
                    }
                }
                return;
            }

            // ── SIMD load/store multiple structures (LD1/ST1) ─────────
            case InstClass::SIMD_LD1:
            case InstClass::SIMD_ST1: {
                uint8_t opcode = (d.raw >> 12) & 0xF;
                bool Q = d.Q;
                int total_bytes = Q ? 16 : 8;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                int nregs = 1;
                if (opcode == 0x0) nregs = 1;
                else if (opcode == 0x2) nregs = 1;
                else if (opcode == 0x4) nregs = 2;
                else if (opcode == 0x6) nregs = 2;
                else if (opcode == 0x7) nregs = 2;
                else if (opcode == 0x8) nregs = 3;
                else if (opcode == 0xA) nregs = 4;
                for (int i = 0; i < nregs; i++) {
                    int r = (d.rt + i) & 0x1F;
                    uint64_t a = base + i * total_bytes;
                    if (d.is_load) {
                        uint8_t buf[16];
                        mem_.read(a, buf, total_bytes);
                        memcpy(&cpu.v_lo[r], buf, 8);
                        if (total_bytes == 16) memcpy(&cpu.v_hi[r], buf + 8, 8);
                        else cpu.v_hi[r] = 0;
                    } else {
                        uint8_t buf[16];
                        memcpy(buf, &cpu.v_lo[r], 8);
                        if (total_bytes == 16) memcpy(buf + 8, &cpu.v_hi[r], 8);
                        mem_.write(a, buf, total_bytes);
                    }
                }
                return;
            }

            // ── SIMD data-processing (sub-dispatched by raw opcode bits) ──
            // The decoder classifies the entire 0x0E000000 / 0x4E000000 /
            // 0x2E000000 / 0x6E000000 group as SIMD_DP. We re-extract the
            // Q/U/size/opcode fields here and sub-dispatch on the exact
            // encoding pattern. This is a large but flat if-chain —
            // migrating it to per-opcode InstClass values is a future
            // cleanup (it would balloon the enum).
            case InstClass::SIMD_DP: {
                uint32_t op = d.raw;
                bool Q = (op >> 30) & 1;
                bool U = (op >> 29) & 1;
                uint8_t size = (op >> 22) & 3;
                uint8_t opcode = (op >> 12) & 0x1F;
                uint8_t rm = (op >> 16) & 0x1F;
                uint8_t rn = (op >> 5) & 0x1F;
                uint8_t rd = op & 0x1F;
                (void)U; (void)opcode;  // used in sub-dispatch below

                // DUP (general)
                if ((op & 0xFFE0FC00) == 0x0E000C00) {
                    uint8_t imm5 = (op >> 16) & 0x1F;
                    int esize;
                    if (imm5 == 0x01) esize = 1;
                    else if (imm5 == 0x02) esize = 2;
                    else if (imm5 == 0x04) esize = 4;
                    else if (imm5 == 0x08) esize = 8;
                    else throw DecodeError(cpu.pc, inst);
                    int elems = (Q ? 16 : 8) / esize;
                    uint64_t src = cpu.regs[rn];
                    uint8_t bytes[16];
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = src;
                        if (esize == 1) v = src & 0xFF;
                        else if (esize == 2) v = src & 0xFFFF;
                        else if (esize == 4) v = src & 0xFFFFFFFF;
                        memcpy(bytes + i * esize, &v, esize);
                    }
                    memcpy(&cpu.v_lo[rd], bytes, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], bytes + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // INS (general)
                if ((op & 0xFFE0FC00) == 0x4E000C00) {
                    uint8_t imm5 = (op >> 16) & 0x1F;
                    int esize = 0, idx = 0;
                    for (int b = 0; b < 5; b++) {
                        if (imm5 & (1 << b)) { esize = 1 << b; break; }
                    }
                    idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
                    uint64_t src = cpu.regs[rn];
                    if (esize == 1) ((uint8_t*)&cpu.v_lo[rd])[idx] = src & 0xFF;
                    else if (esize == 2) ((uint16_t*)&cpu.v_lo[rd])[idx] = src & 0xFFFF;
                    else if (esize == 4) ((uint32_t*)&cpu.v_lo[rd])[idx] = src & 0xFFFFFFFF;
                    else if (esize == 8) {
                        if (idx == 0) cpu.v_lo[rd] = src;
                        else if (idx == 1) cpu.v_hi[rd] = src;
                    }
                    return;
                }
                // ORR (vector) — alias for MOV (vector)
                if ((op & 0xFF20FC00) == 0x0EA01C00) {
                    cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // EXT (extract)
                if ((op & 0xFFE00000) == 0x6E000000) {
                    uint8_t imm4 = (op >> 11) & 0xF;
                    uint8_t buf[32];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf + 16, &cpu.v_lo[rm], 8);
                    memcpy(buf + 24, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    int nbytes = Q ? 16 : 8;
                    memcpy(out, buf + imm4, nbytes);
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // REV16 (vector)
                if ((op & 0xBFFFFC00) == 0x0E201800) {
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    int nbytes = Q ? 16 : 8;
                    for (int i = 0; i < nbytes; i += 2) std::swap(buf[i], buf[i+1]);
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // REV32 (vector)
                if ((op & 0xBFFFFC00) == 0x0E203800) {
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    int nbytes = Q ? 16 : 8;
                    for (int i = 0; i < nbytes; i += 4) {
                        std::swap(buf[i], buf[i+3]);
                        std::swap(buf[i+1], buf[i+2]);
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // REV64 (vector)
                if ((op & 0xBFFFFC00) == 0x0E200800) {
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    int nbytes = Q ? 16 : 8;
                    for (int i = 0; i < nbytes; i += 8) {
                        for (int j = 0; j < 4; j++) std::swap(buf[i+j], buf[i+7-j]);
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // CNT (vector)
                if ((op & 0xBFFFFC00) == 0x0E205800) {
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    int nbytes = Q ? 16 : 8;
                    for (int i = 0; i < nbytes; i++) buf[i] = __builtin_popcount(buf[i]);
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // UADDLV (addv)
                if ((op & 0xBF3FFC00) == 0x0E31B800) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    uint64_t sum = 0;
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = 0;
                        memcpy(&v, buf + i * esize, esize);
                        sum += v;
                    }
                    cpu.v_lo[rd] = sum;
                    cpu.v_hi[rd] = 0;
                    return;
                }
                // CMEQ vs zero
                if ((op & 0xBF9FFC00) == 0x0E208800) {
                    int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        bool is_zero = true;
                        for (int b = 0; b < esize; b++) {
                            if (buf[i*esize + b] != 0) { is_zero = false; break; }
                        }
                        memset(buf + i*esize, is_zero ? 0xFF : 0x00, esize);
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // CMEQ two registers
                if ((op & 0xBFE0FC00) == 0x2E208C00) {
                    int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_rn[16], buf_rm[16];
                    memcpy(buf_rn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_rn + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_rm, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_rm + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems; i++) {
                        bool eq = (memcmp(buf_rn + i*esize, buf_rm + i*esize, esize) == 0);
                        memset(out + i*esize, eq ? 0xFF : 0x00, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // MOVI (vector immediate)
                if ((op & 0x3F8FFC00) == 0x0F00E400) {
                    uint8_t cmode = (op >> 12) & 0xF;
                    uint8_t imm8 = ((op >> 16) & 0x1F) << 3 | ((op >> 5) & 0x7);
                    if (cmode == 0xE) {
                        uint64_t val = imm8;
                        cpu.v_lo[rd] = val;
                        if (Q) cpu.v_hi[rd] = val;
                        else cpu.v_hi[rd] = 0;
                    } else {
                        uint8_t buf[16];
                        memset(buf, imm8, Q ? 16 : 8);
                        memcpy(&cpu.v_lo[rd], buf, 8);
                        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                        else cpu.v_hi[rd] = 0;
                    }
                    return;
                }
                // SHL (vector, immediate)
                if ((op & 0xBF00FC00) == 0x0F00A400) {
                    uint8_t immh = (op >> 19) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) return;
                    else if (immh < 2) { esize = 1; shift = (immh & 1) << 4 | immb; }
                    else if (immh < 4) { esize = 2; shift = (immh & 3) << 4 | immb; }
                    else if (immh < 8) { esize = 4; shift = (immh & 7) << 4 | immb; }
                    else { esize = 8; shift = (immh & 0xF) << 4 | immb; }
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = 0;
                        memcpy(&v, buf + i*esize, esize);
                        v <<= shift;
                        v &= (esize == 8) ? ~0ULL : ((1ULL << (esize*8)) - 1);
                        memcpy(buf + i*esize, &v, esize);
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // USHR (vector, immediate)
                if ((op & 0xBF00FC00) == 0x2F000400) {
                    uint8_t immh = (op >> 19) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) return;
                    else if (immh < 2) { esize = 1; shift = (8 - (((immh & 1) << 4) | immb)); }
                    else if (immh < 4) { esize = 2; shift = (16 - (((immh & 3) << 4) | immb)); }
                    else if (immh < 8) { esize = 4; shift = (32 - (((immh & 7) << 4) | immb)); }
                    else { esize = 8; shift = (64 - (((immh & 0xF) << 4) | immb)); }
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = 0;
                        memcpy(&v, buf + i*esize, esize);
                        v >>= shift;
                        memcpy(buf + i*esize, &v, esize);
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // EOR (vector)
                if ((op & 0xBFE0FC00) == 0x2E201C00) {
                    cpu.v_lo[rd] = cpu.v_lo[rn] ^ cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] ^ cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // AND (vector)
                if ((op & 0xBFE0FC00) == 0x0E201C00) {
                    cpu.v_lo[rd] = cpu.v_lo[rn] & cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ORR (vector) — full form
                if ((op & 0xBFE0FC00) == 0x0EA01C00) {
                    cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // BIC (vector)
                if ((op & 0xBFE0FC00) == 0x0EA01800) {
                    cpu.v_lo[rd] = cpu.v_lo[rn] & ~cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & ~cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // MVN/NOT (vector) — note: encoding collides with CNT in the
                // original if-chain (same mask 0xBFFFFC00 == 0x0E205800). The
                // original code's MVN branch was unreachable. Kept here for
                // source fidelity; the CNT case above catches it first.
                // TBL/TBX (stub: copy Vn to Vd)
                if ((op & 0xBFE0FC00) == 0x0E000000 || (op & 0xBFE0FC00) == 0x0E001000) {
                    cpu.v_lo[rd] = cpu.v_lo[rn];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // UMAXP/UMINP/SMAXP/SMINP family
                if ((op & 0xBFE0FC00) == 0x2E20A400) {
                    bool C = (op >> 15) & 1;
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_n[16], buf_m[16];
                    memcpy(buf_n, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_m, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems / 2; i++) {
                        uint64_t n0=0, n1=0, m0=0, m1=0;
                        memcpy(&n0, buf_n + (2*i) * esize, esize);
                        memcpy(&n1, buf_n + (2*i+1) * esize, esize);
                        memcpy(&m0, buf_m + (2*i) * esize, esize);
                        memcpy(&m1, buf_m + (2*i+1) * esize, esize);
                        uint64_t pn, pm;
                        if (C == 0) {
                            pn = (n0 > n1) ? n0 : n1;
                            pm = (m0 > m1) ? m0 : m1;
                            uint64_t res = (pn > pm) ? pn : pm;
                            memcpy(out + i * esize, &res, esize);
                        } else {
                            pn = (n0 < n1) ? n0 : n1;
                            pm = (m0 < m1) ? m0 : m1;
                            uint64_t res = (pn < pm) ? pn : pm;
                            memcpy(out + i * esize, &res, esize);
                        }
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // CMHS (vector)
                if ((op & 0xBFE0FC00) == 0x2E203400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_n[16], buf_m[16];
                    memcpy(buf_n, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_m, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems; i++) {
                        uint64_t n = 0, m = 0;
                        memcpy(&n, buf_n + i*esize, esize);
                        memcpy(&m, buf_m + i*esize, esize);
                        bool ge = (n >= m);
                        memset(out + i*esize, ge ? 0xFF : 0x00, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SHRN (vector, immediate)
                if ((op & 0xBF00FC00) == 0x0F008400) {
                    uint8_t immh = (op >> 19) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh < 2) { esize = 2; shift = (16 - ((immh & 1) << 4 | immb)); }
                    else if (immh < 4) { esize = 4; shift = (32 - (((immh & 3) << 4) | immb)); }
                    else { esize = 8; shift = (64 - (((immh & 7) << 4) | immb)); }
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    uint8_t out[8] = {0};
                    int dst_elems = (Q ? 8 : 4) / (esize / 2);
                    for (int i = 0; i < dst_elems; i++) {
                        uint64_t v = 0;
                        memcpy(&v, buf + i * esize, esize);
                        v >>= shift;
                        memcpy(out + i * (esize / 2), &v, esize / 2);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                    return;
                }
                // Fallback: NOP for SIMD ops we don't model. This is
                // incorrect but lets glibc continue. Programs that actually
                // depend on FP results will produce wrong output.
                return;
            }

            // ── FP scalar (FMOV/FADD/FSUB/FMUL/FDIV/FCMP/FCVT/...) ────
            // The decoder classifies the entire 0x1E200000 group as
            // FP_SCALAR. We sub-dispatch on raw opcode bits, same as the
            // legacy if-chain did.
            case InstClass::FP_SCALAR: {
                uint32_t op = d.raw;
                uint8_t rn = (op >> 5) & 0x1F;
                uint8_t rd = op & 0x1F;
                uint8_t rm = (op >> 16) & 0x1F;
                bool sf = (op >> 31) & 1;
                bool ftype = (op >> 22) & 1;  // 0=S(32-bit), 1=D(64-bit)

                auto read_fp_d = [&](int r) -> double {
                    uint64_t bits = cpu.v_lo[r];
                    double d; memcpy(&d, &bits, 8); return d;
                };
                auto read_fp_s = [&](int r) -> float {
                    uint32_t bits = (uint32_t)cpu.v_lo[r];
                    float f; memcpy(&f, &bits, 4); return f;
                };
                auto write_fp_d = [&](int r, double d) {
                    uint64_t bits; memcpy(&bits, &d, 8);
                    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
                };
                auto write_fp_s = [&](int r, float f) {
                    uint32_t bits; memcpy(&bits, &f, 4);
                    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
                };

                // FMOV (general ↔ FP, 64-bit)
                if ((op & 0xFFE0FC00) == 0x9E600000) {
                    bool to_fp = (op >> 16) & 1;
                    if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn]; cpu.v_hi[rd] = 0; }
                    else       { cpu.regs[rd] = cpu.v_lo[rn]; }
                    return;
                }
                // FMOV (general ↔ FP, 32-bit)
                if ((op & 0xFFE0FC00) == 0x1E200000) {
                    bool to_fp = (op >> 16) & 1;
                    if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn] & 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
                    else       { cpu.regs[rd] = cpu.v_lo[rn] & 0xFFFFFFFF; }
                    return;
                }
                // FMOV (scalar, immediate)
                if ((op & 0xFFE0001F) == 0x1E600000 && ((op >> 5) & 0x1F) == 0) {
                    uint8_t imm8 = (op >> 13) & 0xFF;
                    uint64_t sign = ((uint64_t)(imm8 >> 7)) & 1;
                    uint64_t exp = ((uint64_t)(imm8 >> 3)) & 0xF;
                    uint64_t mant = ((uint64_t)imm8) & 0x7;
                    uint64_t exp_field;
                    if ((exp & 0xF) == 0xF) exp_field = 0x7FF;
                    else exp_field = ((exp ^ 0x8) & 0xF) + 1023;
                    uint64_t bits = (sign << 63) | (exp_field << 52) | (mant << 49);
                    if (ftype) {
                        cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
                    } else {
                        uint32_t sign32 = (uint32_t)sign;
                        uint32_t exp32;
                        if ((exp & 0xF) == 0xF) exp32 = 0xFF;
                        else exp32 = ((exp ^ 0x8) & 0xF) + 127;
                        uint32_t bits32 = (sign32 << 31) | (exp32 << 23) | (((uint32_t)mant) << 20);
                        cpu.v_lo[rd] = bits32; cpu.v_hi[rd] = 0;
                    }
                    return;
                }
                // FMOV (register, FP to FP)
                if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000) {
                    cpu.v_lo[rd] = cpu.v_lo[rn];
                    if (ftype) cpu.v_hi[rd] = 0;
                    else { cpu.v_lo[rd] &= 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
                    return;
                }
                // FP arithmetic (2-source): FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FNMUL
                if ((op & 0xFF200000) == 0x1E200000 && ((op >> 21) & 1) == 1) {
                    uint8_t opcode = (op >> 12) & 0xF;
                    if (ftype) {
                        double a = read_fp_d(rn), b = read_fp_d(rm), r = 0;
                        switch (opcode) {
                            case 0x2: r = a + b; break;
                            case 0x3: r = a - b; break;
                            case 0x0: r = a * b; break;
                            case 0x1: r = a / b; break;
                            case 0x4: r = (a > b) ? a : b; break;
                            case 0x5: r = (a < b) ? a : b; break;
                            case 0x6: r = -(a * b); break;
                            default: r = 0; break;
                        }
                        write_fp_d(rd, r);
                    } else {
                        float a = read_fp_s(rn), b = read_fp_s(rm), r = 0;
                        switch (opcode) {
                            case 0x2: r = a + b; break;
                            case 0x3: r = a - b; break;
                            case 0x0: r = a * b; break;
                            case 0x1: r = a / b; break;
                            case 0x4: r = (a > b) ? a : b; break;
                            case 0x5: r = (a < b) ? a : b; break;
                            case 0x6: r = -(a * b); break;
                            default: r = 0; break;
                        }
                        write_fp_s(rd, r);
                    }
                    return;
                }
                // FP 1-source: FABS/FNEG/FSQRT/FRINT*
                if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3F) == 0x10) {
                    uint8_t opcode = (op >> 12) & 0xF;
                    if (ftype) {
                        double a = read_fp_d(rn), r = 0;
                        switch (opcode) {
                            case 0x0: r = a; break;
                            case 0x1: r = std::fabs(a); break;
                            case 0x2: r = -a; break;
                            case 0x3: r = std::sqrt(a); break;
                            case 0x4: r = std::rint(a); break;
                            case 0x5: r = std::ceil(a); break;
                            case 0x6: r = std::floor(a); break;
                            case 0x7: r = std::trunc(a); break;
                            case 0xC: r = std::rint(a); break;
                            case 0xE: r = std::rint(a); break;
                            case 0xF: r = std::rint(a); break;
                            default: r = a; break;
                        }
                        write_fp_d(rd, r);
                    } else {
                        float a = read_fp_s(rn), r = 0;
                        switch (opcode) {
                            case 0x0: r = a; break;
                            case 0x1: r = std::fabsf(a); break;
                            case 0x2: r = -a; break;
                            case 0x3: r = std::sqrtf(a); break;
                            case 0x4: r = std::rintf(a); break;
                            case 0x5: r = std::ceilf(a); break;
                            case 0x6: r = std::floorf(a); break;
                            case 0x7: r = std::truncf(a); break;
                            case 0xC: r = std::rintf(a); break;
                            default: r = a; break;
                        }
                        write_fp_s(rd, r);
                    }
                    return;
                }
                // FCVT (S↔D)
                if ((op & 0xFFFFFC00) == 0x1E624000) { // FCVT Sd, Dn
                    write_fp_s(rd, (float)read_fp_d(rn)); return;
                }
                if ((op & 0xFFFFFC00) == 0x1E22C000) { // FCVT Dd, Sn
                    write_fp_d(rd, (double)read_fp_s(rn)); return;
                }
                // FCMP/FCMPE
                if ((op & 0xFFE0FC1F) == 0x1E602000) {
                    if (ftype) {
                        double a = read_fp_d(rn), b = read_fp_d(rm);
                        if (std::isnan(a) || std::isnan(b)) {
                            cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(1);
                        } else if (a == b) {
                            cpu.set_flag_n(0); cpu.set_flag_z(1); cpu.set_flag_c(0); cpu.set_flag_v(0);
                        } else if (a < b) {
                            cpu.set_flag_n(1); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(0);
                        } else {
                            cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(1); cpu.set_flag_v(0);
                        }
                    } else {
                        float a = read_fp_s(rn), b = read_fp_s(rm);
                        if (std::isnan(a) || std::isnan(b)) {
                            cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(1);
                        } else if (a == b) {
                            cpu.set_flag_n(0); cpu.set_flag_z(1); cpu.set_flag_c(0); cpu.set_flag_v(0);
                        } else if (a < b) {
                            cpu.set_flag_n(1); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(0);
                        } else {
                            cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(1); cpu.set_flag_v(0);
                        }
                    }
                    return;
                }
                // FCMP with #0.0
                if ((op & 0xFFE0FC1F) == 0x1E602008) {
                    if (ftype) {
                        double a = read_fp_d(rn);
                        if (std::isnan(a)) { cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(1); }
                        else if (a == 0.0) { cpu.set_flag_n(0); cpu.set_flag_z(1); cpu.set_flag_c(0); cpu.set_flag_v(0); }
                        else if (a < 0.0) { cpu.set_flag_n(1); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(0); }
                        else { cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(1); cpu.set_flag_v(0); }
                    } else {
                        float a = read_fp_s(rn);
                        if (std::isnan(a)) { cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(1); }
                        else if (a == 0.0f) { cpu.set_flag_n(0); cpu.set_flag_z(1); cpu.set_flag_c(0); cpu.set_flag_v(0); }
                        else if (a < 0.0f) { cpu.set_flag_n(1); cpu.set_flag_z(0); cpu.set_flag_c(0); cpu.set_flag_v(0); }
                        else { cpu.set_flag_n(0); cpu.set_flag_z(0); cpu.set_flag_c(1); cpu.set_flag_v(0); }
                    }
                    return;
                }
                // FCVTZS/FCVTZU
                if ((op & 0x7F3F0000) == 0x1E780000 || (op & 0x7F3F0000) == 0x1E790000) {
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf;
                    if (ftype) {
                        double a = read_fp_d(rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : (uint64_t)a;
                            cpu.regs[rd] = is_64bit ? v : (uint32_t)v;
                        } else {
                            int64_t v = (int64_t)a;
                            cpu.regs[rd] = is_64bit ? (uint64_t)v : (uint32_t)(int32_t)v;
                        }
                    } else {
                        float a = read_fp_s(rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : (uint64_t)a;
                            cpu.regs[rd] = is_64bit ? v : (uint32_t)v;
                        } else {
                            int64_t v = (int64_t)a;
                            cpu.regs[rd] = is_64bit ? (uint64_t)v : (uint32_t)(int32_t)v;
                        }
                    }
                    return;
                }
                // SCVTF/UCVTF
                if ((op & 0x7F3F0000) == 0x1E620000 || (op & 0x7F3F0000) == 0x1E630000) {
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf;
                    if (ftype) {
                        if (is_unsigned) {
                            uint64_t v = is_64bit ? cpu.regs[rn] : (uint32_t)cpu.regs[rn];
                            write_fp_d(rd, (double)v);
                        } else {
                            int64_t v = is_64bit ? (int64_t)cpu.regs[rn] : (int32_t)cpu.regs[rn];
                            write_fp_d(rd, (double)v);
                        }
                    } else {
                        if (is_unsigned) {
                            uint64_t v = is_64bit ? cpu.regs[rn] : (uint32_t)cpu.regs[rn];
                            write_fp_s(rd, (float)v);
                        } else {
                            int64_t v = is_64bit ? (int64_t)cpu.regs[rn] : (int32_t)cpu.regs[rn];
                            write_fp_s(rd, (float)v);
                        }
                    }
                    return;
                }
                // FCSEL
                if ((op & 0xFF200C00) == 0x1E200000 && ((op >> 21) & 1) == 0 &&
                    ((op >> 10) & 0xF) == 0xC) {
                    uint8_t cond = (op >> 12) & 0xF;
                    if (ftype) {
                        double r = cond_true(cond, cpu.pstate) ? read_fp_d(rn) : read_fp_d(rm);
                        write_fp_d(rd, r);
                    } else {
                        float r = cond_true(cond, cpu.pstate) ? read_fp_s(rn) : read_fp_s(rm);
                        write_fp_s(rd, r);
                    }
                    return;
                }
                // FMADD/FMSUB
                if ((op & 0xFF200000) == 0x1F000000) {
                    uint8_t ra = (op >> 10) & 0x1F;
                    bool sub = (op >> 15) & 1;
                    if (ftype) {
                        double a = read_fp_d(rn), b = read_fp_d(rm), c = read_fp_d(ra);
                        write_fp_d(rd, sub ? (c - a * b) : (c + a * b));
                    } else {
                        float a = read_fp_s(rn), b = read_fp_s(rm), c = read_fp_s(ra);
                        write_fp_s(rd, sub ? (c - a * b) : (c + a * b));
                    }
                    return;
                }
                // Unknown FP instruction — NOP (don't crash)
                (void)sf; (void)rm;
                return;
            }

            default:
                // Not yet handled by the decoder switch — fall through
                // to the legacy if-chain below.
                break;
        }
    }

    // If we reach here, the instruction was not recognized by the decoder
    // switch above. Bail with a DecodeError so the caller can report the
    // PC and the offending instruction word.
    throw DecodeError(cpu.pc, inst);
}

// ---------------------------------------------------------------------------
// Linux AArch64 syscall layer
//   syscall number in x8, args in x0..x5, return value in x0
// ---------------------------------------------------------------------------

} // namespace arm64emu

// End of interpreter.cpp
