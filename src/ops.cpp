// ops.cpp — IR executor (debug / fallback) for bifrost-emu (v1.4.0-alpha.3)
//
// Executes a list of IR instructions with a tight switch loop. This is
// NOT the JIT — it's the slow reference path used when the JIT is
// disabled or when a block can't be compiled (e.g., the code buffer is
// full). The JIT (frostjit.cpp) compiles the same IR to native x86-64.
//
// For CALL_INTERP ops, the executor calls emu.step_public(cpu) inline
// — the block does NOT split. After the call, if PC changed (branch),
// the executor returns immediately.

#include "ir.hpp"
#include "arm64_emu.hpp"
#include <cstring>

namespace arm64emu {

// Maximum number of virtual registers (ARM64 has 33 + scratch).
// We use a fixed-size array for cache locality.
static constexpr int MAX_VREGS = 512;

// Helper: sign-extend a value from `bits` width.
static inline uint64_t sext(uint64_t v, int bits) {
    if (bits >= 64) return v;
    uint64_t m = 1ULL << (bits - 1);
    return (v ^ m) - m;
}

// Helper: zero-extend a value from `bits` width.
static inline uint64_t zext(uint64_t v, int bits) {
    if (bits >= 64) return v;
    return v & ((1ULL << bits) - 1);
}

// Execute an IR block. Returns the next guest PC.
uint64_t execute_ir(const IRBlock& block, CPU& cpu, Emulator& emu,
                    uint8_t* window_base) {
    // Virtual register file (local array — L1 cache hot).
    uint64_t vregs[MAX_VREGS] = {};

    // Initialize ARM64 register vregs from CPU struct.
    // Vreg 0-30 = X0-X30, 31 = SP, 32 = XZR (always 0).
    for (int i = 0; i < 31; i++) {
        vregs[i] = cpu.regs[i];
    }
    vregs[31] = cpu.sp;
    vregs[32] = 0;  // XZR

    for (size_t i = 0; i < block.insts.size(); i++) {
        const IRInst& inst = block.insts[i];

        switch (inst.op) {
            case IROp::NOP:
                break;

            case IROp::IMM:
                if (inst.dest) vregs[inst.dest] = inst.imm;
                break;

            case IROp::MOV:
                if (inst.dest) vregs[inst.dest] = vregs[inst.src1];
                break;

            case IROp::LOAD_REG: {
                uint8_t ar = inst.src1;
                if (ar < 31)      vregs[inst.dest] = cpu.regs[ar];
                else if (ar == 31) vregs[inst.dest] = cpu.sp;
                else               vregs[inst.dest] = 0;  // XZR
                break;
            }

            case IROp::STORE_REG: {
                uint8_t ar = inst.dest;
                uint64_t v = vregs[inst.src1];
                if (ar < 31) {
                    cpu.regs[ar] = v;
                    vregs[ar] = v;  // keep vregs in sync
                }
                else if (ar == 31) {
                    cpu.sp = v;
                    vregs[31] = v;
                }
                // XZR — discard
                break;
            }

            case IROp::LOAD_MEM: {
                uint64_t addr = vregs[inst.src1] + inst.imm;
                vregs[inst.dest] = 0;
                int w = inst.width;
                if (window_base && addr + w <= Memory::DIRECT_WINDOW_SIZE) {
                    memcpy(&vregs[inst.dest], window_base + addr, w);
                } else {
                    emu.mem().read(addr, &vregs[inst.dest], w);
                }
                break;
            }

            case IROp::STORE_MEM: {
                uint64_t addr = vregs[inst.src1] + inst.imm;
                uint64_t val = vregs[inst.src2];
                int w = inst.width;
                if (window_base && addr + w <= Memory::DIRECT_WINDOW_SIZE) {
                    memcpy(window_base + addr, &val, w);
                } else {
                    emu.mem().write(addr, &val, w);
                }
                break;
            }

            case IROp::ADD:
                vregs[inst.dest] = vregs[inst.src1] + vregs[inst.src2];
                break;

            case IROp::SUB:
                vregs[inst.dest] = vregs[inst.src1] - vregs[inst.src2];
                break;

            case IROp::MUL:
                vregs[inst.dest] = vregs[inst.src1] * vregs[inst.src2];
                break;

            case IROp::AND:
                vregs[inst.dest] = vregs[inst.src1] & vregs[inst.src2];
                break;

            case IROp::OR:
                vregs[inst.dest] = vregs[inst.src1] | vregs[inst.src2];
                break;

            case IROp::XOR:
                vregs[inst.dest] = vregs[inst.src1] ^ vregs[inst.src2];
                break;

            case IROp::SHL:
                vregs[inst.dest] = vregs[inst.src1] << (vregs[inst.src2] & 63);
                break;

            case IROp::SHR:
                vregs[inst.dest] = vregs[inst.src1] >> (vregs[inst.src2] & 63);
                break;

            case IROp::SAR:
                vregs[inst.dest] = (uint64_t)((int64_t)vregs[inst.src1] >>
                                               (vregs[inst.src2] & 63));
                break;

            case IROp::ROR: {
                uint64_t v = vregs[inst.src1];
                uint64_t r = vregs[inst.src2] & 63;
                vregs[inst.dest] = r ? ((v >> r) | (v << (64 - r))) : v;
                break;
            }

            case IROp::NOT:
                vregs[inst.dest] = ~vregs[inst.src1];
                break;

            case IROp::NEG:
                vregs[inst.dest] = -(int64_t)vregs[inst.src1];
                break;

            case IROp::SEXT:
                vregs[inst.dest] = sext(vregs[inst.src1], inst.width);
                break;

            case IROp::ZEXT:
                vregs[inst.dest] = zext(vregs[inst.src1], inst.width);
                break;

            case IROp::CLZ:
                vregs[inst.dest] = vregs[inst.src1] ? __builtin_clzll(vregs[inst.src1]) : 64;
                break;

            case IROp::CLS: {
                // ARM CLS: count leading sign bits = CLZ(v ^ SAR(v, W-1)) - 1.
                // Edge cases: CLS(0) = CLS(~0) = W-1.
                // __builtin_clz(0) is UB, so handle 0 and ~0 explicitly.
                uint64_t v = vregs[inst.src1];
                int width = (inst.width == 32) ? 32 : 64;
                uint64_t all_ones = (width == 64) ? ~0ULL : 0xFFFFFFFFULL;
                if (v == 0 || v == all_ones) {
                    vregs[inst.dest] = width - 1;
                } else {
                    uint64_t operand = (int64_t)v < 0 ? ~v : v;
                    int clz = (width == 64) ? __builtin_clzll(operand)
                                            : __builtin_clz(static_cast<uint32_t>(operand));
                    vregs[inst.dest] = clz - 1;
                }
                break;
            }

            case IROp::RBIT: {
                uint64_t v = vregs[inst.src1], r = 0;
                for (int b = 0; b < 64; b++) if ((v >> b) & 1) r |= 1ULL << (63 - b);
                vregs[inst.dest] = r;
                break;
            }

            case IROp::REV16: {
                uint64_t v = vregs[inst.src1], r = 0;
                for (int i = 0; i < 4; i++) {
                    uint16_t h = (uint16_t)((v >> (i * 16)) & 0xFFFF);
                    r |= (uint64_t)(((h & 0xFF) << 8) | ((h >> 8) & 0xFF)) << (i * 16);
                }
                vregs[inst.dest] = r;
                break;
            }

            case IROp::REV32: {
                uint64_t v = vregs[inst.src1], r = 0;
                for (int i = 0; i < 2; i++) {
                    uint32_t w = (uint32_t)((v >> (i * 32)) & 0xFFFFFFFF);
                    r |= (uint64_t)__builtin_bswap32(w) << (i * 32);
                }
                vregs[inst.dest] = r;
                break;
            }

            case IROp::REV64:
                vregs[inst.dest] = __builtin_bswap64(vregs[inst.src1]);
                break;

            case IROp::ADDS: {
                uint64_t a = vregs[inst.src1];
                uint64_t b = vregs[inst.src2];
                uint64_t r = a + b;
                vregs[inst.dest] = r;
                uint64_t n = (r >> 63) & 1;
                uint64_t z = (r == 0) ? 1 : 0;
                uint64_t c = (r < a) ? 1 : 0;
                uint64_t v = ((~(a ^ b)) & (a ^ r) & (1ULL << 63)) ? 1 : 0;
                cpu.pstate = (uint32_t)((n << 31) | (z << 30) | (c << 29) | (v << 28));
                break;
            }

            case IROp::SUBS: {
                uint64_t a = vregs[inst.src1];
                uint64_t b = vregs[inst.src2];
                uint64_t r = a - b;
                vregs[inst.dest] = r;
                uint64_t n = (r >> 63) & 1;
                uint64_t z = (r == 0) ? 1 : 0;
                uint64_t c = (a >= b) ? 1 : 0;
                uint64_t v = ((a ^ b) & (a ^ r) & (1ULL << 63)) ? 1 : 0;
                cpu.pstate = (uint32_t)((n << 31) | (z << 30) | (c << 29) | (v << 28));
                break;
            }

            case IROp::ADCS: {
                uint64_t a = vregs[inst.src1];
                uint64_t b = vregs[inst.src2];
                uint64_t cin = cpu.flag_c() ? 1 : 0;
                uint64_t r = a + b + cin;
                vregs[inst.dest] = r;
                uint64_t n = (r >> 63) & 1;
                uint64_t z = (r == 0) ? 1 : 0;
                uint64_t c = (r < a) || (cin && r == a) ? 1 : 0;
                uint64_t v = ((~(a ^ b)) & (a ^ r) & (1ULL << 63)) ? 1 : 0;
                cpu.pstate = (uint32_t)((n << 31) | (z << 30) | (c << 29) | (v << 28));
                break;
            }

            case IROp::SBCS: {
                uint64_t a = vregs[inst.src1];
                uint64_t b = vregs[inst.src2];
                uint64_t cin = cpu.flag_c() ? 1 : 0;
                // a - b - (1 - cin) = a + ~b + cin
                uint64_t nb = ~b;
                uint64_t r = a + nb + cin;
                vregs[inst.dest] = r;
                uint64_t n = (r >> 63) & 1;
                uint64_t z = (r == 0) ? 1 : 0;
                uint64_t c = (a + nb + cin) > a ? 1 : 0;  // carry out of add
                uint64_t v = ((~(a ^ nb)) & (a ^ r) & (1ULL << 63)) ? 1 : 0;
                cpu.pstate = (uint32_t)((n << 31) | (z << 30) | (c << 29) | (v << 28));
                break;
            }

            case IROp::TST: {
                uint64_t r = vregs[inst.src1] & vregs[inst.src2];
                uint64_t n = (r >> 63) & 1;
                uint64_t z = (r == 0) ? 1 : 0;
                cpu.pstate = (uint32_t)((n << 31) | (z << 30));
                break;
            }

            case IROp::TST_ZERO: {
                // CBZ/CBNZ: set Z=(val==0), clear N/C/V
                uint64_t z = (vregs[inst.src1] == 0) ? 1 : 0;
                cpu.pstate = (uint32_t)(z << 30);
                break;
            }

            case IROp::BRCOND_ZERO: {
                // CBZ/CBNZ: branch on (val == 0), no flag modification.
                bool is_zero = (vregs[inst.src1] == 0);
                bool take = (inst.cond == 0) ? is_zero : !is_zero;
                if (take) {
                    // Branch taken — set PC to target.
                    // The executor loop will see the PC change.
                    cpu.pc = inst.imm;
                }
                // else: fall through (PC += 4 done by caller)
                break;
            }

            case IROp::BRCOND_BIT: {
                // TBZ/TBNZ: branch on ((val >> bit) & 1), no flag modification.
                // width = bit number, cond=0 (EQ) for TBZ, cond=1 (NE) for TBNZ.
                bool bit_set = (vregs[inst.src1] >> inst.width) & 1;
                bool take = (inst.cond == 0) ? !bit_set : bit_set;
                if (take) {
                    cpu.pc = inst.imm;
                }
                break;
            }

            case IROp::CSEL:
                vregs[inst.dest] = cond_true(inst.cond, cpu.pstate)
                                 ? vregs[inst.src1] : vregs[inst.src2];
                break;

            case IROp::CSINC: {
                bool t = cond_true(inst.cond, cpu.pstate);
                uint64_t s2 = vregs[inst.src2];
                vregs[inst.dest] = t ? vregs[inst.src1] : (s2 + 1);
                break;
            }

            case IROp::CSINV: {
                bool t = cond_true(inst.cond, cpu.pstate);
                uint64_t s2 = vregs[inst.src2];
                vregs[inst.dest] = t ? vregs[inst.src1] : ~s2;
                break;
            }

            case IROp::CSNEG: {
                bool t = cond_true(inst.cond, cpu.pstate);
                uint64_t s2 = vregs[inst.src2];
                vregs[inst.dest] = t ? vregs[inst.src1] : -(int64_t)s2;
                break;
            }

            case IROp::CCMP: {
                if (cond_true(inst.cond, cpu.pstate)) {
                    uint64_t a = vregs[inst.src1];
                    uint64_t b = vregs[inst.src2];
                    uint64_t r = inst.flags_op ? (a - b) : (a + b);
                    uint64_t n = (r >> 63) & 1;
                    uint64_t z = (r == 0) ? 1 : 0;
                    uint64_t c = inst.flags_op ? (a >= b ? 1 : 0) : (r < a ? 1 : 0);
                    uint64_t v = inst.flags_op
                        ? (((a ^ b) & (a ^ r) & (1ULL << 63)) ? 1 : 0)
                        : (((~(a ^ b)) & (a ^ r) & (1ULL << 63)) ? 1 : 0);
                    cpu.pstate = (uint32_t)((n << 31) | (z << 30) | (c << 29) | (v << 28));
                } else {
                    cpu.pstate = (uint32_t)(inst.width << 28);  // nzcv_field in width
                }
                break;
            }

            case IROp::BFM: case IROp::UBFM: case IROp::SBFM: case IROp::EXTR: {
                // These have rich semantics; route through interpreter for
                // correctness in the executor path. The JIT (frostjit.cpp)
                // has direct codegen for these.
                uint64_t save_pc = cpu.pc;
                cpu.pc = inst.arm_pc;
                emu.step_public(cpu);
                // Sync vregs from cpu state.
                for (int j = 0; j < 31; j++) vregs[j] = cpu.regs[j];
                vregs[31] = cpu.sp;
                (void)save_pc;
                break;
            }

            case IROp::BR: {
                for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
                cpu.sp = vregs[31];
                return vregs[inst.src1];
            }

            case IROp::BRCOND: {
                bool taken = cond_true(inst.cond, cpu.pstate);
                if (taken) {
                    for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
                    cpu.sp = vregs[31];
                    return inst.imm;
                }
                break;
            }

            case IROp::BRCOND_FALLTHRU: {
                // Unconditional branch.
                for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
                cpu.sp = vregs[31];
                return inst.imm;
            }

            case IROp::CALL_INTERP: {
                for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
                cpu.sp = vregs[31];
                cpu.pc = inst.arm_pc;
                emu.step_public(cpu);
                for (int j = 0; j < 31; j++) vregs[j] = cpu.regs[j];
                vregs[31] = cpu.sp;
                if (cpu.pc != inst.arm_pc + 4) {
                    return cpu.pc;
                }
                break;
            }

            case IROp::SVC: {
                for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
                cpu.sp = vregs[31];
                cpu.pc = inst.arm_pc;
                emu.syscall_public(cpu);
                // SVC always ends the block — return new PC.
                return cpu.pc;
            }

            case IROp::FMOV_G2F:
                cpu.v_lo[inst.dest] = vregs[inst.src1];
                cpu.v_hi[inst.dest] = 0;
                break;
            case IROp::FMOV_F2G:
                vregs[inst.dest] = cpu.v_lo[inst.src1];
                break;
            case IROp::FMOV_G2FHI:
                cpu.v_hi[inst.dest] = vregs[inst.src1];
                break;
            case IROp::FMOV_FHI2G:
                vregs[inst.dest] = cpu.v_hi[inst.src1];
                break;

            case IROp::FP_BINOP: {
                uint8_t opc = (uint8_t)inst.imm;
                if (inst.width == 1) {  // double
                    double a, b, r = 0;
                    memcpy(&a, &cpu.v_lo[inst.src1], 8);
                    memcpy(&b, &cpu.v_lo[inst.src2], 8);
                    switch (opc) {
                        case 0: r = a * b; break;
                        case 1: r = a / b; break;
                        case 2: r = a + b; break;
                        case 3: r = a - b; break;
                        case 4: r = (a > b) ? a : b; break;
                        case 5: r = (a < b) ? a : b; break;
                        case 6: r = -(a * b); break;
                    }
                    cpu.v_lo[inst.dest] = 0; memcpy(&cpu.v_lo[inst.dest], &r, 8);
                    cpu.v_hi[inst.dest] = 0;
                } else {  // single
                    float a, b, r = 0;
                    uint32_t ta = (uint32_t)cpu.v_lo[inst.src1];
                    uint32_t tb = (uint32_t)cpu.v_lo[inst.src2];
                    memcpy(&a, &ta, 4); memcpy(&b, &tb, 4);
                    switch (opc) {
                        case 0: r = a * b; break;
                        case 1: r = a / b; break;
                        case 2: r = a + b; break;
                        case 3: r = a - b; break;
                        case 4: r = (a > b) ? a : b; break;
                        case 5: r = (a < b) ? a : b; break;
                        case 6: r = -(a * b); break;
                    }
                    uint32_t tr; memcpy(&tr, &r, 4);
                    cpu.v_lo[inst.dest] = tr; cpu.v_hi[inst.dest] = 0;
                }
                break;
            }
            case IROp::FP_UNOP: {
                uint8_t opc = (uint8_t)inst.imm;
                if (inst.width == 1) {  // double
                    double a, r = 0;
                    memcpy(&a, &cpu.v_lo[inst.src1], 8);
                    switch (opc) {
                        case 0: r = a; break;
                        case 1: r = __builtin_fabs(a); break;
                        case 2: r = -a; break;
                        case 3: r = __builtin_sqrt(a); break;
                    }
                    cpu.v_lo[inst.dest] = 0; memcpy(&cpu.v_lo[inst.dest], &r, 8);
                    cpu.v_hi[inst.dest] = 0;
                } else {  // single
                    float a, r = 0;
                    uint32_t ta = (uint32_t)cpu.v_lo[inst.src1];
                    memcpy(&a, &ta, 4);
                    switch (opc) {
                        case 0: r = a; break;
                        case 1: r = __builtin_fabsf(a); break;
                        case 2: r = -a; break;
                        case 3: r = __builtin_sqrtf(a); break;
                    }
                    uint32_t tr; memcpy(&tr, &r, 4);
                    cpu.v_lo[inst.dest] = tr; cpu.v_hi[inst.dest] = 0;
                }
                break;
            }

            case IROp::SIMD_LOGICAL: {
                uint8_t opc = (uint8_t)inst.imm;
                uint64_t lo = cpu.v_lo[inst.src1], hi = cpu.v_hi[inst.src1];
                uint64_t lo2 = cpu.v_lo[inst.src2], hi2 = cpu.v_hi[inst.src2];
                switch (opc) {
                    case 0: lo &= lo2; hi &= hi2; break;  // AND
                    case 1: lo |= lo2; hi |= hi2; break;  // ORR
                    case 2: lo ^= lo2; hi ^= hi2; break;  // EOR
                    case 3: lo &= ~lo2; hi &= ~hi2; break; // BIC
                    case 4: lo |= ~lo2; hi |= ~hi2; break; // ORN
                    case 5: lo ^= ~lo2; hi ^= ~hi2; break; // EON
                }
                cpu.v_lo[inst.dest] = lo;
                cpu.v_hi[inst.dest] = hi;
                break;
            }
            case IROp::SIMD_DUP:
                cpu.v_lo[inst.dest] = vregs[inst.src1];
                cpu.v_hi[inst.dest] = vregs[inst.src1];
                break;
            case IROp::SIMD_MOVI:
                cpu.v_lo[inst.dest] = inst.imm;
                cpu.v_hi[inst.dest] = inst.imm;
                break;
            case IROp::SIMD_LDST:
                if (inst.width == 1) { // load vregs → v_lo/v_hi
                    cpu.v_lo[inst.dest] = vregs[inst.src1];
                    cpu.v_hi[inst.dest] = vregs[inst.src2];
                } else { // store v_lo/v_hi → vregs
                    vregs[inst.src1] = cpu.v_lo[inst.dest];
                    vregs[inst.src2] = cpu.v_hi[inst.dest];
                }
                break;

            case IROp::FP_F2I: {
                bool is_unsigned = (inst.imm == 1);
                if (inst.width == 1) {
                    double a; memcpy(&a, &cpu.v_lo[inst.src1], 8);
                    int64_t r = (int64_t)a;
                    vregs[inst.dest] = is_unsigned ? (uint64_t)r : (uint64_t)r;
                } else {
                    float a; uint32_t tb = (uint32_t)cpu.v_lo[inst.src1];
                    memcpy(&a, &tb, 4);
                    int32_t r = (int32_t)a;
                    vregs[inst.dest] = (uint32_t)r;
                }
                break;
            }
            case IROp::FP_I2F: {
                bool is_unsigned = (inst.imm == 1);
                if (inst.width == 1) {
                    double r = is_unsigned ? static_cast<double>(static_cast<uint64_t>(vregs[inst.src1]))
                                          : static_cast<double>(static_cast<int64_t>(vregs[inst.src1]));
                    cpu.v_lo[inst.dest] = 0; memcpy(&cpu.v_lo[inst.dest], &r, 8);
                    cpu.v_hi[inst.dest] = 0;
                } else {
                    float r = is_unsigned ? static_cast<float>(static_cast<uint32_t>(vregs[inst.src1]))
                                         : static_cast<float>(static_cast<int32_t>(vregs[inst.src1]));
                    uint32_t tr; memcpy(&tr, &r, 4);
                    cpu.v_lo[inst.dest] = tr; cpu.v_hi[inst.dest] = 0;
                }
                break;
            }
            case IROp::FP_CMP: {
                bool is_double = (inst.width == 1);
                bool unordered = false;
                if (is_double) {
                    double a, b;
                    memcpy(&a, &cpu.v_lo[inst.src1], 8);
                    if (inst.src2 != 0 || inst.imm != 0) {
                        memcpy(&b, &cpu.v_lo[inst.src2], 8);
                    } else { b = 0.0; }
                    if (a != a || b != b) unordered = true;
                    else if (a < b) cpu.pstate = 0x80000000;
                    else if (a == b) cpu.pstate = 0x60000000;
                    else cpu.pstate = 0x20000000;
                } else {
                    float a, b;
                    uint32_t ta = (uint32_t)cpu.v_lo[inst.src1];
                    memcpy(&a, &ta, 4);
                    if (inst.src2 != 0 || inst.imm != 0) {
                        uint32_t tb = (uint32_t)cpu.v_lo[inst.src2];
                        memcpy(&b, &tb, 4);
                    } else { b = 0.0f; }
                    if (a != a || b != b) unordered = true;
                    else if (a < b) cpu.pstate = 0x80000000;
                    else if (a == b) cpu.pstate = 0x60000000;
                    else cpu.pstate = 0x20000000;
                }
                if (unordered) cpu.pstate = 0x28000000; // N=0, Z=0, C=1, V=1
                break;
            }
            case IROp::FP_MOVI:
                cpu.v_lo[inst.dest] = inst.imm;
                cpu.v_hi[inst.dest] = 0;
                break;

            default:
                break;
        }
    }

    // Block fell through — store regs back, return fall-through PC.
    for (int j = 0; j < 31; j++) cpu.regs[j] = vregs[j];
    cpu.sp = vregs[31];
    return block.start_pc + block.count * 4;
}

} // namespace arm64emu
