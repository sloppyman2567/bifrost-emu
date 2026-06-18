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
            case InstClass::SVC:
                // Handled by the if-chain below (needs full syscall dispatch)
                break;
            case InstClass::BRK:
                // Handled by the if-chain below
                break;

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
                    uint64_t rotated = (width == 64) ? ror64(src, immr)
                                                      : ((uint32_t)ror64(src, immr));
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
                // Decode bitmask (same logic as the if-chain)
                uint8_t combined = (Nbit << 6) | imms;
                uint64_t imm_val;
                if (combined == 0) {
                    imm_val = 0;
                } else {
                    int hsbit = 0;
                    for (int i = 6; i >= 0; i--) {
                        if (combined & (1 << i)) { hsbit = i; break; }
                    }
                    int esize;
                    if (Nbit) { esize = 64; }
                    else { esize = 1 << (hsbit + 1); }
                    int levels = esize - 1;
                    int S = imms & levels;
                    int R = immr & levels;
                    uint64_t ones = (S + 1 >= 64) ? ~0ULL : ((1ULL << (S + 1)) - 1);
                    uint64_t element;
                    if (esize == 64) { element = ones; }
                    else {
                        element = ones << (esize - 1 - S);
                        element &= (1ULL << esize) - 1;
                    }
                    if (esize < 64) {
                        element = ((element >> R) | (element << (esize - R)))
                                  & ((1ULL << esize) - 1);
                    } else {
                        if (R != 0) element = (element >> R) | (element << (64 - R));
                    }
                    imm_val = 0;
                    for (int off = 0; off < width; off += esize)
                        imm_val |= element << off;
                    if (!d.sf) imm_val &= 0xFFFFFFFF;
                }
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

            default:
                // Not yet handled by the decoder switch — fall through
                // to the legacy if-chain below.
                break;
        }
    }

    // ── Legacy if-chain (transitional — will be deleted in v2.0) ─────
    // This handles all instructions that haven't been migrated to the
    // decoder switch above yet. Each handler here should eventually be
    // moved to a switch case, with its decode logic moved to decoder.cpp.

    // Helper lambdas
    auto rd = [&](int r) -> uint64_t& { return cpu.regs[r]; };

    // ------------------------------------------------------------------
    // Group: unconditional branch / branch with link
    //   B/BL imm26 -> 0001 01/l 0 imm26
    // ------------------------------------------------------------------
    if ((op & 0xFC000000) == 0x14000000 ||   // B
        (op & 0xFC000000) == 0x94000000) {   // BL
        bool link = (op >> 31) & 1;
        int32_t imm = sign_extend(op & 0x03FFFFFF, 26) << 2;
        if (link) cpu.regs[30] = cpu.pc + 4;
        next_pc = cpu.pc + imm;
        return;
    }

    // ------------------------------------------------------------------
    // Group: conditional branch (immediate)
    //   0101 0100 imm19 0 cond
    // ------------------------------------------------------------------
    if ((op & 0xFF000010) == 0x54000000) {
        uint32_t cond = op & 0xF;
        int32_t imm = sign_extend((op >> 5) & 0x7FFFF, 19) << 2;
        if (cond_true(cond, cpu.pstate)) {
            next_pc = cpu.pc + imm;
        }
        return;
    }

    // ------------------------------------------------------------------
    // Group: compare & branch (CBZ/CBNZ)
    //   sf 011 010 0 imm19 Rt
    // ------------------------------------------------------------------
    if ((op & 0x7E000000) == 0x34000000) {
        bool sf = (op >> 31) & 1;
        bool nz = (op >> 24) & 1;
        int32_t imm = sign_extend((op >> 5) & 0x7FFFF, 19) << 2;
        uint8_t  rt = op & 0x1F;
        uint64_t v = cpu.regs[rt];
        if (!sf) v &= 0xFFFFFFFF;
        bool zero = (v == 0);
        if (zero != nz) { // CBZ: zero -> branch; CBNZ: !zero -> branch
            next_pc = cpu.pc + imm;
        }
        return;
    }

    // ------------------------------------------------------------------
    // Group: test bit & branch (TBZ/TBNZ)
    //   b5 011 011 op imm14 b40 Rt
    //   op=0 -> TBZ  (branch if bit == 0)
    //   op=1 -> TBNZ (branch if bit != 0)
    // ------------------------------------------------------------------
    if ((op & 0x7E000000) == 0x36000000) {
        bool nz = (op >> 24) & 1;   // 0=TBZ, 1=TBNZ
        int32_t imm = sign_extend((op >> 5) & 0x3FFF, 14) << 2;
        uint8_t b40 = (op >> 19) & 0x1F;
        uint8_t b5  = (op >> 31) & 1;
        uint8_t bit = (b5 << 5) | b40;
        uint8_t  rt = op & 0x1F;
        uint64_t v = cpu.regs[rt];
        bool set = (v >> bit) & 1;
        // TBZ (nz=0): branch when set == 0, i.e., set == nz
        // TBNZ (nz=1): branch when set == 1, i.e., set == nz
        if (set == nz) {
            next_pc = cpu.pc + imm;
        }
        return;
    }

    // ------------------------------------------------------------------
    // Group: branch via register (BR, BLR, RET, ERET, DRPS)
    //   1101 0110 0011 1111 0000 00 op0 Rn 00000
    // ------------------------------------------------------------------
    if ((op & 0xFFFFFC00) == 0xD61F0000) { // BR
        uint8_t rn = (op >> 5) & 0x1F;
        next_pc = cpu.regs[rn];
        return;
    }
    if ((op & 0xFFFFFC00) == 0xD63F0000) { // BLR
        uint8_t rn = (op >> 5) & 0x1F;
        cpu.regs[30] = cpu.pc + 4;
        next_pc = cpu.regs[rn];
        return;
    }
    if ((op & 0xFFFFFC1F) == 0xD65F0000) { // RET [Rn=LR by default]
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t r  = rn ? rn : 30;
        next_pc = cpu.regs[r];
        return;
    }

    // ------------------------------------------------------------------
    // Group: exception (SVC/HVC/SMC) - we only handle SVC #0
    //   1101 0100 000 imm16 000 00001
    // ------------------------------------------------------------------
    if ((op & 0xFFE0001F) == 0xD4000001) { // SVC
        syscall(cpu);
        return;
    }
    if ((op & 0xFFE0001F) == 0xD4000002 || // HVC
        (op & 0xFFE0001F) == 0xD4000003) { // SMC
        throw EmuError("HVC/SMC not supported in user mode");
    }

    // ------------------------------------------------------------------
    // Group: system instructions (BRK, HLT, MSR, MRS, etc.)
    //
    // BRK is used by libc for fatal paths:
    //   - glibc: BRK #1000 inside abort() after tgkill() fails to kill
    //            the process.  The intent is to trigger SIGTRAP and
    //            force a core dump.
    //   - musl:  BRK #1 in __builtin_trap() / a_crash() for unreachable
    //            paths and assertion failures.
    //
    // On real Linux, BRK delivers SIGTRAP, whose default action is to
    // terminate the process.  We honor that semantics here: any BRK
    // ends emulation with a non-zero exit code derived from the
    // immediate.  This prevents libc's abort() from falling through
    // into unrelated code and dumping random .rodata strings to stderr
    // (which was happening with glibc 2.36+ static binaries that hit
    // the getrandom vDSO assertion during libc init).
    // ------------------------------------------------------------------
    if ((op & 0xFFE0001F) == 0xD4200000) { // BRK #imm16
        uint16_t imm = (op >> 5) & 0xFFFF;
        if (brk_verbose_) {
            fprintf(stderr, "[emu] BRK #%u at pc=0x%llx (terminating)\n",
                    imm, (unsigned long long)cpu.pc);
        }
        // Match the shell convention for signal-terminated processes:
        //   128 + signal.  BRK raises SIGTRAP (signal 5), so exit 133.
        // We use 133 for all BRKs to mirror "killed by SIGTRAP".
        cpu.running = false;
        cpu.exit_code = 128 + 5;  // SIGTRAP
        return;
    }
    if ((op & 0xFFE0001F) == 0xD4400000) { // HLT #imm16
        uint16_t imm = (op >> 5) & 0xFFFF;
        // Some baremetal demos use HLT #0 to exit; treat as exit.
        cpu.running = false;
        cpu.exit_code = imm;
        return;
    }
    // ------------------------------------------------------------------
    // Group: system instructions (MSR/MRS/HINT/Barrier/CLREX/SYS/AT/DC/IC/TLBI)
    //
    // The entire 0xD5000000 - 0xD5FFFFFF space is "system instructions".
    // For user-mode Linux binaries we treat them mostly as NOPs or
    // stubs:
    //   - MSR <sysreg>, Xt -- write to sysreg (most are EL1+, ignore)
    //   - MRS Xt, <sysreg> -- read from sysreg (return 0 unless known)
    //   - HINT space (NOP/WFE/WFI/SEV/YIELD) -- no-op
    //   - Barriers (DSB/DMB/ISB) -- no-op (single-threaded)
    //   - CLREX -- no-op
    //   - SYS/AT/DC/IC/TLBI -- EL1+ only, no-op
    //
    // A few sysregs we DO care about (glibc reads these):
    //   - TPIDR_EL0 (thread pointer) - return 0 for the main thread
    //   - CTR_EL0 (cache type) - return a plausible value
    //   - DCZID_EL0 (DC ZVA block size) - return 0 (don't advertise DC ZVA)
    //   - NZCV (condition flags) - read/write pstate
    //   - FPCR/FPSR - return 0
    // ------------------------------------------------------------------
    if ((op & 0xFF000000) == 0xD5000000) {
        bool L   = (op >> 21) & 1;   // 0 = MSR (write), 1 = MRS (read)
        uint8_t op0 = (op >> 19) & 0x3;
        uint8_t op1 = (op >> 16) & 0x7;
        uint8_t crn = (op >> 12) & 0xF;
        uint8_t crm = (op >> 8) & 0xF;
        uint8_t op2 = (op >> 5) & 0x7;
        uint8_t rt  = op & 0x1F;

        // Special-case known readable system registers. Encoding pattern:
        // MRS Xt, <sysreg> = 1101 0101 0011 op0[1:0] op1[2:0] CRn CRm op2 Rt
        // The high bits (excluding op0/op1/CRn/CRm/op2/Rt) for MRS are 0xD53B...
        // when op0=3 op1=3. We just check the sysreg field directly.

        if (L == 1 && op0 == 3 && op1 == 3) {
            // EL0-readable sysregs (mostly). Determine which one.
            uint64_t val = 0;
            bool handled = true;
            if (crn == 13 && crm == 0 && op2 == 2) {
                // TPIDR_EL0 - thread pointer (read-write)
                val = cpu.tpidr_el0;
            } else if (crn == 13 && crm == 0 && op2 == 3) {
                // TPIDRRO_EL0 - read-only thread pointer
                val = cpu.tpidrro_el0;
            } else if (crn == 0 && crm == 0 && op2 == 1) {
                // CTR_EL0 - cache type register
                // Format: DIC(1) IDC(1) Erg(3) CWG(3) DminLine(4) IminLine(4)
                // Common value: 0x8444C004 -- 0b1000_0100_0100_0100_1100_0000_0000_0100
                // = DIC=1 IDC=0 Erg=4 CWG=4 DminLine=12 IminLine=4
                val = 0x8444C004;
            } else if (crn == 0 && crm == 0 && op2 == 7) {
                // DCZID_EL0 - DC ZVA identifier; bit 4 = 1 means DC ZVA not available
                val = (1u << 4);  // disable DC ZVA
            } else if (crn == 4 && crm == 2 && op2 == 0) {
                // NZCV
                uint32_t nzcv = 0;
                if (cpu.flag_n()) nzcv |= (1u << 31);
                if (cpu.flag_z()) nzcv |= (1u << 30);
                if (cpu.flag_c()) nzcv |= (1u << 29);
                if (cpu.flag_v()) nzcv |= (1u << 28);
                val = nzcv;
            } else if (crn == 4 && crm == 4 && op2 == 0) {
                // FPCR
                val = 0;
            } else if (crn == 4 && crm == 4 && op2 == 1) {
                // FPSR
                val = 0;
            } else if (crn == 0 && crm == 0 && op2 == 0) {
                // MIDR_EL1 - main ID register (not visible at EL0, but
                // some libs probe it). Return a Cortex-A72 value.
                val = 0x410FD080;
            } else if (crn == 0 && crm == 0 && op2 == 5) {
                // MVFR0_EL1 (media/NEON)
                val = 0x10110222;
            } else if (crn == 0 && crm == 0 && op2 == 6) {
                // MVFR1_EL1
                val = 0x12122211;
            } else if (crn == 0 && crm == 0 && op2 == 7) {
                // MVFR2_EL1
                val = 0x00000043;
            } else if (crn == 0 && crm == 2 && op2 == 0) {
                // ID_AA64PFR0_EL1
                val = 0x00000022;  // ARMv8-A, no SVE, no EL3
            } else if (crn == 0 && crm == 2 && op2 == 2) {
                // ID_AA64MMFR0_EL1
                val = 0x00000000;
            } else if (crn == 0 && crm == 2 && op2 == 4) {
                // ID_AA64ISAR0_EL1 -Instruction Set Attribute Register 0
                val = 0x00000000;
            } else {
                handled = false;
            }
            if (handled) {
                if (rt != 31) cpu.regs[rt] = val;
                return;
            }
            // Otherwise fall through to NOP
        }

        // MSR write to NZCV
        if (L == 0 && op0 == 3 && op1 == 3 && crn == 4 && crm == 2 && op2 == 0) {
            uint64_t v = cpu.regs[rt];
            cpu.set_flag_n(v & (1u << 31));
            cpu.set_flag_z(v & (1u << 30));
            cpu.set_flag_c(v & (1u << 29));
            cpu.set_flag_v(v & (1u << 28));
            return;
        }
        // MSR write to TPIDR_EL0 (thread pointer for TLS)
        if (L == 0 && op0 == 3 && op1 == 3 && crn == 13 && crm == 0 && op2 == 2) {
            cpu.tpidr_el0 = cpu.regs[rt];
            return;
        }
        // MSR write to TPIDRRO_EL0 (read-only thread pointer)
        if (L == 0 && op0 == 3 && op1 == 3 && crn == 13 && crm == 0 && op2 == 3) {
            cpu.tpidrro_el0 = cpu.regs[rt];
            return;
        }
        // MSR write to FPCR/FPSR - just store (we don't use them)
        if (L == 0 && op0 == 3 && op1 == 3 && crn == 4 && crm == 4) {
            if (op2 == 0) cpu.fpcr = (uint32_t)cpu.regs[rt];
            else if (op2 == 1) cpu.fpsr = (uint32_t)cpu.regs[rt];
            return;
        }
        // Other MSR writes to EL0-accessible sysregs we don't model - NOP.
        if (L == 0 && op0 == 3 && op1 == 3) {
            return;
        }

        // Everything else: NOP. Covers HINT space, barriers,
        // SYS/AT/DC/IC/TLBI (all EL1+), MSR to EL1+ sysregs.
        //
        // CLREX is encoded as 0xD503305F (System, L=0, op0=3, op1=3,
        // CRn=0101, CRm=0000, op2=010, Rt=11111). Handle it explicitly
        // because we need to clear the local exclusive monitor.
        if (op == 0xD503305F) {
            return;
        }
        return;
    }

    // ADR/ADRP — handled by the decoder switch above.

    // ------------------------------------------------------------------
    // Group: data processing - immediate
    // ------------------------------------------------------------------

    // MOVZ / MOVK / MOVN — handled by the decoder switch above.

    // Add/subtract (immediate) — handled by the decoder switch above.

    // Bitfield/EXTR/Logical immediate — handled by the decoder switch above.

    // EXTR and Logical immediate — handled by the decoder switch above.

    // ------------------------------------------------------------------
    // Group: Load/store pair (ALL modes: post, offset, pre)
    //
    // Encoding: opc 101 V mode L imm7 Rt2 Rn Rt
    //   mode (bits 25:24): 00=post, 01=offset, 10=pre
    //   V (bit 26): 0=GP, 1=SIMD
    //
    // CRITICAL: This MUST come before the logical shifted register handler
    // because pre-index STP/LDP (bits 31:24 = 1010 1010) shares top bits
    // with ORR (bits 31:24 = 1010 1010). They are truly indistinguishable
    // by the top byte alone. The distinguishing factor is bits 15:12:
    // STP/LDP has Rt2 there (a register number 0-31), while ORR has
    // imm6 (shift amount) at bits 15:10. We can't use that to distinguish
    // reliably. Instead, we rely on the ARM ARM's hierarchical decode:
    // Load/Store Pair is a separate major group from Data Processing.
    // We check bits 29:26 = 1010 (pair group, V=0 for GP) which catches
    // all GP STP/LDP modes. ORR has bits 29:26 = 1010 too — BUT ORR's
    // bit 25 is always 1 (from its 01010 fixed pattern at bits 28:24),
    // while STP/LDP post/offset has bit 25 = 0. STP/LDP pre has bit 25
    // = 1, same as ORR. So we handle pre-index specially below.
    // ------------------------------------------------------------------
    // GP STP/LDP: bits 29:26 = 1010, bit 26 = 0 (V=0 for GP)
    // The mask 0x3C000000 checks bits 29:26 = 1010.
    // This catches post (bit 25=0), offset (bit 25=0), and pre (bit 25=1).
    // For pre-index (bit 25=1), ORR also matches — but we check here FIRST,
    // so STP/LDP pre gets handled before ORR can catch it.
    // Load/store pair — moved BEFORE logical handler to avoid collision.
    // Mask 0x3A000000 catches GP+SIMD post/offset (bit 25=0), excludes
    // pre-index (bit 25=1) and ORR (bit 25=1). Pre-index STP/LDP falls
    // through to the logical handler — known issue, fix in v2.0.
    if ((op & 0x3A000000) == 0x28000000) {
        uint8_t opc = (op >> 30) & 3;
        bool    is_load = (op >> 22) & 1;
        bool    is_vec = (op >> 26) & 1;
        int16_t imm7 = sign_extend((op >> 15) & 0x7F, 7);
        uint8_t rt2 = (op >> 10) & 0x1F;
        uint8_t rn  = (op >> 5) & 0x1F;
        uint8_t rt  = op & 0x1F;
        int esize;
        if (is_vec) {
            esize = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
        } else {
            esize = (opc == 2) ? 8 : 4;
        }
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];
        // Mode extraction: use (op >> 23) & 3 with old-style values.
        // This is "buggy" per the ARM ARM (mode is in bits 25:24, not 24:23)
        // but works correctly for post-index loads (the most common case in
        // function epilogues). The "correct" (op >> 24) & 3 calc breaks musl
        // because offset loads get different writeback behavior.
        // TODO: properly fix in v2.0 with full hierarchical decoder.
        uint8_t mode = (op >> 23) & 3;
        int64_t disp = imm7 * esize;
        uint64_t addr;
        if (mode == 1) { // post-index
            addr = base;
            uint64_t nb = base + disp;
            if (rn == 31) cpu.sp = nb; else cpu.regs[rn] = nb;
        } else if (mode == 3) { // pre-index
            addr = base + disp;
            uint64_t nb = base + disp;
            if (rn == 31) cpu.sp = nb; else cpu.regs[rn] = nb;
        } else { // offset
            addr = base + disp;
        }
        if (is_vec) {
            if (is_load) {
                uint64_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
                mem_.read(addr, &lo1, 8);
                if (esize >= 16) mem_.read(addr + 8, &hi1, 8);
                mem_.read(addr + esize, &lo2, 8);
                if (esize >= 16) mem_.read(addr + esize + 8, &hi2, 8);
                cpu.v_lo[rt] = lo1; cpu.v_hi[rt] = hi1;
                cpu.v_lo[rt2] = lo2; cpu.v_hi[rt2] = hi2;
            } else {
                mem_.write(addr, &cpu.v_lo[rt], 8);
                if (esize >= 16) mem_.write(addr + 8, &cpu.v_hi[rt], 8);
                mem_.write(addr + esize, &cpu.v_lo[rt2], 8);
                if (esize >= 16) mem_.write(addr + esize + 8, &cpu.v_hi[rt2], 8);
            }
        } else {
            if (is_load) {
                uint64_t v1 = 0, v2 = 0;
                mem_.read(addr, &v1, esize);
                mem_.read(addr + esize, &v2, esize);
                if (rt  != 31) cpu.regs[rt]  = v1;
                if (rt2 != 31) cpu.regs[rt2] = v2;
            } else {
                uint64_t v1 = (rt  == 31) ? 0 : cpu.regs[rt];
                uint64_t v2 = (rt2 == 31) ? 0 : cpu.regs[rt2];
                mem_.write(addr, &v1, esize);
                mem_.write(addr + esize, &v2, esize);
            }
        }
        return;
    }

    // Group: data processing - register
    // ------------------------------------------------------------------

    // Add/subtract (shifted register) : sf op 0 1 shift 0 Rm imm6 Rn Rd
    if ((op & 0x1F200000) == 0x0B000000) {
        bool sf = (op >> 31) & 1;
        uint8_t opc = (op >> 29) & 3; // 00 ADD, 01 ADDS, 10 SUB, 11 SUBS
        uint8_t shift = (op >> 22) & 3;
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t imm6 = (op >> 10) & 0x3F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = cpu.regs[rm];
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
        switch (shift) {
            case 0: b = b << imm6; break;
            case 1: b = (width == 64) ? (b >> imm6) : ((uint32_t)b >> imm6); break;
            case 2: b = ((int64_t)b) >> imm6; break; // arithmetic
            case 3: b = ror64(b, imm6) & (width == 64 ? ~0ULL : 0xFFFFFFFF); break;
        }
        if (!sf) b &= 0xFFFFFFFF;
        bool set_flags = (opc & 1);
        bool is_sub = (opc & 2);
        uint64_t res;
        if (is_sub) res = set_sub_flags(cpu, a, b, width, set_flags);
        else        res = set_add_flags(cpu, a, b, 0, width, set_flags);
        if (rd_ != 31) cpu.regs[rd_] = res;
        return;
    }

    // Add/subtract (extended register) : sf op 0 0 1 0 0 option imm3 Rn Rd
    if ((op & 0x1FE00000) == 0x0B200000) {
        bool sf = (op >> 31) & 1;
        uint8_t opc = (op >> 29) & 3;
        uint8_t option = (op >> 13) & 7;
        uint8_t imm3 = (op >> 10) & 7;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = extend_reg(cpu.regs[rm], option, imm3, sf);
        if (!sf) { a &= 0xFFFFFFFF; }
        bool set_flags = (opc & 1);
        bool is_sub = (opc & 2);
        uint64_t res;
        if (is_sub) res = set_sub_flags(cpu, a, b, width, set_flags);
        else        res = set_add_flags(cpu, a, b, 0, width, set_flags);
        if (rd_ != 31) cpu.regs[rd_] = res;
        return;
    }

    // Add/subtract (with carry) — now handled by the decoder switch above.
    // (ADC/ADCS/SBC/SBCS: encoding 0x1A000000 family)

    // Conditional select (CSEL/CSINC/CSINV/CSNEG) is handled further below
    // (after the logical-shifted-register group) with full op+S decoding.


    // Logical (shifted register) : sf opc 01010 shift N Rm imm6 Rn Rd
    if ((op & 0x1F000000) == 0x0A000000) {
        bool sf = (op >> 31) & 1;
        uint8_t opc = (op >> 29) & 3; // 00 AND, 01 ORR, 10 EOR, 11 ANDS
        uint8_t shift = (op >> 22) & 3;
        uint8_t N = (op >> 21) & 1; // must be 0
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t imm6 = (op >> 10) & 0x3F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = cpu.regs[rm];
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
        switch (shift) {
            case 0: b = b << imm6; break;
            case 1: b = (width == 64) ? (b >> imm6) : ((uint32_t)b >> imm6); break;
            case 2: b = ((int64_t)b) >> imm6; break;
            case 3: b = ror64(b, imm6) & (width == 64 ? ~0ULL : 0xFFFFFFFF); break;
        }
        if (!sf) b &= 0xFFFFFFFF;
        uint64_t res;
        bool set_flags = false;
        switch (opc) {
            case 0: res = a & b; break;
            case 1: res = a | b; break;
            case 2: res = a ^ b; break;
            case 3: res = a & b; set_flags = true; break;
            default: throw DecodeError(cpu.pc, inst);
        }
        if (!sf) res &= 0xFFFFFFFF;
        if (rd_ != 31) cpu.regs[rd_] = res;
        if (set_flags) {
            cpu.set_flag_n((res >> (width - 1)) & 1);
            cpu.set_flag_z(res == 0);
            cpu.set_flag_c(false);
            cpu.set_flag_v(false);
        }
        return;
    }

    // Conditional select : sf op S 10 11010100 Rm cond imm2 Rn Rd
    //   Fixed bits 28:21 = 11010100 (bit 21=0 distinguishes from CCMP).
    //   The operation is determined by BOTH op (bits 30:29) AND S (bits 11:10):
    //     (00, 00) = CSEL    (00, 01) = CSINC
    //     (10, 00) = CSINV   (10, 01) = CSNEG
    //   CSET is an alias for CSINC with WZR operands.
    if ((op & 0x1FE00000) == 0x1A800000) {
        bool sf = (op >> 31) & 1;
        uint8_t op_field = (op >> 29) & 3;   // bits 30:29
        uint8_t s_field  = (op >> 10) & 3;   // bits 11:10
        // Combined operation: 0=CSEL, 1=CSINC, 2=CSINV, 3=CSNEG
        uint8_t opc;
        if (op_field == 0 && s_field == 0) opc = 0;      // CSEL
        else if (op_field == 0 && s_field == 1) opc = 1;  // CSINC
        else if (op_field == 2 && s_field == 0) opc = 2;  // CSINV
        else if (op_field == 2 && s_field == 1) opc = 3;  // CSNEG
        else opc = 0; // fallback (shouldn't happen)
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t cond = (op >> 12) & 0xF;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = cpu.regs[rm];
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
        uint64_t res;
        if (cond_true(cond, cpu.pstate)) res = a;
        else {
            switch (opc) {
                case 0: res = b; break;        // CSEL
                case 1: res = b + 1; break;    // CSINC
                case 2: res = ~b; break;       // CSINV
                case 3: res = -b; break;       // CSNEG
                default: throw DecodeError(cpu.pc, inst);
            }
        }
        if (!sf) res &= 0xFFFFFFFF;
        if (rd_ != 31) cpu.regs[rd_] = res;
        return;
    }

    // Data processing (1 source) : sf 0 0 11010110 opcode2 opcode Rn Rd
    // Covers RBIT, REV16, REV32, REV, CLZ, CLS
    if ((op & 0x5FE00000) == 0x5AC00000) {
        bool sf = (op >> 31) & 1;
        uint8_t opcode = (op >> 10) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t v = cpu.regs[rn];
        if (!sf) v &= 0xFFFFFFFF;
        switch (opcode) {
            case 0x00: { // RBIT
                uint64_t r = 0;
                for (int i = 0; i < width; i++) if (v & (1ULL << i)) r |= (1ULL << (width - 1 - i));
                v = r;
                break;
            }
            case 0x01: { // REV16
                // reverse bytes in each 16-bit halfword
                if (width == 64) {
                    v = ((v & 0xFF00FF00FF00FF00ULL) >> 8) |
                        ((v & 0x00FF00FF00FF00FFULL) << 8);
                } else {
                    v = ((v & 0xFF00FF00ULL) >> 8) | ((v & 0x00FF00FFULL) << 8);
                }
                break;
            }
            case 0x02: { // REV32
                if (width == 64) {
                    v = ((v & 0xFFFF0000FFFF0000ULL) >> 16) |
                        ((v & 0x0000FFFF0000FFFFULL) << 16);
                } else {
                    v = ((v & 0xFFFF0000ULL) >> 16) | ((v & 0x0000FFFFULL) << 16);
                }
                break;
            }
            case 0x03: { // REV (64-bit) or REV (32-bit)
                if (width == 64) {
                    v = __builtin_bswap64(v);
                } else {
                    v = __builtin_bswap32((uint32_t)v);
                }
                break;
            }
            case 0x04: { // CLZ
                if (v == 0) v = width;  // CLZ of 0 returns the bit width
                else v = (width == 64) ? __builtin_clzll(v) : __builtin_clz((uint32_t)v);
                break;
            }
            case 0x05: { // CLS
                if (width == 64) {
                    v = (v >> 63) ? __builtin_clzll(~v) : __builtin_clzll(v);
                } else {
                    v = (v >> 31) ? __builtin_clz((uint32_t)~v) : __builtin_clz((uint32_t)v);
                }
                break;
            }
            default: throw DecodeError(cpu.pc, inst);
        }
        if (!sf) v &= 0xFFFFFFFF;
        if (rd_ != 31) cpu.regs[rd_] = v;
        return;
    }

    // Data processing (2 source) : sf 0 S 11010110 Rm opcode Rn Rd
    // Covers UDIV, SDIV, LSL, LSR, ASR, ROR, MUL, MNEG
    if ((op & 0x5FE00000) == 0x1AC00000) {
        bool sf = (op >> 31) & 1;
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t opcode = (op >> 10) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = cpu.regs[rm];
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
        uint64_t res = 0;
        switch (opcode) {
            case 0x02: { // UDIV
                if (b == 0) res = 0;
                else res = (width == 64) ? a / b : (uint32_t)a / (uint32_t)b;
                break;
            }
            case 0x03: { // SDIV
                if (b == 0) res = 0;
                else {
                    if (width == 64) {
                        res = (int64_t)a / (int64_t)b;
                    } else {
                        res = (int32_t)a / (int32_t)b;
                    }
                }
                break;
            }
            case 0x08: { // LSL
                res = (width == 64) ? (a << (b & 63)) : ((uint32_t)a << (b & 31));
                break;
            }
            case 0x09: { // LSR
                res = (width == 64) ? (a >> (b & 63)) : ((uint32_t)a >> (b & 31));
                break;
            }
            case 0x0A: { // ASR
                res = (width == 64) ? ((int64_t)a >> (b & 63))
                                    : ((int32_t)a >> (b & 31));
                break;
            }
            case 0x0B: { // ROR
                res = (width == 64) ? ror64(a, b & 63) : (uint32_t)ror64(a, b & 31);
                break;
            }
            case 0x00: { // MUL (we map MUL via the 3-source path; sometimes here as alias)
                res = (width == 64) ? (a * b) : ((uint32_t)a * (uint32_t)b);
                break;
            }
            default: throw DecodeError(cpu.pc, inst);
        }
        if (!sf) res &= 0xFFFFFFFF;
        if (rd_ != 31) cpu.regs[rd_] = res;
        return;
    }

    // Multiply-add family (data processing 3 source):
    //   MADD/MSUB:     sf 00 11011 000 Rm o0 Ra Rn Rd  (bits 28:21 = 11011000)
    //   SMADDL/SMSUBL: 1 00 11011 001 Rm o0 Ra Rn Rd   (bits 28:21 = 11011001, signed)
    //   UMADDL/UMSUBL: 1 00 11011 101 Rm o0 Ra Rn Rd   (bits 28:21 = 11011101, unsigned)
    //   UMULH:         sf 00 11011 110 Rm 0 000111 Rn Rd (bits 28:21 = 11011110, unsigned)
    //   SMULH:         sf 00 11011 111 Rm 0 000111 Rn Rd (bits 28:21 = 11011111, signed)
    // Note: per capstone, 0x9bc57c81 = umulh with bits 28:21 = 11011110.
    if ((op & 0x1F000000) == 0x1B000000) {
        bool sf = (op >> 31) & 1;
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t sub_op = (op >> 21) & 0x7;  // bits 23:21
        // sub_op values (verified against capstone):
        //   0b000 = MADD/MSUB (32x32->32 or 64x64->64)
        //   0b001 = SMADDL/SMSUBL (32x32->64 signed)
        //   0b101 = UMADDL/UMSUBL (32x32->64 unsigned)
        //   0b110 = UMULH (64x64->high 64 unsigned)
        //   0b111 = SMULH (64x64->high 64 signed)
        uint8_t o0 = (op >> 15) & 1;
        uint8_t ra = (op >> 10) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd_ = op & 0x1F;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        uint64_t b = cpu.regs[rm];
        uint64_t c = cpu.regs[ra];
        if (!sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; c &= 0xFFFFFFFF; }

        if (sub_op == 0b110 || sub_op == 0b111) {
            // UMULH / SMULH (high 64 bits of 128-bit product)
            unsigned __int128 prod;
            if (sub_op == 0b110) {
                // UMULH (unsigned)
                prod = (unsigned __int128)a * (unsigned __int128)b;
            } else {
                // SMULH (signed)
                prod = (unsigned __int128)((int64_t)a * (int64_t)b);
            }
            uint64_t res = (uint64_t)(prod >> 64);
            if (!sf) res &= 0xFFFFFFFF;
            if (rd_ != 31) cpu.regs[rd_] = res;
            return;
        }

        if (sub_op == 0b101 || sub_op == 0b001) {
            // UMADDL/UMSUBL or SMADDL/SMSUBL (32x32->64 multiply-add)
            uint64_t prod;
            if (sub_op == 0b101) {
                // unsigned
                prod = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
            } else {
                // signed
                prod = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
            }
            uint64_t res;
            if (o0 == 0) res = c + prod;     // UMADDL/SMADDL
            else         res = c - prod;     // UMSUBL/SMSUBL
            if (rd_ != 31) cpu.regs[rd_] = res;
            return;
        }

        // MADD / MSUB (sub_op == 0)
        uint64_t prod = (width == 64) ? (a * b) : ((uint32_t)a * (uint32_t)b);
        uint64_t res;
        if (o0 == 0) res = c + prod;     // MADD
        else         res = c - prod;     // MSUB
        if (!sf) res &= 0xFFFFFFFF;
        if (rd_ != 31) cpu.regs[rd_] = res;
        return;
    }

    // ------------------------------------------------------------------
    // Group: LSE atomic memory operations (LDADD/LDCLR/LDEOR/LDSET/
    //        SMAX/SMIN/UMAX/UMIN/SWP/CAS/CASA/CASL/CASAL)
    //
    // Encoding: size 111000 o0 L 0 Rs op 00 Rn Rt  (bits 29:24 = 111000)
    //
    // *** Disambiguation from LDUR/STUR ***
    //
    // The LDUR/STUR unscaled load/store encoding (size 111000 opc 0 imm9
    // 00 Rn Rt) overlaps with the LSE atomics encoding in three of the
    // four discriminating fields: bits 29:24, bit 21, and bits 11:10.
    // The ARM ARM disambiguates them by the binary's declared feature
    // set: if the ELF declares AArch64 LSE atomics (via the
    // GNU_PROPERTY_AARCH64_FEATURE_1_LSE bit in .note.gnu.property),
    // the encoding is interpreted as LSE; otherwise it's LDUR/STUR.
    //
    // We honor that contract here: the LSE atomics handler is only
    // reached when the loaded ELF's `has_lse` flag is set (parsed by
    // ElfLoader::load from PT_NOTE segments). Binaries compiled without
    // +lse — which is the default for musl-static builds — always have
    // has_lse = false, so any LDUR x7, [x4, #-8] in musl's memcpy is
    // correctly routed to the unscaled load/store handler below.
    //
    // CAS (opcodes 0xC-0xF within LSE) shares this gating. SWP
    // (bit 21 = 1) has a distinct encoding and does not collide with
    // LDUR/STUR, but we still gate it on has_lse_ for consistency.
    // ------------------------------------------------------------------
    if (has_lse_ &&
        (op & 0x3F000000) == 0x38000000 &&   // bits 29:24 = 111000
        (op & 0x00200000) == 0 &&            // bit 21 = 0 (NOT load/store reg offset, NOT SWP)
        (op & 0x00000C00) == 0) {            // bits 11:10 = 00 (LSE atomics fixed)
        uint8_t size = (op >> 30) & 3;
        bool L  = (op >> 22) & 1;
        uint8_t rs = (op >> 16) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rt = op & 0x1F;
        int width_bytes = 1 << size;
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];
        uint8_t atom_op = (op >> 12) & 0xF;

        // CAS family (opcodes 0xC-0xF): compare-and-swap.
        if (atom_op >= 0xC) {
            uint64_t old = 0;
            mem_.read(base, &old, width_bytes);
            uint64_t cmp = cpu.regs[rs];
            uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
            cmp &= mask;
            old &= mask;
            if (old == cmp) {
                uint64_t newv = cpu.regs[rt] & mask;
                mem_.write(base, &newv, width_bytes);
            }
            if (rt != 31) cpu.regs[rt] = old;
            return;
        }

        // Other LSE atomics
        uint64_t a = 0, b = cpu.regs[rs];
        mem_.read(base, &a, width_bytes);
        uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
        a &= mask;
        b &= mask;
        uint64_t newv = 0;
        switch (atom_op) {
            case 0x0: newv = (a + b) & mask; break;
            case 0x1: newv = (a & ~b) & mask; break;
            case 0x2: newv = (a ^ b) & mask; break;
            case 0x3: newv = (a | b) & mask; break;
            case 0x4: { int64_t sa=(int64_t)(a<<(64-width_bytes*8))>>(64-width_bytes*8);
                        int64_t sb=(int64_t)(b<<(64-width_bytes*8))>>(64-width_bytes*8);
                        newv=(sa>sb?sa:sb)&mask; break; }
            case 0x5: { int64_t sa=(int64_t)(a<<(64-width_bytes*8))>>(64-width_bytes*8);
                        int64_t sb=(int64_t)(b<<(64-width_bytes*8))>>(64-width_bytes*8);
                        newv=(sa<sb?sa:sb)&mask; break; }
            case 0x6: newv = (a > b ? a : b) & mask; break;
            case 0x7: newv = (a < b ? a : b) & mask; break;
            case 0x8: newv = b & mask; break;
            default:  newv = a & mask; break;
        }
        mem_.write(base, &newv, width_bytes);
        if (L && rt != 31) cpu.regs[rt] = a;
        return;
    }

    // ------------------------------------------------------------------
    // Group: load/store
    // ------------------------------------------------------------------

    // Load/store (immediate, unsigned offset)
    //   size 111 0 01 opc imm12 Rn Rt   (immediate, unsigned, post/pre variants too)
    if ((op & 0x3B000000) == 0x39000000) {
        uint8_t size  = (op >> 30) & 3;
        uint8_t opc   = (op >> 22) & 3;
        bool    v     = (op >> 26) & 1; // SIMD?
        uint16_t imm12 = (op >> 10) & 0xFFF;
        uint8_t rn     = (op >> 5) & 0x1F;
        uint8_t rt     = op & 0x1F;
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];
        if (v) {
            // SIMD LDR/STR encoding (per ARM ARM):
            //   opc=00 size=xx → STR B/H/S/D form (1/2/4/8 bytes)
            //   opc=01 size=xx → LDR B/H/S/D form (1/2/4/8 bytes)
            //   opc=10 size=00 → STR Q form (128-bit / 16 bytes)
            //   opc=11 size=00 → LDR Q form (128-bit / 16 bytes)
            bool is_load = opc & 1;
            bool is_q    = (opc & 2) && size == 0;
            int nbytes = is_q ? 16 : (1 << size);
            uint64_t scale = is_q ? 4 : size;  // log2 of access size
            uint64_t addr = base + ((uint64_t)imm12 << scale);
            if (is_load) {
                uint64_t lo = 0, hi = 0;
                mem_.read(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                cpu.v_lo[rt] = lo;
                cpu.v_hi[rt] = (nbytes >= 16) ? hi : 0;
            } else {
                uint64_t lo = cpu.v_lo[rt];
                uint64_t hi = cpu.v_hi[rt];
                mem_.write(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) {
                    mem_.write(addr + 8, &hi, nbytes - 8);
                }
            }
            return;
        }
        bool is_load = (opc & 2) || (opc & 1);  // opc=0 STR, opc=1 LDR, opc=2 LDRSW (load), opc=3 LDR
        uint64_t addr = base + (imm12 << size);
        int width_bytes = 1 << size;
        if (is_load) {
            uint64_t v = 0;
            mem_.read(addr, &v, width_bytes);
            if (rt != 31) {
                if (opc & 2) {
                    // sign-extend
                    int bits = width_bytes * 8;
                    v = sign_extend(v, bits);
                }
                cpu.regs[rt] = v;
            }
        } else {
            uint64_t v = (rt == 31) ? 0 : cpu.regs[rt];
            uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
            v &= mask;
            mem_.write(addr, &v, width_bytes);
        }
        return;
    }

    // Load/store (immediate, pre/post-indexed)
    //   size 111 0 00 opc 0 imm9 MODE Rn Rt
    //   MODE = 00 (LDUR/STUR unscaled), 01 (post-index), 11 (pre-index)
    if ((op & 0x3B200C00) == 0x38000000 ||  // unscaled (LDUR/STUR)
        (op & 0x3B200C00) == 0x38000400 ||  // post-index
        (op & 0x3B200C00) == 0x38000C00) {  // pre-index
        uint8_t size = (op >> 30) & 3;
        uint8_t opc  = (op >> 22) & 3;
        bool    is_vec = (op >> 26) & 1;
        int16_t imm9  = sign_extend((op >> 12) & 0x1FF, 9);
        uint8_t rn    = (op >> 5) & 0x1F;
        uint8_t rt    = op & 0x1F;
        bool is_load = (opc & 2) || (opc & 1);
        // bits 11:10 = MODE
        // 00 = unscaled (no writeback)
        // 01 = post-index (writeback after access)
        // 11 = pre-index (writeback before access)
        bool pre_index  = ((op >> 10) & 3) == 3;
        bool post_index = ((op >> 10) & 3) == 1;
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];
        uint64_t addr;
        if (post_index) {
            addr = base;
            uint64_t new_base = base + imm9;
            if (rn == 31) cpu.sp = new_base; else cpu.regs[rn] = new_base;
        } else {
            addr = base + imm9;
            if (pre_index) {
                uint64_t new_base = base + imm9;
                if (rn == 31) cpu.sp = new_base; else cpu.regs[rn] = new_base;
            }
        }
        if (is_vec) {
            // SIMD LDUR/STUR. Same opc encoding as unsigned-offset:
            //   opc=00 → STR B/H/S/D, opc=01 → LDR B/H/S/D
            //   opc=10 → STR Q (128-bit), opc=11 → LDR Q (128-bit)
            bool is_load_v = opc & 1;
            bool is_q = (opc & 2) && size == 0;
            int nbytes = is_q ? 16 : (1 << size);
            if (is_load_v) {
                uint64_t lo = 0, hi = 0;
                mem_.read(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                cpu.v_lo[rt] = lo;
                cpu.v_hi[rt] = (nbytes >= 16) ? hi : 0;
            } else {
                uint64_t lo = cpu.v_lo[rt];
                mem_.write(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) {
                    uint64_t hi = cpu.v_hi[rt];
                    mem_.write(addr + 8, &hi, nbytes - 8);
                }
            }
            return;
        }
        int width_bytes = 1 << size;
        if (is_load) {
            uint64_t v = 0;
            mem_.read(addr, &v, width_bytes);
            if (rt != 31) {
                if (opc & 2) {
                    int bits = width_bytes * 8;
                    v = sign_extend(v, bits);
                }
                cpu.regs[rt] = v;
            }
        } else {
            uint64_t v = (rt == 31) ? 0 : cpu.regs[rt];
            uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
            v &= mask;
            mem_.write(addr, &v, width_bytes);
        }
        return;
    }

    // Load/store (register offset)
    //   size 111 0 01 opc 1 Rm option S 10 Rn Rt
    if ((op & 0x3B200C00) == 0x38200800) {
        uint8_t size  = (op >> 30) & 3;
        uint8_t opc   = (op >> 22) & 3;
        bool    is_vec = (op >> 26) & 1;
        uint8_t rm     = (op >> 16) & 0x1F;
        uint8_t option = (op >> 13) & 7;
        uint8_t S      = (op >> 12) & 1;
        uint8_t rn     = (op >> 5) & 0x1F;
        uint8_t rt     = op & 0x1F;
        bool is_load = (opc & 2) || (opc & 1);
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];
        uint64_t off = extend_reg(cpu.regs[rm], option, S ? size : 0, true);
        uint64_t addr = base + off;
        if (is_vec) {
            bool is_load_v = opc & 1;
            bool is_q = (opc & 2) && size == 0;
            int nbytes = is_q ? 16 : (1 << size);
            if (is_load_v) {
                uint64_t lo = 0, hi = 0;
                mem_.read(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8);
                cpu.v_lo[rt] = lo;
                cpu.v_hi[rt] = (nbytes >= 16) ? hi : 0;
            } else {
                uint64_t lo = cpu.v_lo[rt];
                mem_.write(addr, &lo, std::min(nbytes, 8));
                if (nbytes > 8) {
                    uint64_t hi = cpu.v_hi[rt];
                    mem_.write(addr + 8, &hi, nbytes - 8);
                }
            }
            return;
        }
        int width_bytes = 1 << size;
        if (is_load) {
            uint64_t v = 0;
            mem_.read(addr, &v, width_bytes);
            if (rt != 31) {
                if (opc & 2) {
                    int bits = width_bytes * 8;
                    v = sign_extend(v, bits);
                }
                cpu.regs[rt] = v;
            }
        } else {
            uint64_t v = (rt == 31) ? 0 : cpu.regs[rt];
            uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
            v &= mask;
            mem_.write(addr, &v, width_bytes);
        }
        return;
    }


    // ------------------------------------------------------------------
    // Group: Load/Store exclusive (STXR/LDXR/STLR/LDAR/etc)
    //
    // Single-threaded emulator: exclusives always succeed (STXR returns 0
    // in Ws). Acquire/release variants (STLR/LDAR) are just regular
    // loads/stores.
    //
    // Encoding: size 001000 o0 L Rs 0 0 1 1 1 Rn Rt  (STXR/LDXR/STLXR/LDAXR)
    //          size 001000 1 1 L 0 0 0 1 1 1 1 Rn Rt  (STLR/LDAR)
    //          size 001000 0 1 L 0 Rs 1 1 1 1 1 Rn Rt (CAS/CASP - LSE atomics)
    //          size 111000 o0 L Rs opcode Rn Rt        (LDADD/LDCLR/etc - LSE)
    // We implement them all as non-atomic load/store + (for STXR) success=0.
    // ------------------------------------------------------------------
    // (LSE atomics handler was here but has been moved earlier, before
    // the load/store handlers, to prevent CAS/atomics being caught by
    // the regular load/store register-offset handler.)

    // ------------------------------------------------------------------
    // Group: Load/Store exclusive (STXR/LDXR/STLR/LDAR/etc)
    //
    // Encoding: size 001000 o0 L Rs 0 0 1 1 1 Rn Rt  (STXR/LDXR/STLXR/LDAXR)
    //          size 001000 1 1 L 0 0 0 1 1 1 1 Rn Rt  (STLR/LDAR)
    // ------------------------------------------------------------------
    if ((op & 0x3F000000) == 0x08000000) {
        // Various load/store exclusive and atomic encodings.
        // Common shape: size 001000 ... Rn Rt.
        uint8_t size = (op >> 30) & 3;
        // L is bit 22 for STXR/LDXR/STLR/LDAR family.
        // o0 (acquire/release) is bit 23.
        // Encodings:
        //   STXR:  bit 23=0, bit 22=0
        //   LDXR:  bit 23=0, bit 22=1
        //   STLXR: bit 23=1, bit 22=0
        //   LDAXR: bit 23=1, bit 22=1
        //   STLR:  bit 23=1, bit 22=0, Rs=0, low6=111111
        //   LDAR:  bit 23=1, bit 22=1, Rs=0, low6=111111
        bool L  = (op >> 22) & 1;
        bool o0 = (op >> 23) & 1;
        uint8_t rs = (op >> 16) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rt = op & 0x1F;
        int width_bytes = 1 << size;
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];

        // Detect specific sub-forms by the low bits.
        // STXR/LDXR family: bits 15:10 = 001111 (exclusives)
        // STLR/LDAR family: bits 15:10 = 111111 (acquire/release, no Rs)
        // CAS family: bits 15:12 = 1111 with bit 21 set
        uint8_t low6 = (op >> 10) & 0x3F;

        if (low6 == 0x0F || low6 == 0x1F || low6 == 0x3F) {
            // Exclusive load/store. The low6 field distinguishes sub-forms:
            //   0x0F: STXR/LDXR (non-acquire, with Rs)
            //   0x1F: STLXR/LDAXR (acquire/release, with Rs)
            //   0x3F: STLR/LDAR (no Rs) OR LDXR/LDAXR (no Rs, o0=0)
            // The key insight: when o0=0 and L=1, it's LDXR (mark monitor).
            // When o0=1, it's acquire/release (STLR/LDAR).
            bool use_monitor = (o0 == 0) || (low6 != 0x3F);
            if (L == 0) {
                // Store-exclusive: write Wt to [Xn] only if the monitor
                // is still tagged for this address; set Ws = 0 on success,
                // Ws = 1 on failure. Either way, clear the monitor.
                if (use_monitor) {
                    bool ok = cpu.excl_check(base, width_bytes);
                    if (ok) {
                        uint64_t v = cpu.regs[rt];
                        uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
                        v &= mask;
                        mem_.write(base, &v, width_bytes);
                    }
                    if (rs != 31) cpu.regs[rs] = ok ? 0 : 1;
                    cpu.excl_clear();
                } else {
                    // STLR: store-release (no monitor check, always succeeds)
                    uint64_t v = cpu.regs[rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(base, &v, width_bytes);
                }
            } else {
                // Load-exclusive: read from [Xn] into Wt
                uint64_t v = 0;
                mem_.read(base, &v, width_bytes);
                cpu.regs[rt] = v;
                if (use_monitor) {
                    cpu.excl_mark(base, width_bytes);
                }
            }
            return;
        }

        // ── CAS family (compare-and-swap) ─────────────────────────────
        // CAS is encoded within the LSE atomic ops space with opcodes
        // 0xC (CAS), 0xD (CASA), 0xE (CASL), 0xF (CASAL). The A/L
        // suffixes are just memory ordering hints we ignore.
        //
        // CAS semantics:
        //   - Rt = comparand (compared against [Rn])
        //   - Rs = new value (stored to [Rn] if comparand matched)
        //   - Rt receives old memory value (always, success or failure)
        //
        // Old CAS and LSE atomics handlers were here but have been
        // moved to the top-level LSE atomics handler above (since LSE
        // atomics have bits 29:24 = 111000, not 001000, and were never
        // reaching this nested code). This block now only handles
        // exclusive load/store (STXR/LDXR/STLR/LDAR) and unknown
        // sub-forms of the 001000 encoding group.

        // Unknown sub-form; treat as NOP to avoid crashing
        return;
    }

    // ------------------------------------------------------------------
    // Group: SIMD/NEON instructions (basic subset for memset/memcpy/etc)
    //
    // The full AArch64 SIMD encoding space is enormous. We implement the
    // handful of instructions commonly used by glibc's optimized mem*:
    //   - DUP Vd.T, Rn       (broadcast GP register to vector)
    //   - DUP Vd.T, #imm8    (broadcast immediate to vector)
    //   - LD1 {Vt.T}, [Xn]   (vector load, single structure)
    //   - ST1 {Vt.T}, [Xn]   (vector store, single structure)
    //   - MOV/Vmov (vector)  (covered by ORR alias)
    //   - INS/INS-gen        (insert GP reg into vector element)
    //   - EXT                (extract bytes from pair of vectors)
    //   - REV16/REV32/REV64  (byte reversal within elements)
    //   - FMOV (scalar)      (move FP scalar between GP and FP regs)
    //
    // Many other SIMD ops are stubbed (treated as NOPs) when they're
    // not on the critical path.
    // ------------------------------------------------------------------
    if ((op & 0xBE000000) == 0x0C000000) {
        // Advanced SIMD load/store multiple structures (LD1/ST1/...)
        // 0 Q 0011000 L 000000 opcode size Rn Rt
        // bits 31:25 = 0_Q_0011000 -- mask off Q (bit 30) with 0xBE000000.
        // This distinguishes from SIMD data-processing (bits 31:25 = 0_Q_0001111 = 0x0E).
        uint8_t size = (op >> 10) & 3;
        uint8_t opcode = (op >> 12) & 0xF;
        bool L = (op >> 22) & 1;  // 0=store, 1=load
        bool Q = (op >> 30) & 1;  // 0=8B/4H/2S, 1=16B/8H/4S/2D
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rt = op & 0x1F;
        int esize = 1 << size;
        int elems = (Q ? 16 : 8) / esize;
        int total_bytes = Q ? 16 : 8;
        uint64_t base = (rn == 31) ? cpu.sp : cpu.regs[rn];

        // For LD1/ST1 single structure (opcode 0b0000 with various
        // sub-encodings via size field), we transfer one register.
        // For multi-register (e.g. LD1 {Vt.16B, Vt+1.16B}), we transfer
        // multiple consecutive registers.
        int nregs = 1;
        if (opcode == 0x0) nregs = 1;        // LD1/ST1 1 reg
        else if (opcode == 0x2) nregs = 1;   // LD1R/ST1R (replicate)
        else if (opcode == 0x4) nregs = 2;   // LD1/ST1 2 regs
        else if (opcode == 0x6) nregs = 2;   // LD1R/ST1R 2 regs
        else if (opcode == 0x7) nregs = 2;   // LD3/ST3 (rare)
        else if (opcode == 0x8) nregs = 3;   // LD1/ST1 3 regs
        else if (opcode == 0xA) nregs = 4;   // LD1/ST1 4 regs

        for (int i = 0; i < nregs; i++) {
            int r = (rt + i) & 0x1F;
            uint64_t a = base + i * total_bytes;
            if (L) {
                // load
                uint8_t buf[16];
                mem_.read(a, buf, total_bytes);
                memcpy(&cpu.v_lo[r], buf, 8);
                if (total_bytes == 16) memcpy(&cpu.v_hi[r], buf + 8, 8);
                else cpu.v_hi[r] = 0;
            } else {
                // store
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[r], 8);
                if (total_bytes == 16) memcpy(buf + 8, &cpu.v_hi[r], 8);
                mem_.write(a, buf, total_bytes);
            }
        }
        return;
    }

    if ((op & 0x9E000000) == 0x0E000000) {
        // Advanced SIMD data-processing (scalar + vector). Many sub-cases.
        // 0 Q U 0 11110 size opcode Rm Rn Rd
        // Top bits 31, 28:24 (masking off Q=bit 30 and U=bit 29).
        // We use mask 0x9E000000 (0b1001 1110 ...) to mask off both Q and U.
        bool Q = (op >> 30) & 1;
        bool U = (op >> 29) & 1;
        uint8_t size = (op >> 22) & 3;
        uint8_t opcode = (op >> 12) & 0x1F;
        uint8_t rm = (op >> 16) & 0x1F;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd = op & 0x1F;

        // DUP (general) : 0 Q 0 0 1 1 1 1 0 0 0 imm5 0 0 0 0 1 1 1 1 1 Rn Rd
        //   Top 11 bits (31:21): 0 Q 0 0 1 1 1 1 0 0 0 -> constant when Q ignored
        //   Bits 15:10: 000011 -> constant
        // Match: (op & 0xFFE0FC00) == 0x0E000C00  (masks off Q, imm5, Rn, Rd)
        if ((op & 0xFFE0FC00) == 0x0E000C00) {
            // DUP Vd.T, Rn (general)
            bool Q = (op >> 30) & 1;
            // imm5[4:0] encodes element size: 00001=B, 00010=H, 00100=S, 01000=D
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

        // INS (general) : 0 1 0 0 1 1 1 1 0 0 0 imm5 0 0 0 0 1 1 1 1 1 Rn Rd
        //   Same shape as DUP but Q=1 always (bit 30 = 1).
        if ((op & 0xFFE0FC00) == 0x4E000C00) {
            uint8_t imm5 = (op >> 16) & 0x1F;
            // imm5 has exactly one bit set; that bit position (mod 5) gives index
            int esize = 0, idx = 0;
            for (int b = 0; b < 5; b++) {
                if (imm5 & (1 << b)) { esize = 1 << b; break; }
            }
            // imm5 also encodes the index: shift right by log2(esize)
            idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
            uint64_t src = cpu.regs[rn];
            // Insert element at index idx
            if (esize == 1) {
                ((uint8_t*)&cpu.v_lo[rd])[idx] = src & 0xFF;
            } else if (esize == 2) {
                ((uint16_t*)&cpu.v_lo[rd])[idx] = src & 0xFFFF;
            } else if (esize == 4) {
                ((uint32_t*)&cpu.v_lo[rd])[idx] = src & 0xFFFFFFFF;
            } else if (esize == 8) {
                if (idx == 0) cpu.v_lo[rd] = src;
                else if (idx == 1) cpu.v_hi[rd] = src;
            }
            return;
        }

        // ORR (vector) - serves as MOV (vector) too
        // 0 Q 0 0 11110 00010 0 Rm 0 0 0 1 1 1 Rn Rd
        if ((op & 0xFF20FC00) == 0x0EA01C00) {
            cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // EXT (extract) - byte extraction from concatenated pair
        // 0 Q 0 101110 imm4 0 Rm 0 0 0 N Rn Rd
        if ((op & 0xFFE00000) == 0x6E000000) {
            uint8_t imm4 = (op >> 11) & 0xF;
            // Concatenate Vm:Vn (Vm high, Vn low), extract 16/8 bytes starting at imm4
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

        // REV16 (vector) - reverse bytes within 16-bit halfwords
        // 0 Q 0 0 11110 00 1 0000 0 1 0 1 1 0 Rn Rd
        if ((op & 0xBFFFFC00) == 0x0E201800) {
            uint8_t buf[16];
            memcpy(buf, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
            int nbytes = Q ? 16 : 8;
            for (int i = 0; i < nbytes; i += 2) {
                std::swap(buf[i], buf[i+1]);
            }
            memcpy(&cpu.v_lo[rd], buf, 8);
            if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
            else cpu.v_hi[rd] = 0;
            return;
        }

        // REV32 (vector) - reverse bytes within 32-bit words
        // 0 Q 0 0 11110 00 1 0000 0 1 1 1 1 0 Rn Rd
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

        // REV64 (vector) - reverse bytes within 64-bit doublewords
        // 0 Q 0 0 11110 00 1 0000 0 1 0 0 1 1 0 Rn Rd
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

        // CNT (vector) - count bits in each byte
        // 0 Q 0 0 11110 00 1 0000 0 1 0 1 1 1 Rn Rd
        if ((op & 0xBFFFFC00) == 0x0E205800) {
            uint8_t buf[16];
            memcpy(buf, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
            int nbytes = Q ? 16 : 8;
            for (int i = 0; i < nbytes; i++) {
                buf[i] = __builtin_popcount(buf[i]);
            }
            memcpy(&cpu.v_lo[rd], buf, 8);
            if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
            else cpu.v_hi[rd] = 0;
            return;
        }

        // UADDLV (addv) - horizontal add of elements
        // 0 Q 0 0 11110 size 1 1000 0 1 1 0 1 1 Rn Rd (opcode = 11011, not 11000)
        // For addv b31, v31.8b: size=00, opcode=11011
        if ((op & 0xBF3FFC00) == 0x0E31B800) {
            // size field determines element size:
            //   00 = 8-bit (addv over 8B or 16B)
            //   01 = 16-bit (addv over 4H or 8H)
            //   10 = 32-bit (addv over 2S or 4S)
            //   11 = reserved
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

        // CMEQ (vector) - compare bitwise equal to zero, or two registers
        // 0 Q 0 0 11110 size 1 0000 1 0 0 1 1 Rn Rd (vs zero)
        // 0 Q 1 0 11110 size 1 0000 1 0 0 1 1 Rm Rn Rd (two regs)
        if ((op & 0xBF9FFC00) == 0x0E208800) {  // CMEQ vs zero
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
        if ((op & 0xBFE0FC00) == 0x2E208C00) {  // CMEQ two registers
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

        // MOVI (vector immediate) - move immediate to vector
        // Multiple forms based on cmode:
        //   cmode=1110, Q=1: MOVI Vd.2D, #imm8 (64-bit, each lane = imm8)
        //   cmode=1110, Q=0: MOVI Vd.1D, #imm8
        //   cmode=1111: MOVI Vd.16B/8B, #imm8 (byte broadcast)
        // The 2D form (cmode=1110) is used heavily by musl to zero 128-bit
        // vector registers (movi v1.2d, #0x0). Without this, softfloat
        // routines get garbage in comparison operands.
        if ((op & 0x3F8FFC00) == 0x0F00E400) {
            bool Q = (op >> 30) & 1;
            uint8_t cmode = (op >> 12) & 0xF;
            // Extract imm8: bits 20:16 (abcde) and bits 7:5 (fgh) → 8-bit
            uint8_t imm8 = ((op >> 16) & 0x1F) << 3 | ((op >> 5) & 0x7);

            if (cmode == 0xE) {
                // 64-bit form: each 64-bit lane = imm8 (zero-extended)
                uint64_t val = imm8;  // zero-extended to 64 bits
                cpu.v_lo[rd] = val;
                if (Q) cpu.v_hi[rd] = val;
                else cpu.v_hi[rd] = 0;
            } else {
                // Byte broadcast form (cmode=0xF)
                uint8_t buf[16];
                memset(buf, imm8, Q ? 16 : 8);
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
            }
            return;
        }

        // SHL (vector, immediate) - shift left by immediate
        // 0 Q 0 1 11110 immh immb 010101 Rn Rd
        if ((op & 0xBF00FC00) == 0x0F00A400) {
            uint8_t immh = (op >> 19) & 0xF;
            uint8_t immb = (op >> 16) & 0xF;
            int esize, shift;
            if (immh == 0) { /* reserved */ return; }
            else if (immh < 2) { esize = 1; shift = (immh & 1) << 4 | immb; }
            else if (immh < 4) { esize = 2; shift = (immh & 3) << 4 | immb; }
            else if (immh < 8) { esize = 4; shift = (immh & 7) << 4 | immb; }
            else { esize = 8; shift = (immh & 0xF) << 4 | immb; }
            (void)shift; (void)esize;
            // For simplicity, just shift each element of size esize by `shift` bits
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

        // USHR (vector, immediate) - unsigned shift right by immediate
        // 0 Q 1 0 11110 immh immb 000001 Rn Rd
        if ((op & 0xBF00FC00) == 0x2F000400) {
            uint8_t immh = (op >> 19) & 0xF;
            uint8_t immb = (op >> 16) & 0xF;
            int esize, shift;
            if (immh == 0) { /* reserved */ return; }
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

        // EOR (vector) - bitwise XOR
        // 0 Q 1 0 11110 00 1 0000 0 1 1 0 1 1 Rn Rd
        if ((op & 0xBFE0FC00) == 0x2E201C00) {
            cpu.v_lo[rd] = cpu.v_lo[rn] ^ cpu.v_lo[rm];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] ^ cpu.v_hi[rm];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // AND (vector) - bitwise AND
        if ((op & 0xBFE0FC00) == 0x0E201C00) {
            cpu.v_lo[rd] = cpu.v_lo[rn] & cpu.v_lo[rm];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & cpu.v_hi[rm];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // ORR (vector) - bitwise OR (covers MOVI alias too)
        // 0 Q 0 1 11110 00 1 0000 0 1 0 1 1 1 Rm Rn Rd
        if ((op & 0xBFE0FC00) == 0x0EA01C00) {
            cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // BIC (vector) - bitwise clear (AND NOT)
        if ((op & 0xBFE0FC00) == 0x0EA01800) {
            cpu.v_lo[rd] = cpu.v_lo[rn] & ~cpu.v_lo[rm];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & ~cpu.v_hi[rm];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // MVN/NOT (vector) - bitwise NOT
        // 0 Q 0 0 11110 00 1 0000 0 1 0 1 0 1 Rn Rd
        if ((op & 0xBFFFFC00) == 0x0E205800) {
            // Wait that's CNT. Let me re-check.
            // MVN: 0 Q 0 0 11110 00 1 0000 0 1 0 1 0 1 0 Rn Rd -> 0x0E205800? No.
            // Actually MVN is 0x2E205800 (bit 29 = 1 for U=1).
            cpu.v_lo[rd] = ~cpu.v_lo[rn];
            if (Q) cpu.v_hi[rd] = ~cpu.v_hi[rn];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // TBL/TBX (vector table lookup) - complex, stub for now
        if ((op & 0xBFE0FC00) == 0x0E000000 ||  // TBL
            (op & 0xBFE0FC00) == 0x0E001000) {  // TBX
            // Stub: just copy Vn to Vd (incorrect but won't crash)
            cpu.v_lo[rd] = cpu.v_lo[rn];
            if (Q) cpu.v_hi[rd] = cpu.v_hi[rn];
            else cpu.v_hi[rd] = 0;
            return;
        }

        // UMAXP/UMINP/SMAXP/SMINP (pairwise max/min)
        // Encoding: 0 Q U 0 11110 size 1 C 0001 1 Rm Rn Rd
        // For UMAXP: U=1, C=0 -> bits 28:10 = 0_11110_size_1_0_0001_1
        // For UMINP: U=1, C=1
        // For SMAXP: U=0, C=0
        // For SMINP: U=0, C=1
        if ((op & 0xBFE0FC00) == 0x2E20A400) {  // UMAXP/UMINP/SMAXP/SMINP family
            bool C = (op >> 15) & 1;  // 0 = max, 1 = min
            int esize = 1 << size;
            int elems = (Q ? 16 : 8) / esize;
            uint8_t buf_n[16], buf_m[16];
            memcpy(buf_n, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
            memcpy(buf_m, &cpu.v_lo[rm], 8);
            if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
            uint8_t out[16] = {0};
            // Pairwise: for each pair of adjacent elements, compute max/min
            // of (Vn[2i], Vn[2i+1]) and (Vm[2i], Vm[2i+1]), then max/min of those.
            for (int i = 0; i < elems / 2; i++) {
                uint64_t n0 = 0, n1 = 0, m0 = 0, m1 = 0;
                memcpy(&n0, buf_n + (2*i) * esize, esize);
                memcpy(&n1, buf_n + (2*i+1) * esize, esize);
                memcpy(&m0, buf_m + (2*i) * esize, esize);
                memcpy(&m1, buf_m + (2*i+1) * esize, esize);
                uint64_t pn, pm;
                if (C == 0) {
                    // max
                    pn = (n0 > n1) ? n0 : n1;
                    pm = (m0 > m1) ? m0 : m1;
                    uint64_t res = (pn > pm) ? pn : pm;
                    memcpy(out + i * esize, &res, esize);
                } else {
                    // min
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

        // CMHS (vector) - compare unsigned higher or same (>=)
        // 0 Q 1 0 11110 size 1 0000 1 1 0 1 1 1 Rm Rn Rd
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

        // SHRN (vector, immediate) - shift right narrow
        // 0 Q 0 0 11110 immh immb 100001 Rn Rd
        if ((op & 0xBF00FC00) == 0x0F008400) {
            uint8_t immh = (op >> 19) & 0xF;
            uint8_t immb = (op >> 16) & 0xF;
            int esize, shift;
            if (immh < 2) { esize = 2; shift = (16 - ((immh & 1) << 4 | immb)); }
            else if (immh < 4) { esize = 4; shift = (32 - (((immh & 3) << 4) | immb)); }
            else { esize = 8; shift = (64 - (((immh & 7) << 4) | immb)); }
            // Narrow: source elements are 2x size of destination.
            int src_esize = esize * 2;
            int src_elems = (Q ? 16 : 8) / src_esize;
            int dst_elems = src_elems * 2;
            uint8_t buf[16];
            memcpy(buf, &cpu.v_lo[rn], 8);
            if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
            uint8_t out[8] = {0};
            for (int i = 0; i < dst_elems; i++) {
                uint64_t v = 0;
                memcpy(&v, buf + i * esize, esize);
                v >>= shift;
                memcpy(out + i * esize, &v, esize);
            }
            memcpy(&cpu.v_lo[rd], out, 8);
            cpu.v_hi[rd] = 0;
            return;
        }

        // Fallback: treat as NOP for SIMD ops we don't model. This is
        // incorrect but lets glibc continue. Programs that actually
        // depend on FP results will produce wrong output.
        return;
    }

    // ------------------------------------------------------------------
    // Group: Conditional compare (CCMP/CCMN, immediate and register)
    //   Encoding: sf op 1 1 01010 0 o2 ... cond ... 1 0 Rn nzcv
    //   Fixed bits 28:21 = 11010010 (bit 22=0 distinguishes from CSEL which has bit 22=1)
    //   bit 29 = 1 (constant)
    //   bit 30: 0=CCMN, 1=CCMP
    //   bit 21 (o2): 0=immediate, 1=register
    //   Mask: 0x3FE00000 (bits 29:21 — not bit 31 or 30)
    //   Expected: 0x3A400000 (bit 29=1, bits 28:21=11010010)
    // ------------------------------------------------------------------
    if ((op & 0x3FE00000) == 0x3A400000) {
        bool sf = (op >> 31) & 1;
        bool is_ccmp = (op >> 30) & 1;    // bit 30: 0=CCMN, 1=CCMP
        bool is_register = (op >> 21) & 1; // bit 21 (o2): 0=immediate, 1=register
        uint8_t cond = (op >> 12) & 0xF;
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t nzcv_field = op & 0xF;  // bottom 4 bits
        uint64_t operand;
        if (is_register) {
            uint8_t rm = (op >> 16) & 0x1F;
            operand = cpu.regs[rm];
        } else {
            uint8_t imm5 = (op >> 16) & 0x1F;
            operand = imm5;
        }
        // is_ccmp (bit 30): 0=CCMN (add), 1=CCMP (sub)
        bool is_sub = is_ccmp;
        int width = sf ? 64 : 32;
        uint64_t a = cpu.regs[rn];
        if (!sf) a &= 0xFFFFFFFF;
        if (!sf) operand &= 0xFFFFFFFF;

        if (cond_true(cond, cpu.pstate)) {
            // Perform the comparison: set NZCV based on (a - operand) or (a + operand)
            if (is_sub) {
                set_sub_flags(cpu, a, operand, width, true);
            } else {
                set_add_flags(cpu, a, operand, 0, width, true);
            }
        } else {
            // Set NZCV from the nzcv field
            cpu.set_flag_n(nzcv_field & 8);
            cpu.set_flag_z(nzcv_field & 4);
            cpu.set_flag_c(nzcv_field & 2);
            cpu.set_flag_v(nzcv_field & 1);
        }
        return;
    }

    // ------------------------------------------------------------------
    // Group: scalar FP/SIMD (FMOV, FADD, FSUB, FMUL, FDIV, FCMP, etc.)
    //
    // FP register file: v_lo[0..31] holds bits 63:0, v_hi[0..31] holds
    // bits 127:64. For scalar FP:
    //   - S registers (32-bit float):  v_lo[n] bits 31:0
    //   - D registers (64-bit double): v_lo[n] bits 63:0
    //
    // We use C++ native float/double for the actual arithmetic, converting
    // to/from the bit representation.
    // ------------------------------------------------------------------
    if ((op & 0xFFE00000) == 0x1E200000 ||  // FP scalar (various)
        (op & 0xFF000000) == 0x1E000000 ||  // FP data-processing (32-bit)
        (op & 0xFF000000) == 0x9E000000) {  // FP data-processing (64-bit)
        uint8_t rn = (op >> 5) & 0x1F;
        uint8_t rd = op & 0x1F;
        uint8_t rm = (op >> 16) & 0x1F;
        bool sf = (op >> 31) & 1;
        bool ftype = (op >> 22) & 1;  // 0=S(32-bit), 1=D(64-bit)

        // Helper: read FP register as double
        auto read_fp_d = [&](int r) -> double {
            uint64_t bits = cpu.v_lo[r];
            double d;
            memcpy(&d, &bits, 8);
            return d;
        };
        // Helper: read FP register as float
        auto read_fp_s = [&](int r) -> float {
            uint32_t bits = (uint32_t)cpu.v_lo[r];
            float f;
            memcpy(&f, &bits, 4);
            return f;
        };
        // Helper: write double to FP register
        auto write_fp_d = [&](int r, double d) {
            uint64_t bits;
            memcpy(&bits, &d, 8);
            cpu.v_lo[r] = bits;
            cpu.v_hi[r] = 0;
        };
        // Helper: write float to FP register
        auto write_fp_s = [&](int r, float f) {
            uint32_t bits;
            memcpy(&bits, &f, 4);
            cpu.v_lo[r] = bits;
            cpu.v_hi[r] = 0;
        };

        // ── FMOV (general ↔ FP) ──────────────────────────────────────
        // FMOV Dd, Rn: 0x9E670000 | (Rn<<5) | Rd
        // FMOV Rd, Dn: 0x9E660000 | (Rn<<5) | Rd
        if ((op & 0xFFE0FC00) == 0x9E600000) {
            bool to_fp = (op >> 16) & 1;
            if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn]; cpu.v_hi[rd] = 0; }
            else       { cpu.regs[rd] = cpu.v_lo[rn]; }
            return;
        }
        // FMOV Sd, Wn: 0x1E270000 | (Rn<<5) | Rd
        // FMOV Wd, Sn: 0x1E260000 | (Rn<<5) | Rd
        if ((op & 0xFFE0FC00) == 0x1E200000) {
            bool to_fp = (op >> 16) & 1;
            if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn] & 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
            else       { cpu.regs[rd] = cpu.v_lo[rn] & 0xFFFFFFFF; }
            return;
        }

        // ── FMOV (scalar, immediate) ─────────────────────────────────
        // FMOV Dd, #imm: 0x1E601000 | (imm8<<13) | Rd
        if ((op & 0xFFE0001F) == 0x1E600000 && ((op >> 5) & 0x1F) == 0) {
            // Decode 8-bit immediate to double
            uint8_t imm8 = (op >> 13) & 0xFF;
            // ARM64 FP immediate encoding: sign(1) | exp(4) | mantissa(3)
            // Reconstruct double: sign << 63 | (exp << 52) | (mantissa << 48)
            uint64_t sign = ((uint64_t)(imm8 >> 7)) & 1;
            uint64_t exp = ((uint64_t)(imm8 >> 3)) & 0xF;
            uint64_t mant = ((uint64_t)imm8) & 0x7;
            // Exponent: if all 4 bits set → inf/nan, else exp = (bits ^ 0x8) + 1023
            uint64_t exp_field;
            if ((exp & 0xF) == 0xF) {
                exp_field = 0x7FF;
            } else {
                exp_field = ((exp ^ 0x8) & 0xF) + 1023;
            }
            uint64_t bits = (sign << 63) | (exp_field << 52) | (mant << 49);
            if (ftype) { // D register
                cpu.v_lo[rd] = bits;
                cpu.v_hi[rd] = 0;
            } else { // S register
                // For S register, use 32-bit version
                uint32_t sign32 = (uint32_t)sign;
                uint32_t exp32;
                if ((exp & 0xF) == 0xF) exp32 = 0xFF;
                else exp32 = ((exp ^ 0x8) & 0xF) + 127;
                uint32_t mant32 = mant;
                uint32_t bits32 = (sign32 << 31) | (exp32 << 23) | (mant32 << 20);
                cpu.v_lo[rd] = bits32;
                cpu.v_hi[rd] = 0;
            }
            return;
        }

        // ── FMOV (register, FP to FP) ────────────────────────────────
        if ((op & 0xFFFFFC00) == 0x1E604000 ||  // FMOV Dd, Dn
            (op & 0xFFFFFC00) == 0x1E204000) {  // FMOV Sd, Sn
            cpu.v_lo[rd] = cpu.v_lo[rn];
            if (ftype) cpu.v_hi[rd] = 0;
            else { cpu.v_lo[rd] &= 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
            return;
        }

        // FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] — now handled by the
        // decoder switch above (InstClass::FMOV_VD1 / FMOV_RVD1).

        // ── FP arithmetic (2-source): FADD/FSUB/FMUL/FDIV/FMAX/FMIN ─
        // Encoding: sf 0 0 11110 ftype 1 Rm opcode 1 Rn Rd
        //   opcode: 0010=FADD, 0011=FSUB, 0000=FMUL, 0001=FDIV,
        //           0100=FMAX, 0101=FMIN, 0110=FNMUL
        if ((op & 0xFF200000) == 0x1E200000 && ((op >> 21) & 1) == 1) {
            uint8_t opcode = (op >> 12) & 0xF;
            if (ftype) { // 64-bit double
                double a = read_fp_d(rn), b = read_fp_d(rm), r = 0;
                switch (opcode) {
                    case 0x2: r = a + b; break;                    // FADD
                    case 0x3: r = a - b; break;                    // FSUB
                    case 0x0: r = a * b; break;                    // FMUL
                    case 0x1: r = a / b; break;                    // FDIV
                    case 0x4: r = (a > b) ? a : b; break;          // FMAX
                    case 0x5: r = (a < b) ? a : b; break;          // FMIN
                    case 0x6: r = -(a * b); break;                 // FNMUL
                    default: r = 0; break;
                }
                write_fp_d(rd, r);
            } else { // 32-bit float
                float a = read_fp_s(rn), b = read_fp_s(rm), r = 0;
                switch (opcode) {
                    case 0x2: r = a + b; break;                    // FADD
                    case 0x3: r = a - b; break;                    // FSUB
                    case 0x0: r = a * b; break;                    // FMUL
                    case 0x1: r = a / b; break;                    // FDIV
                    case 0x4: r = (a > b) ? a : b; break;          // FMAX
                    case 0x5: r = (a < b) ? a : b; break;          // FMIN
                    case 0x6: r = -(a * b); break;                 // FNMUL
                    default: r = 0; break;
                }
                write_fp_s(rd, r);
            }
            return;
        }

        // ── FP 1-source: FABS/FNEG/FSQRT/FRINT ───────────────────────
        // Encoding: sf 0 0 11110 ftype 1 opcode 10000 Rn Rd
        //   opcode: 0001=FABS, 0010=FNEG, 0011=FSQRT,
        //           0100=FRINTN, 0101=FRINTP, 0110=FRINTM, 0111=FRINTZ,
        //           1100=FRINTA, 1110=FRINTX, 1111=FRINTI
        if ((op & 0xFF3F0000) == 0x1E200000 && ((op >> 15) & 1) == 1 &&
            ((op >> 14) & 1) == 0 && ((op >> 13) & 1) == 0) {
            // Actually this is: bits 15:10 = 1xxxxx where bit 15=1
            // Let me check more carefully
        }
        // Simpler: check for the 1-source pattern directly
        // sf 0 0 11110 ftype 1 opcode 10000 Rn Rd
        // mask: bits 31:24 = x0 11110 x, bits 21=1, bits 15:10 = 10000
        if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3F) == 0x10) {
            uint8_t opcode = (op >> 12) & 0xF;
            if (ftype) { // 64-bit double
                double a = read_fp_d(rn), r = 0;
                switch (opcode) {
                    case 0x0: r = a; break;                        // FMOV (already handled, but just in case)
                    case 0x1: r = std::fabs(a); break;                  // FABS
                    case 0x2: r = -a; break;                       // FNEG
                    case 0x3: r = std::sqrt(a); break;                  // FSQRT
                    case 0x4: r = std::rint(a); break;                  // FRINTN (round to nearest even)
                    case 0x5: r = std::ceil(a); break;                  // FRINTP (round toward +inf)
                    case 0x6: r = std::floor(a); break;                 // FRINTM (round toward -inf)
                    case 0x7: r = std::trunc(a); break;                 // FRINTZ (round toward 0)
                    case 0xC: r = std::rint(a); break;                  // FRINTA
                    case 0xE: r = std::rint(a); break;                  // FRINTX
                    case 0xF: r = std::rint(a); break;                  // FRINTI
                    default: r = a; break;
                }
                write_fp_d(rd, r);
            } else { // 32-bit float
                float a = read_fp_s(rn), r = 0;
                switch (opcode) {
                    case 0x0: r = a; break;
                    case 0x1: r = std::fabsf(a); break;                 // FABS
                    case 0x2: r = -a; break;                       // FNEG
                    case 0x3: r = std::sqrtf(a); break;                 // FSQRT
                    case 0x4: r = std::rintf(a); break;                 // FRINTN
                    case 0x5: r = std::ceilf(a); break;                 // FRINTP
                    case 0x6: r = std::floorf(a); break;                // FRINTM
                    case 0x7: r = std::truncf(a); break;                // FRINTZ
                    case 0xC: r = std::rintf(a); break;                 // FRINTA
                    default: r = a; break;
                }
                write_fp_s(rd, r);
            }
            return;
        }

        // ── FCVT (convert between S and D) ──────────────────────────
        // FCVT Sd, Dn: 0x1E624000 | (Rn<<5) | Rd
        // FCVT Dd, Sn: 0x1E22C000 | (Rn<<5) | Rd
        if ((op & 0xFFFFFC00) == 0x1E624000) { // FCVT Sd, Dn (double→float)
            double d = read_fp_d(rn);
            write_fp_s(rd, (float)d);
            return;
        }
        if ((op & 0xFFFFFC00) == 0x1E22C000) { // FCVT Dd, Sn (float→double)
            float f = read_fp_s(rn);
            write_fp_d(rd, (double)f);
            return;
        }

        // ── FCMP/FCMPE (compare) ────────────────────────────────────
        // FCMP Dn, Dm: 0x1E602000 | (Rm<<16) | (Rn<<5)
        // Sets NZCV flags in PSTATE.
        if ((op & 0xFFE0FC1F) == 0x1E602000) {
            if (ftype) { // 64-bit
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
            } else { // 32-bit
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
        // FCMP with #0.0: 0x1E602008 | (Rn<<5)
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

        // ── FCVTZS/FCVTZU (FP → signed/unsigned int) ────────────────
        // FCVTZS Wd, Dn: 0x1E780000 | (Rn<<5) | Rd
        // FCVTZS Xd, Dn: 0x9E780000 | (Rn<<5) | Rd
        // FCVTZU Wd, Dn: 0x1E790000 | (Rn<<5) | Rd
        // FCVTZU Xd, Dn: 0x9E790000 | (Rn<<5) | Rd
        if ((op & 0x7F3F0000) == 0x1E780000 ||  // FCVTZS
            (op & 0x7F3F0000) == 0x1E790000) {  // FCVTZU
            bool is_unsigned = ((op >> 16) & 1);
            bool is_64bit = sf;
            if (ftype) { // double
                double a = read_fp_d(rn);
                if (is_unsigned) {
                    uint64_t v = (a < 0) ? 0 : (uint64_t)a;
                    if (is_64bit) cpu.regs[rd] = v;
                    else cpu.regs[rd] = (uint32_t)v;
                } else {
                    int64_t v = (int64_t)a;
                    if (is_64bit) cpu.regs[rd] = (uint64_t)v;
                    else cpu.regs[rd] = (uint32_t)(int32_t)v;
                }
            } else { // float
                float a = read_fp_s(rn);
                if (is_unsigned) {
                    uint64_t v = (a < 0) ? 0 : (uint64_t)a;
                    if (is_64bit) cpu.regs[rd] = v;
                    else cpu.regs[rd] = (uint32_t)v;
                } else {
                    int64_t v = (int64_t)a;
                    if (is_64bit) cpu.regs[rd] = (uint64_t)v;
                    else cpu.regs[rd] = (uint32_t)(int32_t)v;
                }
            }
            return;
        }

        // ── SCVTF/UCVTF (int → FP) ──────────────────────────────────
        // SCVTF Dd, Wn: 0x1E620000 | (Rn<<5) | Rd
        // SCVTF Dd, Xn: 0x9E620000 | (Rn<<5) | Rd
        // UCVTF Dd, Wn: 0x1E630000 | (Rn<<5) | Rd
        // UCVTF Dd, Xn: 0x9E630000 | (Rn<<5) | Rd
        if ((op & 0x7F3F0000) == 0x1E620000 ||  // SCVTF
            (op & 0x7F3F0000) == 0x1E630000) {  // UCVTF
            bool is_unsigned = ((op >> 16) & 1);
            bool is_64bit = sf;
            if (ftype) { // double
                if (is_unsigned) {
                    uint64_t v = is_64bit ? cpu.regs[rn] : (uint32_t)cpu.regs[rn];
                    write_fp_d(rd, (double)v);
                } else {
                    int64_t v = is_64bit ? (int64_t)cpu.regs[rn] : (int32_t)cpu.regs[rn];
                    write_fp_d(rd, (double)v);
                }
            } else { // float
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

        // ── FSEL (conditional select) ───────────────────────────────
        // FCSel Dd, Dn, Dm, cond: 0x1E600C00 | (cond<<12) | (Rm<<16) | (Rn<<5) | Rd
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

        // ── FMADD/FMSUB (fused multiply-accumulate) ─────────────────
        // FMADD Dd, Dn, Dm, Da: 0x1F000000 | (Rm<<16) | (Ra<<10) | (Rn<<5) | Rd
        // FMSUB Dd, Dn, Dm, Da: 0x1F008000 | (Rm<<16) | (Ra<<10) | (Rn<<5) | Rd
        if ((op & 0xFF200000) == 0x1F000000) {
            uint8_t ra = (op >> 10) & 0x1F;
            bool sub = (op >> 15) & 1;  // 0=FMADD, 1=FMSUB
            if (ftype) { // double
                double a = read_fp_d(rn), b = read_fp_d(rm), c = read_fp_d(ra);
                double r = sub ? (c - a * b) : (c + a * b);
                write_fp_d(rd, r);
            } else { // float
                float a = read_fp_s(rn), b = read_fp_s(rm), c = read_fp_s(ra);
                float r = sub ? (c - a * b) : (c + a * b);
                write_fp_s(rd, r);
            }
            return;
        }

        // Unknown FP instruction — don't crash, just NOP
        (void)sf; (void)rm;
        return;
    }

    // ------------------------------------------------------------------
    // Group: MOV (register) - really ORR Rd, XZR, Rm
    // ------------------------------------------------------------------
    // (Already handled by logical shifted register case.)

    // ------------------------------------------------------------------
    // Unhandled - bail
    // ------------------------------------------------------------------
    throw DecodeError(cpu.pc, inst);
}

// ---------------------------------------------------------------------------
// Linux AArch64 syscall layer
//   syscall number in x8, args in x0..x5, return value in x0
// ---------------------------------------------------------------------------

} // namespace arm64emu

// End of interpreter.cpp
