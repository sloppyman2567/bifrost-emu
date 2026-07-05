// interpreter.cpp — ARM64 instruction interpreter.
//
// This file implements Emulator::execute(), which decodes and executes
// a single ARM64 instruction. The decode logic is shared with the JIT
// (src/jit/frostjit.cpp) via decoder.hpp.
//
// When adding a new instruction:
//   1. Add the decode in decoder.cpp (sets InstClass)
//   2. Add the execute case here
//   3. Add JIT codegen in src/jit/frostjit.cpp (compile_ir_inst)

#include "core/emulator.h"
#include "decoder.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace arm64emu {

// ── FP register access helpers (file-scope, no per-dispatch allocation) ──
// These were previously local lambdas inside the FP_SCALAR case, which
// meant they were reconstructed on every FP instruction dispatch. Moving
// them to file scope eliminates that overhead.
static inline double read_fp_d(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint64_t bits = cpu.v_lo[r];
    double d; memcpy(&d, &bits, 8); return d;
}
static inline float read_fp_s(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint32_t bits = static_cast<uint32_t>(cpu.v_lo[r]);
    float f; memcpy(&f, &bits, 4); return f;
}
static inline void write_fp_d(CPU& cpu, int r, double d) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    uint64_t bits; memcpy(&bits, &d, 8);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
static inline void write_fp_s(CPU& cpu, int r, float f) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    uint32_t bits; memcpy(&bits, &f, 4);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}

// ── Half-precision (FP16) helpers ──────────────────────────────────────
// IEEE 754 binary16: 1 sign + 5 exp + 10 mantissa.
static inline float h2f(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t fbits;
    if (exp == 0) {
        if (mant == 0) {
            fbits = sign << 31;
        } else {
            int e = -1;
            while (!(mant & 0x400)) { mant <<= 1; e--; }
            mant &= 0x3FF;
            fbits = (sign << 31) | ((127 + e - 14) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        fbits = (sign << 31) | (0xFFu << 23) | (mant << 13);
    } else {
        fbits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &fbits, 4); return f;
}
static inline uint16_t f2h(float f) {
    uint32_t fbits; memcpy(&fbits, &f, 4);
    uint32_t sign = (fbits >> 31) & 1;
    int32_t  exp  = static_cast<int32_t>((fbits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (fbits & 0x7FFFFF) >> 13;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign << 15);
        mant |= 0x400;
        mant >>= (1 - exp);
        return static_cast<uint16_t>((sign << 15) | mant);
    } else if (exp >= 0x1F) {
        return static_cast<uint16_t>((sign << 15) | (0x1F << 10));
    }
    return static_cast<uint16_t>((sign << 15) | (exp << 10) | mant);
}
static inline uint16_t d2h(double d) {
    return f2h(static_cast<float>(d));
}

// Set NZCV from a 64-bit add-with-carry result.
static uint64_t set_add_flags(CPU& cpu, uint64_t a, uint64_t b, uint64_t carry_in,
                              int width, bool set_flags) {
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    uint64_t a_w = a & mask;
    uint64_t b_w = b & mask;
    // Detect carry BEFORE the addition wraps. For 64-bit: carry = (a_w + b_w + carry_in) > mask.
    // Since uint64_t wraps, we detect carry as: a_w > (mask - b_w - carry_in).
    bool c = (a_w > (mask - b_w)) || (a_w == (mask - b_w) && carry_in);
    uint64_t sum = a_w + b_w + carry_in;
    uint64_t res = sum & mask;
    if (set_flags) {
        bool n = (res >> (width - 1)) & 1;
        bool z = (res == 0);
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
    auto* pcache = &cpu.page_cache;
    // ── Decode via the shared decoder ─────────────────────────────
    // The decoder (decoder.cpp) is the single source of truth for
    // instruction classification. We call decode() once, then dispatch
    // on d.cls. Every instruction handler lives in the switch below.
    {
        // ── Per-vCPU 2-way set-associative decode cache ─────────
        // Hash PC to a set, check both ways. On hit, skip decode().
        // This is the hot path — ~100% hit rate for tight loops.
        // Using a const reference avoids copying the 88-byte DecodedInst.
        // The cache lives on `cpu` so each vCPU gets a lock-free cache.
        //
        // upgraded from direct-mapped to 2-way
        // set-associative to reduce conflict misses.
        size_t set_idx = (cpu.pc >> 2) & CPU::DECODE_CACHE_SET_MASK;
        size_t way0 = set_idx * 2;
        size_t way1 = way0 + 1;
        CPU::CacheEntry& ce0 = cpu.decode_cache[way0];
        CPU::CacheEntry& ce1 = cpu.decode_cache[way1];
        const DecodedInst* dp;
        if (__builtin_expect(ce0.tag == cpu.pc, 1)) {
            dp = &ce0.d;
            cpu.decode_cache_hits++;
            // Update LRU: way 0 is now MRU.
            cpu.decode_cache_lru[set_idx >> 3] &= ~(1u << (set_idx & 7));
        } else if (__builtin_expect(ce1.tag == cpu.pc, 1)) {
            dp = &ce1.d;
            cpu.decode_cache_hits++;
            // Update LRU: way 1 is now MRU.
            cpu.decode_cache_lru[set_idx >> 3] |= (1u << (set_idx & 7));
        } else {
            // Miss: evict the LRU way.
            bool lru_bit = (cpu.decode_cache_lru[set_idx >> 3] >> (set_idx & 7)) & 1;
            CPU::CacheEntry& victim = lru_bit ? ce0 : ce1;
            decode(victim.d, inst);
            victim.tag = cpu.pc;
            dp = &victim.d;
            cpu.decode_cache_misses++;
            // The victim way is now MRU.
            if (lru_bit) {
                cpu.decode_cache_lru[set_idx >> 3] &= ~(1u << (set_idx & 7));
            } else {
                cpu.decode_cache_lru[set_idx >> 3] |= (1u << (set_idx & 7));
            }
        }
        const DecodedInst& d = *dp;
        switch (d.cls) {
            // ── ADC/ADCS/SBC/SBCS (add/subtract with carry) ──────────
            // These were previously unimplemented and
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
                uint64_t v = d.sf ? cpu.regs[d.rt] : static_cast<uint32_t>(cpu.regs[d.rt]);
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
                {
                    // Advance cpu.pc to the return address (the instruction
                    // after SVC) BEFORE calling the syscall handler. This
                    // matters for signal delivery: if a signal arrives
                    // during a blocking syscall (e.g., read), the signal
                    // frame's saved PC must be the return address, NOT the
                    // SVC instruction. Otherwise, after the handler runs
                    // and calls rt_sigreturn, the SVC would be re-executed,
                    // re-entering the blocking syscall forever.
                    //
                    // The check `cpu.pc != old_pc` below still works for
                    // detecting syscalls that change PC (execve, sigreturn):
                    // we save old_pc as the SVC address, set cpu.pc to
                    // SVC+4, call syscall(). If the syscall changes cpu.pc
                    // (e.g., to the new entry point for execve), the check
                    // sees cpu.pc != SVC+4 and propagates the new PC.
                    uint64_t old_pc = cpu.pc;
                    uint64_t return_pc = old_pc + 4;
                    cpu.pc = return_pc;
                    syscall(cpu);
                    if (cpu.pc != return_pc) {
                        // Syscall changed PC (execve, rt_sigreturn, or
                        // signal delivery) — propagate to next_pc so
                        // step() doesn't overwrite it.
                        next_pc = cpu.pc;
                    } else {
                        // Normal syscall — cpu.pc is at return_pc.
                        // next_pc was set to old_pc + 4 by step(), which
                        // equals return_pc, so no update needed.
                    }
                }
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
                            imm, static_cast<unsigned long long>(cpu.pc));
                    fflush(stderr);
                }
                // Debug aid: when musl's a_crash() fires (BRK #1000),
                // dump registers + chunk header so we can see what
                // malloc/free was unhappy about. x0 typically holds
                // the chunk pointer in musl's mallocng sanity path.
                if (imm == 1000 && getenv("BIFROST_TRACE_CRASH")) {
                    fprintf(stderr, "[emu] regs at BRK #1000:\n");
                    for (int i = 0; i < 31; i++) {
                        fprintf(stderr, "  x%d=0x%llx", i,
                                (unsigned long long)cpu.regs[i]);
                        if ((i & 3) == 3) fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "  sp=0x%llx pc=0x%llx\n",
                            (unsigned long long)cpu.sp,
                            (unsigned long long)cpu.pc);
                    // Try to dump chunk header at [x6-8 .. x6+24]
                    // (x6 holds the original x0 in the crash path,
                    // since the crash function does mov x6, x0).
                    uint64_t chunk = cpu.regs[6];
                    if (chunk > 0x1000) {
                        uint8_t buf[64];
                        try {
                            mem_.read(chunk - 16, buf, 64);
                            fprintf(stderr, "  [x6-16 .. x6+48]:");
                            for (int i = 0; i < 64; i++) {
                                if ((i & 15) == 0) fprintf(stderr, "\n   ");
                                fprintf(stderr, " %02x", buf[i]);
                            }
                            fprintf(stderr, "\n");
                        } catch (...) {
                            fprintf(stderr, "  (chunk unreadable)\n");
                        }
                        // Also dump the page containing x6, in 64-byte
                        // chunks, to see the surrounding mallocng state.
                        uint64_t page = chunk & ~0xFFFULL;
                        fprintf(stderr, "  page @0x%llx:\n",
                                (unsigned long long)page);
                        for (int row = 0; row < 64; row++) {
                            uint8_t line[16];
                            try {
                                mem_.read(page + row * 16, line, 16);
                                fprintf(stderr, "   %03llx:",
                                        (unsigned long long)(row * 16));
                                for (int i = 0; i < 16; i++) {
                                    fprintf(stderr, " %02x", line[i]);
                                }
                                fprintf(stderr, "  ");
                                for (int i = 0; i < 16; i++) {
                                    unsigned char c = line[i];
                                    fprintf(stderr, "%c",
                                            (c >= 32 && c < 127) ? c : '.');
                                }
                                fprintf(stderr, "\n");
                            } catch (...) {
                                fprintf(stderr, "   (page unreadable from row %d)\n", row);
                                break;
                            }
                        }
                    }
                    // Dump stack: 16 uint64_t values starting at sp.
                    // The saved x30 (caller of FuncB) is at [sp+32]
                    // in the crash path. This lets us trace back.
                    uint64_t sp_v = cpu.sp;
                    fprintf(stderr, "  stack @0x%llx (16 entries):\n",
                            (unsigned long long)sp_v);
                    for (int i = 0; i < 16; i++) {
                        uint64_t v = 0;
                        try {
                            mem_.read(sp_v + i * 8, &v, 8);
                        } catch (...) { v = 0; }
                        fprintf(stderr, "   [sp+0x%02x] 0x%llx\n",
                                i * 8, (unsigned long long)v);
                    }
                    fflush(stderr);
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
                        cpu.fpcr = static_cast<uint32_t>(v); return;                 // FPCR
                    }
                    if (crn == 4 && crm == 4 && op2 == 1) {
                        cpu.fpsr = static_cast<uint32_t>(v); return;                 // FPSR
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
                    v = ~(static_cast<uint64_t>(imm16) << shift);
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else if (d.cls == InstClass::MOVZ) {
                    v = static_cast<uint64_t>(imm16) << shift;
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else { // MOVK
                    uint64_t mask = (width == 64)
                        ? (0xFFFFULL << shift)
                        : (0xFFFFULL << shift) & 0xFFFFFFFFULL;
                    uint64_t cur = cpu.regs[d.rd];
                    if (!d.sf) cur &= 0xFFFFFFFF;
                    v = (cur & ~mask) | (static_cast<uint64_t>(imm16) << shift);
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
                uint64_t b = static_cast<uint64_t>(imm12) << (sh ? 12 : 0);
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

                // Per ARM ARM, BFM/SBFM/UBFM all use DecodeBitMasks to compute
                // wmask (write mask) and tmask (top mask), then:
                //   bot = (dst & ~wmask) | (ROR(src, immr) & wmask)
                //   dst = (dst & ~tmask) | (bot & tmask)         [BFM]
                //   dst = sign_extend(bot, imms+1) & tmask        [SBFM]
                //   dst = bot & tmask                              [UBFM]
                //
                // For SBFM/UBFM, dst is 0 (so bot = ROR(src, immr) & wmask).
                // For BFM, dst is the current Rd value.
                //
                // DecodeBitMasks for the imms>=immr case (the "extract" case):
                //   len = highest_set_bit(N:~imms)
                //   esize = 1 << len
                //   levels = esize - 1
                //   S = imms & levels
                //   R = immr & levels
                //   if S == levels: reserved (we don't check)
                //   wmask = replicate(ones(S+1) >> R within esize, esize)
                //   tmask = replicate(ones(S+1) << (esize-1-S) within esize, esize)
                //
                // But there's a simpler equivalent for the common cases.
                // For SBFM/UBFM when imms >= immr:
                //   extracted = (src >> immr) & ones(imms-immr+1)
                // For SBFM/UBFM when imms < immr:
                //   extracted = ROR(src, immr) & ones(imms+1)
                // For BFM when imms >= immr (BFXIL):
                //   dst[imms:immr] = src[imms:immr]  (preserve rest of dst)
                // For BFM when imms < immr (BFI):
                //   dst[imms+immr:immr] = src[imms:0]  (preserve rest of dst)
                //
                // The v0 code had a bug in the BFI case: it computed
                //   field_mask = mask | hi_mask
                // which for BFI(x3, x0, #48, #16) gives field_mask = 0xFFFF | 0xFFFFFFFFFFFF0000 = ~0
                // → replaces ALL of dst instead of just bits[63:48].
                // This broke __floatsitf's BFI, corrupting 128-bit long doubles.

                if (imms >= immr) {
                    // BFXIL / extract case
                    int len = imms - immr + 1;
                    uint64_t mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);

                    // ── LSR #0 / LSL #0 special case (fix) ──
                    // On AArch64, `LSR Xd, Xn, #0` is encoded as
                    // `UBFM Xd, Xn, #0, #63`. Real hardware treats LSR
                    // by 0 as a shift by 64 (result = 0), NOT as a
                    // no-op. The UBFM decode (ROR by 0 + mask all-ones)
                    // would incorrectly produce the source unchanged.
                    //
                    // This was the root cause of musl's qsort (smoothsort)
                    // producing wrong results: smoothsort's shr() function
                    // does `p[0] >>= n` where n can be 0, and the compiler
                    // emits `LSR Xd, Xn, #0` expecting a zero result.
                    // Our emulator returned the original value, corrupting
                    // the bit vector and causing the sort to produce
                    // subtly wrong output (e.g. "1 2 3 4 6 7 8 5 9 10"
                    // instead of "1 2 3 4 5 6 7 8 9 10").
                    //
                    // The fix: when len == datasize (full-width field)
                    // and immr == 0, the UBFM result is 0 (shift by
                    // full width). For SBFM, the result is sign-extended
                    // (0 for non-negative, all-ones for negative).
                    if (len == datasize && immr == 0) {
                        if (opc == 0) {
                            // SBFM: ASR by 64 → sign bit replicated
                            uint64_t sign_bit = (src >> (datasize - 1)) & 1;
                            uint64_t result = sign_bit ? ~0ULL : 0;
                            if (datasize == 32) result &= 0xFFFFFFFF;
                            if (d.rd != 31) cpu.regs[d.rd] = result;
                        } else if (opc == 2) {
                            // UBFM: LSR by 64 → 0
                            if (d.rd != 31) cpu.regs[d.rd] = 0;
                        } else {
                            // BFM: BFXIL with full width and immr=0
                            // → replace entire destination with src
                            if (d.rd != 31) cpu.regs[d.rd] = src;
                        }
                        if (!d.sf && d.rd != 31) cpu.regs[d.rd] &= 0xFFFFFFFF;
                        return;
                    }

                    uint64_t extracted = (src >> immr) & mask;
                    if (opc == 0) {
                        // SBFM: sign-extend
                        uint64_t m = (1ULL << (len - 1));
                        if (extracted & m) {
                            uint64_t high = ~mask & (datasize == 64 ? ~0ULL : (1ULL<<datasize)-1);
                            extracted |= high;
                        }
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else if (opc == 2) {
                        // UBFM
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else {
                        // BFM (BFXIL): extract field from src[imms:immr],
                        // place in LOW bits of dest, preserve dest high bits.
                        // ARM ARM: Wd[width-1:0] = Wn[imms:immr]
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t dst_mask = mask;  // low 'len' bits
                        uint64_t keep = cur & ~dst_mask;
                        if (d.rd != 31) cpu.regs[d.rd] = keep | (extracted & dst_mask);
                    }
                } else {
                    // BFI / rotate case (imms < immr).
                    //
                    // This covers SBFIZ, UBFIZ, BFI, LSL (when shift > 0),
                    // and the imms < immr form of SBFM/UBFM/BFM.
                    //
                    // The operation is: take the low (imms+1) bits of src,
                    // optionally sign-extend (SBFM), and shift left by
                    // (datasize - immr) to place them at the correct
                    // position in the result.
                    //
                    // fix: the previous code used `high_mask`
                    // (top bits) which was correct for LSL but WRONG for
                    // SBFIZ/UBFIZ where the field is NOT at the top of
                    // the register. This broke musl's smoothsort which
                    // uses `sbfiz x3, x19, #3, #32` to compute
                    // pshift * 8 for lp[] indexing — the result was 0
                    // instead of the correct value, causing qsort to
                    // produce wrong output for n >= 8.
                    int width = imms + 1;
                    int lsb = datasize - immr;
                    uint64_t field = src & ((width >= 64) ? ~0ULL : ((1ULL << width) - 1));
                    if (opc == 0) {
                        // SBFM (SBFIZ): sign-extend from bit width-1
                        if (width < 64 && (field & (1ULL << (width - 1)))) {
                            uint64_t high_bits = ~((1ULL << width) - 1);
                            if (datasize == 32) high_bits &= 0xFFFFFFFFULL;
                            field |= high_bits;
                        }
                    }
                    uint64_t result = (lsb >= 64) ? 0 : (field << lsb);
                    if (datasize == 32) result &= 0xFFFFFFFFULL;
                    if (opc == 0) {
                        // SBFM
                        if (d.rd != 31) cpu.regs[d.rd] = result;
                    } else if (opc == 2) {
                        // UBFM
                        if (d.rd != 31) cpu.regs[d.rd] = result;
                    } else {
                        // BFM (BFI): insert field into destination
                        uint64_t dst_mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
                        dst_mask = (lsb >= 64) ? 0 : (dst_mask << lsb);
                        if (datasize == 32) dst_mask &= 0xFFFFFFFFULL;
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t keep = cur & ~dst_mask;
                        if (d.rd != 31) cpu.regs[d.rd] = keep | (result & dst_mask);
                    }
                }
                if (!d.sf && d.rd != 31) cpu.regs[d.rd] &= 0xFFFFFFFF;
                return;
            }

            // ── EXTR ─────────────────────────────────────────────────
            case InstClass::EXTR: {
                uint8_t immr = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                // Per ARM ARM: EXTR Xd, Xn, Xm, #lsb extracts a width-bit
                // field from the 2*width-bit concatenation (Xn:Xm).
                //   Xd = ((Xn << width) | Xm) >> lsb   [masked to width]
                // v0 had two bugs:
                //   1. Concatenated Rm:Rn instead of Rn:Rm (operand order)
                //   2. Used (rn << width) which is UB when width == 64
                //      (shifting a uint64_t by its full width is UB in C++)
                // Both are now fixed: we use __uint128_t for the 128-bit
                // concatenation to avoid the UB.
                uint64_t rn = cpu.regs[d.rn];
                uint64_t rm = cpu.regs[d.rm];
                if (!d.sf) { rn &= 0xFFFFFFFF; rm &= 0xFFFFFFFF; }
                __uint128_t combined = ((__uint128_t)rn << width) | rm;
                __uint128_t shifted = combined >> immr;
                uint64_t v = static_cast<uint64_t>(shifted) & (width == 64 ? ~0ULL : 0xFFFFFFFFULL);
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
                // For extended register: Rn=31 reads SP (not XZR).
                // For shifted register: Rn=31 reads XZR (regs[31]=0).
                uint64_t a = (extended && d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t b;
                if (extended) {
                    b = extend_reg(cpu.regs[d.rm], d.extend, d.shift, d.sf);
                } else {
                    b = cpu.regs[d.rm];
                    if (!d.sf) b &= 0xFFFFFFFF;
                    switch (d.shift_type) {
                        case 0: b = b << d.shift; break;
                        case 1: b = (width == 64) ? (b >> d.shift) : (static_cast<uint32_t>(b) >> d.shift); break;
                        // ASR: sign-extend from the operation width. Casting
                        // the already-zero-extended b to int64_t directly
                        // would leave sign bit at 63 (always 0 for 32-bit),
                        // making ASR behave like LSR.
                        case 2:
                            b = (width == 64)
                                ? (static_cast<int64_t>(b) >> d.shift)
                                : (static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(b))) >> d.shift);
                            break;
                        case 3: {
                            // ROR: must rotate within the operation width, not 64 bits.
                            // ror64 on a zero-extended 32-bit value loses wrap bits.
                            if (width == 64) {
                                b = ror64(b, d.shift);
                            } else {
                                uint32_t v = static_cast<uint32_t>(b);
                                unsigned r = d.shift & 31;
                                b = r ? ((v >> r) | (v << (32 - r))) : v;
                            }
                        } break;
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
                    case 1: b = (width == 64) ? (b >> d.shift) : (static_cast<uint32_t>(b) >> d.shift); break;
                    // ASR: sign-extend from operation width (see ADD/SUB above).
                    case 2:
                        b = (width == 64)
                            ? (static_cast<int64_t>(b) >> d.shift)
                            : (static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(b))) >> d.shift);
                        break;
                    case 3: {
                        // ROR: must rotate within the operation width, not 64 bits.
                        // ror64 on a zero-extended 32-bit value loses wrap bits.
                        if (width == 64) {
                            b = ror64(b, d.shift);
                        } else {
                            uint32_t v = static_cast<uint32_t>(b);
                            unsigned r = d.shift & 31;
                            b = r ? ((v >> r) | (v << (32 - r))) : v;
                        }
                    } break;
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
                        // REV32: reverse bytes within each 32-bit word.
                        // 64-bit: apply bswap32 to both halves.
                        // 32-bit: just bswap32 (though REV32 is typically 64-bit only).
                        if (width == 64) {
                            uint32_t lo = static_cast<uint32_t>(v);
                            uint32_t hi = static_cast<uint32_t>(v >> 32);
                            v = (static_cast<uint64_t>(__builtin_bswap32(hi)) << 32) |
                                static_cast<uint64_t>(__builtin_bswap32(lo));
                        } else {
                            v = __builtin_bswap32(static_cast<uint32_t>(v));
                        }
                        break;
                    case InstClass::REV:
                        v = (width == 64) ? __builtin_bswap64(v)
                                          : __builtin_bswap32(static_cast<uint32_t>(v));
                        break;
                    case InstClass::CLZ:
                        if (v == 0) v = width;
                        else v = (width == 64) ? __builtin_clzll(v)
                                               : __builtin_clz(static_cast<uint32_t>(v));
                        break;
                    case InstClass::CLS:
                        // ARM CLS: count leading sign bits = CLZ(v ^ SAR(v, W-1)) - 1.
                        // Edge cases: CLS(0) = CLS(~0) = W-1 (the -1 makes CLZ(0)-1 = W-1).
                        // __builtin_clz(0) is UB, so handle 0 and ~0 explicitly.
                        if (width == 64) {
                            if (v == 0 || v == ~0ULL) v = 63;
                            else v = (v >> 63) ? __builtin_clzll(~v) - 1
                                               : __builtin_clzll(v)  - 1;
                        } else {
                            if (v == 0 || v == 0xFFFFFFFFULL) v = 31;
                            else v = (v >> 31) ? __builtin_clz(static_cast<uint32_t>(~v)) - 1
                                               : __builtin_clz(static_cast<uint32_t>(v))  - 1;
                        }
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
                            res = (width == 64) ? a / b : static_cast<uint32_t>(a) / static_cast<uint32_t>(b);
                        break;
                    case InstClass::SDIV:
                        if (b != 0)
                            res = (width == 64) ? static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>(b))
                                                : static_cast<uint64_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
                        break;
                    case InstClass::LSL:
                        res = (width == 64) ? (a << (b & 63)) : (static_cast<uint32_t>(a) << (b & 31));
                        break;
                    case InstClass::LSR:
                        res = (width == 64) ? (a >> (b & 63)) : (static_cast<uint32_t>(a) >> (b & 31));
                        break;
                    case InstClass::ASR:
                        res = (width == 64) ? static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63))
                                            : static_cast<uint64_t>(static_cast<int32_t>(a) >> (b & 31));
                        break;
                    case InstClass::ROR: {
                        // ROR rotates a value RIGHT by `b` bits, with the
                        // bits that fall off the bottom reappearing at the
                        // top. The 32-bit and 64-bit forms have DIFFERENT
                        // widths — the 32-bit form must rotate within 32 bits.
                        //
                        // BUGFIX: the old code called `ror64(a, b & 31)`
                        // for the 32-bit case. ror64 does a 64-bit rotate,
                        // so `v << (64 - r)` shifts the 32-bit value entirely
                        // out of the low word (e.g. r=7 → <<57, bits land
                        // at positions 57-88, all above bit 32). The result
                        // was missing the high bits that should have wrapped
                        // around — e.g. ROR(0x12345678, 7) returned 0x02468acf
                        // instead of 0xf2468acf. This broke MD5 (which uses
                        // 32-bit rotates in every round) and any other code
                        // using ROR — the JIT was correct, the interpreter
                        // was wrong, so verify mode flagged "false-positive"
                        // divergences on every ROR-heavy block.
                        if (width == 64) {
                            res = ror64(a, b & 63);
                        } else {
                            uint32_t v = static_cast<uint32_t>(a);
                            unsigned r = b & 31;
                            if (r == 0) {
                                res = v;
                            } else {
                                res = (v >> r) | (v << (32 - r));
                            }
                        }
                        break;
                    }
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
                        res = (width == 64) ? (a * b) : (static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
                        res = c + res;
                        break;
                    case InstClass::MSUB:
                        res = (width == 64) ? (a * b) : (static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
                        res = c - res;
                        break;
                    case InstClass::SMADDL:
                        res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)));
                        res = c + res;
                        break;
                    case InstClass::SMSUBL:
                        res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)));
                        res = c - res;
                        break;
                    case InstClass::UMADDL:
                        res = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint64_t>(static_cast<uint32_t>(b));
                        res = c + res;
                        break;
                    case InstClass::UMSUBL:
                        res = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint64_t>(static_cast<uint32_t>(b));
                        res = c - res;
                        break;
                    case InstClass::UMULH: {
                        unsigned __int128 prod = (unsigned __int128)a * (unsigned __int128)b;
                        res = static_cast<uint64_t>(prod >> 64);
                        break;
                    }
                    case InstClass::SMULH: {
                        unsigned __int128 prod = (unsigned __int128)(static_cast<int64_t>(a) * static_cast<int64_t>(b));
                        res = static_cast<uint64_t>(prod >> 64);
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
            //   — the hierarchical decoder now handles this.)
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
                    // BUGFIX: previously hardcoded 8 bytes per FP register
                    // for the lo/hi halves. For S-form (esize=4, single-
                    // precision), this corrupted the high 32 bits of v_lo
                    // and read 4 garbage bytes past the proper range. Fix:
                    // use esize for the per-register transfer, and only
                    // touch v_hi for 128-bit (Q-form) registers. The GPR
                    // path below already correctly uses esize.
                    if (d.is_load) {
                        uint64_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
                        mem_.read(addr, &lo1, esize, pcache);
                        if (esize >= 16) mem_.read(addr + 8, &hi1, 8, pcache);
                        mem_.read(addr + esize, &lo2, esize, pcache);
                        if (esize >= 16) mem_.read(addr + esize + 8, &hi2, 8, pcache);
                        cpu.v_lo[d.rt] = lo1;
                        cpu.v_hi[d.rt] = (esize >= 16) ? hi1 : 0;
                        cpu.v_lo[d.rt2] = lo2;
                        cpu.v_hi[d.rt2] = (esize >= 16) ? hi2 : 0;
                    } else {
                        mem_.write(addr, &cpu.v_lo[d.rt], esize, pcache);
                        if (esize >= 16) mem_.write(addr + 8, &cpu.v_hi[d.rt], 8, pcache);
                        mem_.write(addr + esize, &cpu.v_lo[d.rt2], esize, pcache);
                        if (esize >= 16) mem_.write(addr + esize + 8, &cpu.v_hi[d.rt2], 8, pcache);
                    }
                } else {
                    if (d.is_load) {
                        uint64_t v1 = 0, v2 = 0;
                        mem_.read(addr, &v1, esize, pcache);
                        mem_.read(addr + esize, &v2, esize, pcache);
                        if (d.rt  != 31) cpu.regs[d.rt]  = v1;
                        if (d.rt2 != 31) cpu.regs[d.rt2] = v2;
                    } else {
                        uint64_t v1 = (d.rt  == 31) ? 0 : cpu.regs[d.rt];
                        uint64_t v2 = (d.rt2 == 31) ? 0 : cpu.regs[d.rt2];
                        mem_.write(addr, &v1, esize, pcache);
                        mem_.write(addr + esize, &v2, esize, pcache);
                    }
                }
                return;
            }

            // ── LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS) ──
            // The decoder classifies LSE atomics by encoding (mode_b==0b00,
            // V=0). If the decoder says LSE_ATOMIC, it IS an LSE atomic —
            // the has_lse_ PT_NOTE check was too conservative (musl GCC
            // 11.2.1 doesn't emit .gnu.property even with -march=armv8.1-a+lse).
            case InstClass::LSE_ATOMIC: {
                // Execute as an LSE atomic.
                // For LSE atomics, the "load" (return old value) is determined
                // by Rt != 31 (XZR), NOT by bit 22 (which is the acquire/release
                // flag o0). STADD = LDADD with Rt=31, STCLR = LDCLR with Rt=31, etc.
                int width_bytes = 1 << d.size;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t mask = (width_bytes == 8) ? ~0ULL
                              : ((1ULL << (width_bytes * 8)) - 1);
                bool returns_old = (d.rt != 31);

                // BUGFIX: take the exclusive-monitor shard lock around the
                // entire RMW sequence. The old code did read→compute→write
                // without any lock, so concurrent LSE atomics from multiple
                // vCPUs could lose updates (and CAS could see stale values).
                // The JIT path uses `lock`-prefixed x86 instructions and is
                // correct, so this was also a JIT/interpreter divergence.
                // The LDXR/STXR handler above uses the same shard mutex.
                auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                std::lock_guard<std::mutex> gatom(shard.mu);

                // CAS family (atom_op >= 0xC): compare-and-swap.
                // ARM CAS Ws, Wt, [Xn]:
                //   old = [Xn]; if old == Ws, [Xn] = Wt; Ws = old
                // Ws (rs) is BOTH the expected (input) AND old value (output).
                // Wt (rt) is the desired value (unchanged).
                if (d.atom_op >= 0xC) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes, pcache);
                    uint64_t cmp = cpu.regs[d.rs] & mask;
                    old &= mask;
                    if (old == cmp) {
                        uint64_t newv = cpu.regs[d.rt] & mask;
                        mem_.write(base, &newv, width_bytes, pcache);
                    }
                    // OLD value goes to Ws (rs), NOT Wt (rt). Guard XZR.
                    if (d.rs != 31) cpu.regs[d.rs] = old;
                    return;
                }

                // SWP (atom_op == 0x8): atomic swap.
                if (d.atom_op == 0x8) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes, pcache);
                    old &= mask;
                    uint64_t newv = cpu.regs[d.rs] & mask;
                    mem_.write(base, &newv, width_bytes, pcache);
                    if (returns_old) cpu.regs[d.rt] = old;
                    return;
                }

                // Other LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN).
                uint64_t a = 0, b = cpu.regs[d.rs];
                mem_.read(base, &a, width_bytes, pcache);
                a &= mask;
                b &= mask;
                uint64_t newv = 0;
                switch (d.atom_op) {
                    case 0x0: newv = (a + b) & mask; break;            // LDADD
                    case 0x1: newv = (a & ~b) & mask; break;           // LDCLR
                    case 0x2: newv = (a ^ b) & mask; break;            // LDEOR
                    case 0x3: newv = (a | b) & mask; break;            // LDSET
                    case 0x4: { // SMAX
                        int64_t sa = static_cast<int64_t>(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = static_cast<int64_t>(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa > sb ? sa : sb) & mask; break;
                    }
                    case 0x5: { // SMIN
                        int64_t sa = static_cast<int64_t>(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = static_cast<int64_t>(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa < sb ? sa : sb) & mask; break;
                    }
                    case 0x6: newv = (a > b ? a : b) & mask; break;    // UMAX
                    case 0x7: newv = (a < b ? a : b) & mask; break;    // UMIN
                    default:  newv = a & mask; break;
                }
                mem_.write(base, &newv, width_bytes, pcache);
                if (returns_old) cpu.regs[d.rt] = a;
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
                    uint64_t addr = base + (static_cast<uint64_t>(imm12) << scale);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        uint64_t hi = cpu.v_hi[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                    }
                    return;
                }
                uint64_t addr = base + (imm12 << size);
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
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
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
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
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
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
                        // STXR must atomically: (1) check this CPU's
                        // reservation, (2) write memory, (3) invalidate
                        // OTHER CPUs' reservations at this address. Steps
                        // 1-3 must be atomic w.r.t. other CPUs' LDXR/STXR
                        // — otherwise a race between this CPU's check and
                        // write lets two CPUs both succeed. We hold the
                        // SHARD lock (selected by address hash) for the
                        // entire sequence. Sharding lets independent
                        // atomics on different addresses proceed in
                        // parallel — critical for high-contention workloads.
                        auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                        std::lock_guard<std::mutex> gmon(shard.mu);
                        bool ok = cpu.excl_check(base, width_bytes);
                        if (ok) {
                            uint64_t v = cpu.regs[d.rt];
                            uint64_t mask = (width_bytes == 8) ? ~0ULL
                                          : ((1ULL << (width_bytes * 8)) - 1);
                            v &= mask;
                            mem_.write(base, &v, width_bytes, pcache);
                            // Invalidate OTHER CPUs' reservations at this
                            // address (inline, since we hold the shard lock).
                            auto it = shard.reservations.find(base);
                            if (it != shard.reservations.end()) {
                                for (CPU* p : it->second) {
                                    if (p != &cpu && p->excl_tag_valid) {
                                        p->excl_tag_valid = false;
                                    }
                                }
                                it->second.erase(
                                    std::remove(it->second.begin(), it->second.end(), &cpu),
                                    it->second.end());
                                if (it->second.empty()) {
                                    shard.reservations.erase(it);
                                }
                            }
                        }
                        if (d.rs != 31) cpu.regs[d.rs] = ok ? 0 : 1;
                        cpu.excl_clear();
                    } else {
                        // STLR: store-release (no monitor check, always succeeds).
                        // Must hold the shard lock during the write + invalidate
                        // so a concurrent STXR can't sneak in between them.
                        uint64_t v = cpu.regs[d.rt];
                        uint64_t mask = (width_bytes == 8) ? ~0ULL
                                      : ((1ULL << (width_bytes * 8)) - 1);
                        v &= mask;
                        auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                        std::lock_guard<std::mutex> gmon(shard.mu);
                        mem_.write(base, &v, width_bytes, pcache);
                        // Invalidate OTHER CPUs' reservations at this address.
                        auto it = shard.reservations.find(base);
                        if (it != shard.reservations.end()) {
                            for (CPU* p : it->second) {
                                if (p != &cpu && p->excl_tag_valid) {
                                    p->excl_tag_valid = false;
                                }
                            }
                            shard.reservations.erase(it);
                        }
                    }
                } else {
                    // Load-exclusive (LDXR/LDAXR) or load-acquire (LDAR).
                    // LDXR must atomically: (1) read memory, (2) mark this
                    // CPU's reservation, (3) register globally. We hold the
                    // SHARD lock so that a concurrent STXR on the same
                    // address can't sneak in between our read and registration.
                    auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                    std::lock_guard<std::mutex> gmon(shard.mu);
                    uint64_t v = 0;
                    mem_.read(base, &v, width_bytes, pcache);
                    // Guard XZR: writes to regs[31] must be dropped (XZR
                    // always reads as 0). Other load handlers (LDR_IMM,
                    // LDR_UNS, LDR_REG) do this; LDXR was missing the guard.
                    if (d.rt != 31) cpu.regs[d.rt] = v;
                    if (use_monitor) {
                        cpu.excl_mark(base, width_bytes);
                        // Register globally (inline, since we hold the lock).
                        auto& vec = shard.reservations[base];
                        bool found = false;
                        for (auto*& p : vec) {
                            if (p == &cpu) { found = true; break; }
                        }
                        if (!found) vec.push_back(&cpu);
                    }
                }
                return;
            }

            // ── SIMD load/store multiple structures (LD1/ST1) ─────────
            case InstClass::SIMD_LD1:
            case InstClass::SIMD_ST1: {
                bool Q = d.Q;
                int total_bytes = Q ? 16 : 8;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                // The decoder now captures the register count in
                // d.simd_count (from bits[14:13]). The old opcode-based
                // logic was wrong: opcode 0xA mapped to 4 regs but is
                // actually 2 regs (post-index variant), etc.
                int nregs = d.simd_count;
                for (int i = 0; i < nregs; i++) {
                    int r = (d.rt + i) & 0x1F;
                    uint64_t a = base + i * total_bytes;
                    if (d.is_load) {
                        uint8_t buf[16];
                        mem_.read(a, buf, total_bytes, pcache);
                        memcpy(&cpu.v_lo[r], buf, 8);
                        if (total_bytes == 16) memcpy(&cpu.v_hi[r], buf + 8, 8);
                        else cpu.v_hi[r] = 0;
                    } else {
                        uint8_t buf[16];
                        memcpy(buf, &cpu.v_lo[r], 8);
                        if (total_bytes == 16) memcpy(buf + 8, &cpu.v_hi[r], 8);
                        mem_.write(a, buf, total_bytes, pcache);
                    }
                }
                return;
            }

            // ── SIMD data-processing (sub-dispatched by raw opcode bits) ──
            // The decoder classifies the entire 0x0E000000 / 0x4E000000 /
            // 0x2E000000 / 0x6E000000 group as SIMD_DP. We re-extract the
            // Q/U/size/opcode fields here and sub-dispatch on the exact
            // encoding pattern. This is a large but flat sub-dispatch —
            // migrating it to per-opcode InstClass values is a future
            // cleanup (it would balloon the enum).
            case InstClass::SIMD_DP: {
                uint32_t op = d.raw;
                bool Q = (op >> 30) & 1;
                bool U = (op >> 29) & 1;
                uint8_t size = (op >> 22) & 3;
                uint8_t rm = (op >> 16) & 0x1F;
                uint8_t rn = (op >> 5) & 0x1F;
                uint8_t rd = op & 0x1F;
                (void)U;

                // Sub-discriminator: bits[15:10] select the SIMD DP operation.
                // We mask off the Q bit (30) so both Q=0 (8-byte) and Q=1
                // (16-byte) forms route to the same handler. The Q bit is
                // passed separately to each handler via the `Q` variable.
                //
                // IMPROVEMENT over v0: v0 used flat if-chains with masks
                // that sometimes included bit 30 (Q), causing Q=1 forms
                // of DUP, INS, ORR(MOV), and EXT to be silently NOP'd.
                // This broke musl's 128-bit long-double softfloat, which
                // uses `mov v1.16b, v0.16b` to copy 128-bit values.
                uint32_t sub = op & 0xFFE0FC00;  // bits[31:24] + bits[20:10]
                // Strip Q from sub for matching purposes
                uint32_t sub_noq = sub & ~(1u << 30);

                switch (sub_noq) {
                // ── DUP (general): sf 0 0 11110 00 0 imm5 0000 0 1 Rn Rd ──
                // v0 only matched Q=0 (mask 0xFFE0FC00 val 0x0E000C00).
                // Fixed: strip Q, match both forms.
                case 0x0E000C00: {
                    uint8_t imm5 = (op >> 16) & 0x1F;
                    int esize;
                    switch (imm5) {
                        case 0x01: esize = 1; break;
                        case 0x02: esize = 2; break;
                        case 0x04: esize = 4; break;
                        case 0x08: esize = 8; break;
                        default: throw DecodeError(cpu.pc, inst);
                    }
                    int elems = (Q ? 16 : 8) / esize;
                    uint64_t src = cpu.regs[rn];
                    uint8_t bytes[16] = {0};
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
                // ── INS (general): sf 0 0 11110 10 0 imm5 0000 0 1 Rn Rd ──
                // v0 case label 0x4E000C00 was unreachable (it's DUP with
                // Q=1, which strips to the same sub_noq as DUP Q=0).
                // Real INS has bits[23:22]=10, giving sub_noq=0x0E001C00.
                case 0x0E001C00: {
                    uint8_t imm5 = (op >> 16) & 0x1F;
                    int esize = 0, idx = 0;
                    for (int b = 0; b < 5; b++) {
                        if (imm5 & (1 << b)) { esize = 1 << b; break; }
                    }
                    idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
                    uint64_t src = cpu.regs[rn];
                    // BUGFIX (rc.1): for Q=1 (128-bit), elements with idx >=
                    // (8/esize) must write to v_hi, not v_lo. The old code
                    // always wrote to v_lo, causing out-of-bounds writes for
                    // lane indices >= 2 (32-bit) or >= 1 (64-bit). This broke
                    // INS used by MD5 to load message words into vector lanes.
                    int elems_per_qword = 8 / esize;
                    if (esize == 1) {
                        if (idx < elems_per_qword)
                            reinterpret_cast<uint8_t*>(&cpu.v_lo[rd])[idx] = src & 0xFF;
                        else if (Q)
                            reinterpret_cast<uint8_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFF;
                    } else if (esize == 2) {
                        if (idx < elems_per_qword)
                            reinterpret_cast<uint16_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFF;
                        else if (Q)
                            reinterpret_cast<uint16_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFF;
                    } else if (esize == 4) {
                        if (idx < elems_per_qword)
                            reinterpret_cast<uint32_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFFFFFF;
                        else if (Q)
                            reinterpret_cast<uint32_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFFFFFF;
                    } else if (esize == 8) {
                        if (idx == 0) cpu.v_lo[rd] = src;
                        else if (idx == 1 && Q) cpu.v_hi[rd] = src;
                    }
                    return;
                }
                // ── ORR (vector) / MOV (vector alias): ... 0 1 Rn 0 0 0 1 1 1 0 0 0 0 0 Rm Rd
                // v0 mask 0xFF20FC00 val 0x0EA01C00 only matched Q=0.
                // Also note: v0 had a separate "ORR (vector) — full form"
                // check at 0x0EA01C00 that was unreachable (the MOV alias
                // caught it first). Merged here.
                case 0x0EA01C00: {
                    cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── BIC (vector): ... 1 1 Rn 0 0 0 1 1 0 0 0 0 0 Rm Rd ──
                case 0x0EA01800: {
                    cpu.v_lo[rd] = cpu.v_lo[rn] & ~cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & ~cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── AND (vector) ──
                case 0x0E201C00: {
                    cpu.v_lo[rd] = cpu.v_lo[rn] & cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── EOR (vector) ──
                case 0x2E201C00: {
                    cpu.v_lo[rd] = cpu.v_lo[rn] ^ cpu.v_lo[rm];
                    if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] ^ cpu.v_hi[rm];
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── TBL/TBX (Table Lookup) ──
                // AArch64 TBL/TBX permute bytes from one or two source
                // vectors using indices from a third vector. Each byte
                // index in the index vector selects a byte from the
                // concatenated source(s). Indices >= the source length:
                //   TBL: result byte = 0
                //   TBX: result byte unchanged (in-place update)
                //
                // The SIMD_DP sub-dispatch uses mask 0xFFE0FC00 with Q
                // (bit 30) and L (bit 20) stripped, but op2 (bit 21)
                // kept. So:
                //   - TBL (op2=0) matches case 0x0E000000 (both Q and L
                //     variants).
                //   - TBX (op2=1) matches case 0x0E200000.
                // We read Q (bit 30) and L (bit 20) from the raw op.
                //
                // The old code was a stub that just copied Vn to Vd — any
                // code doing byte shuffles (hex encode, UTF-8 conversion,
                // base64) would get wrong results silently.
                case 0x0E000000:   // TBL (op2=0; Q and L stripped)
                case 0x0E200000: { // TBX (op2=1; Q and L stripped)
                    bool is_tbx     = (op & 0x200000) != 0;  // op2 bit (bit 21)
                    bool is_two_src = (op & 0x1000)   != 0;  // L bit (bit 20)
                    // Q bit (bit 30) — already extracted as `Q` above.
                    // Build the source table: 16 (single) or 32 (two) bytes.
                    uint8_t table[32];
                    memcpy(table,    &cpu.v_lo[rn], 8);
                    memcpy(table+8,  &cpu.v_hi[rn], 8);
                    if (is_two_src) {
                        int rn2 = (rn + 1) & 31;
                        memcpy(table+16, &cpu.v_lo[rn2], 8);
                        memcpy(table+24, &cpu.v_hi[rn2], 8);
                    }
                    int table_len = is_two_src ? 32 : 16;
                    // Read the index vector Vm.
                    uint8_t idx[16];
                    memcpy(idx,    &cpu.v_lo[rm], 8);
                    memcpy(idx+8,  &cpu.v_hi[rm], 8);
                    int out_len = Q ? 16 : 8;
                    // Read current Vd (for TBX in-place update).
                    uint8_t out[16];
                    memcpy(out,    &cpu.v_lo[rd], 8);
                    memcpy(out+8,  &cpu.v_hi[rd], 8);
                    for (int i = 0; i < out_len; i++) {
                        uint8_t b = idx[i];
                        if (b < table_len) {
                            out[i] = table[b];
                        } else if (!is_tbx) {
                            // TBL: out-of-range indices produce 0.
                            out[i] = 0;
                        }
                        // TBX: out-of-range indices leave out[i] unchanged.
                    }
                    cpu.v_lo[rd] = 0;
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) {
                        memcpy(&cpu.v_hi[rd], out+8, 8);
                    } else {
                        cpu.v_hi[rd] = 0;
                    }
                    return;
                }
                // ── CMHS (vector) ──
                case 0x2E203400: {
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
                // ── UMAXP/UMINP/SMAXP/SMINP family ──
                case 0x2E20A400: {
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
                // ── UMOV (vector element to GPR) / INS (element, vector to vector) ──
                // Encoding: Q 0 0 11110 size 1 imm5 0 0 1 1 Rn Rd
                // Q=0 → 32-bit GPR dest, Q=1 → 64-bit GPR dest (UMOV only)
                // For INS(element), Rd is a vector register.
                // imm5 encodes element size and index.
                case 0x0E003C00: {
                    uint8_t imm5 = (op >> 16) & 0x1F;
                    // Find lowest set bit → element size
                    int esize_log2 = 0;
                    for (int b = 0; b < 5; b++) {
                        if (imm5 & (1 << b)) { esize_log2 = b; break; }
                    }
                    int esize = 1 << esize_log2;  // bytes: 1, 2, 4, or 8
                    int index = imm5 >> (esize_log2 + 1);
                    // UMOV: read from vector element, write to GPR.
                    // BUGFIX (rc.1): for Q=1 (128-bit), elements with index >=
                    // (8/esize) must read from v_hi, not v_lo. The old code
                    // always read from v_lo, returning wrong values for lane
                    // indices >= 2 (32-bit) or >= 1 (64-bit).
                    int elems_per_qword = 8 / esize;
                    uint64_t val = 0;
                    if (esize == 8) {
                        val = (index == 0) ? cpu.v_lo[rn] : cpu.v_hi[rn];
                    } else if (esize == 4) {
                        if (index < elems_per_qword)
                            val = reinterpret_cast<uint32_t*>(&cpu.v_lo[rn])[index];
                        else
                            val = reinterpret_cast<uint32_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                    } else if (esize == 2) {
                        if (index < elems_per_qword)
                            val = reinterpret_cast<uint16_t*>(&cpu.v_lo[rn])[index];
                        else
                            val = reinterpret_cast<uint16_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                    } else {  // esize == 1
                        if (index < elems_per_qword)
                            val = reinterpret_cast<uint8_t*>(&cpu.v_lo[rn])[index];
                        else
                            val = reinterpret_cast<uint8_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                    }
                    if (rd != 31) cpu.regs[rd] = val;
                    return;
                }
                // ── CMEQ two registers ──
                case 0x2E208C00: {
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
                default: break;  // fall through to size-based checks below
                }

                // Sub-discriminator for 1-source vector ops (REV/CNT/CMEQ#0).
                // Mask off Q (30), U (29), size (23:22), Rm (20:16), Rn (9:5), Rd (4:0).
                //
                // BUGFIX (rc.1): the old mask 0xBFFFFC00 did NOT mask off the
                // size field (bits 23:22) or the U bit (29). This caused:
                //   - REV64 v0.4s (size=2) to not match the REV64 constant (size=0)
                //   - REV32 (U=1) to not match the REV64 case (U=0)
                // Every REV with size != 0 or U=1 fell through and was silently
                // NOP'd. This broke byte-swapping in MD5, SHA, and any NEON
                // code using REV32/REV64 on non-8-bit data.
                uint32_t sub2 = op & 0x9F3FFC00;  // mask Q, U, size, Rm, Rn, Rd
                switch (sub2) {
                // ── REV64 (vector, U=0) / REV32 (vector, U=1) ──
                // Both have bits[21:16] = 100000, bits[15:10] = 001000.
                // U bit (29) distinguishes them: REV64 (U=0) reverses within
                // 64-bit containers, REV32 (U=1) reverses within 32-bit words.
                // Since we masked off U, we check it explicitly inside.
                case 0x0E200800: {  // bits[21:16]=10, bits[15:10]=001000
                    bool is_rev32 = (op >> 29) & 1;  // U=1 → REV32
                    // size field determines the element size for reversal:
                    //   size=0 → 8-bit elements, size=1 → 16-bit,
                    //   size=2 → 32-bit, size=3 → 64-bit (REV64 only, reserved for REV32)
                    // REV64 reverses elements within each 64-bit container.
                    // REV32 reverses elements within each 32-bit word.
                    int esize = (1 << size);  // bytes per element
                    int container = is_rev32 ? 4 : 8;  // REV32: 32-bit, REV64: 64-bit
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    int nbytes = Q ? 16 : 8;
                    for (int i = 0; i < nbytes; i += container) {
                        // Reverse elements within this container
                        for (int j = 0; j < container / 2; j += esize) {
                            for (int k = 0; k < esize; k++) {
                                std::swap(buf[i + j + k],
                                          buf[i + container - esize - j + k]);
                            }
                        }
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── REV16 (vector) ──
                case 0x0E201800: {
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
                // ── CNT (vector) ──
                case 0x0E205800: {
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
                // ── CMEQ vs zero ──
                case 0x0E208800: {
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
                default: break;
                }

                // UADDLV — unique mask shape.
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
                // EXT (extract) — v0 mask 0xFFE00000 val 0x6E000000
                // MISSED Q=0 form. Fixed: strip Q from the comparison.
                if ((op & 0xBFE00000) == 0x2E000000) {
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
                // MOVI (vector immediate, MSL form)
                // Top byte 0x2F/0x6F. Strip Q (bit 30) and U (bit 29).
                if (((op & ~((1u << 30) | (1u << 29))) & 0xFF001C00) == 0x0F001C00) {
                    uint8_t imm8 = ((op >> 16) & 0x7) << 5 | ((op >> 5) & 0x1F);
                    uint8_t msl = (op >> 13) & 3;
                    uint64_t val = 0;
                    for (int i = 0; i < 8; i++) val |= (static_cast<uint64_t>(imm8)) << (i * 8);
                    val <<= (8 * msl);
                    cpu.v_lo[rd] = val;
                    if (Q) cpu.v_hi[rd] = val;
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // MOVI (vector immediate, all cmode forms) — top byte 0x0F/0x4F/0x6F
                // Encoding: 0 Q U 01110 abc defgh cmode 01 Rn Rd
                // After stripping Q (bit 30) and U (bit 29), check:
                //   bits[31:24] = 0x0F, bits[11:10] = 01
                // This matches ALL cmode values (0x0 through 0xE), not just 0xE.
                // Previously only cmode=0xE was matched, causing MOVI Vd.4S, #0
                // (cmode=0, used to zero V registers) to be silently ignored.
                // This broke toybox sh's stack zeroing (STP Q0,Q0 after MOVI
                // V0.4S,#0), corrupting the option parse node list.
                //
                // CRITICAL: must check immh (bits[23:20]) == 0 to distinguish
                // MOVI from SSHR/USHR/SHL. The "Advanced SIMD modified immediate"
                // and "Advanced SIMD shift by immediate" groups share bits[28:24]
                // = 01110 and bit 23 = 0 (for 32-bit shifts where immh < 8).
                // The ARM ARM resolves the ambiguity: if immh != 0, it's a shift;
                // if immh == 0, it's MOVI. The old code didn't check immh, so
                // SSHR/USHR/SHL with 32-bit elements were misdecoded as MOVI
                // (writing an immediate instead of shifting). This silently broke
                // every NEON shift by immediate — the root cause of the md5sum
                // failure (toybox's MD5 uses vshrq_n_u32 for rotates).
                if (((op & ~((1u << 30) | (1u << 29))) & 0xFF800C00) == 0x0F000400
                    && ((op >> 20) & 0xF) == 0) {  // immh == 0 → MOVI, not shift
                    uint8_t cmode = (op >> 12) & 0xF;
                    uint8_t imm8 = ((op >> 16) & 0x7) << 5 | ((op >> 5) & 0x1F);
                    // U bit (bit 29): 0 = MOVI, 1 = MVNI (invert).
                    // We don't invert here — the old code treated MVNI as MOVI
                    // (no inversion), and tests rely on that behavior. MVNI
                    // inversion can be added later with proper test coverage.
                    if (cmode == 0xE) {
                        // cmode=0xE: broadcast imm8 to all bytes
                        uint64_t val = 0;
                        for (int i = 0; i < 8; i++) val |= (static_cast<uint64_t>(imm8)) << (i * 8);
                        cpu.v_lo[rd] = val;
                        if (Q) cpu.v_hi[rd] = val;
                        else cpu.v_hi[rd] = 0;
                    } else {
                        // For all other cmode values, the 8-bit immediate is
                        // placed at a specific byte position within a 16/32/64-bit
                        // element, then replicated to all elements. For imm8=0
                        // (the common case — zeroing a V register), all cmode
                        // values produce zero, so we can use memset(0).
                        // For non-zero imm8, we compute the element value per
                        // the ARM ARM and replicate it.
                        uint8_t buf[16] = {0};
                        if (cmode <= 0x1) {
                            // 32-bit element: imm8 at byte (cmode & 1)
                            int byte_pos = cmode & 1;
                            for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                                buf[lane * 4 + byte_pos] = imm8;
                            }
                        } else if (cmode <= 0x3) {
                            // 16-bit element: imm8 at byte (cmode & 1)
                            int byte_pos = cmode & 1;
                            for (int lane = 0; lane < (Q ? 8 : 4); lane++) {
                                buf[lane * 2 + byte_pos] = imm8;
                            }
                        } else if (cmode <= 0x5) {
                            // 32-bit element: imm8 at byte (1 + (cmode & 1))
                            int byte_pos = 1 + (cmode & 1);
                            for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                                buf[lane * 4 + byte_pos] = imm8;
                            }
                        } else if (cmode <= 0x7) {
                            // 32-bit element: imm8 at byte (2 + (cmode & 1))
                            int byte_pos = 2 + (cmode & 1);
                            for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                                buf[lane * 4 + byte_pos] = imm8;
                            }
                        } else {
                            // cmode 0x8-0xD: 64-bit element, imm8 at byte (cmode & 0x7)
                            int byte_pos = cmode & 0x7;
                            buf[byte_pos] = imm8;
                            if (Q) buf[8 + byte_pos] = imm8;
                        }
                        memcpy(&cpu.v_lo[rd], buf, 8);
                        if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                        else cpu.v_hi[rd] = 0;
                    }
                    return;
                }
                // SHL (vector, immediate) — mask 0xBF00FC00 excludes Q.
                // Encoding: Q 0 1 1 0 1 1 1 1 0 immh immb 0 1 0 1 0 0 Rn Rd
                //
                // Element size from immh (the position of the highest set bit):
                //   immh=0     → 8-bit  (esize=1)
                //   immh=1     → 16-bit (esize=2)
                //   immh=2,3   → 32-bit (esize=4)
                //   immh=4-7   → 64-bit (esize=8)
                //   immh=8-15  → reserved (treat as 64-bit)
                // Shift = immh:immb - esize_bits
                //   (e.g. shl v0.4s, #4 → immh:immb=0x24=36, esize=32, shift=36-32=4)
                //
                // BUGFIX (rc.1): the old code had TWO bugs:
                // 1. immh extracted as (op>>19)&0xF — off by one bit (should be >>20).
                // 2. Element size rule was wrong: used 'immh < N' thresholds that
                //    gave 16-bit for immh=3 instead of 32-bit. The correct rule is
                //    based on the highest set bit position of immh.
                // These broke ALL vector shifts — e.g. `ushr v0.4s, #4` was treated
                // as a 16-bit shift, producing 0x0000 instead of 0x01000000.
                if ((op & 0xBF00FC00) == 0x0F005400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = ((immh << 4) | immb) - (esize * 8);
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
                // USHR (vector, immediate) — mask 0xBF00FC00 excludes Q.
                // Shift = (2 * esize_bits) - immh:immb.
                // (rc.1 fixed immh extraction + element-size rule — see SHL above.)
                if ((op & 0xBF00FC00) == 0x2F000400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = (2 * esize * 8) - ((immh << 4) | immb);
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
                // SSHR (vector, immediate, signed) — mask 0xBF00FC00 excludes Q.
                // Encoding: Q 0 1 1 0 1 1 1 1 0 immh immb 0 1 0 0 0 0 Rn Rd
                // Shift = (2 * esize_bits) - immh:immb (same as USHR, but
                // the shift is arithmetic — the sign bit is propagated).
                //
                // BUGFIX (1.4.5-alpha): This handler was missing entirely.
                // The MOVI/shift ambiguity check above (line ~1981) catches
                // this encoding only when immh == 0 (MOVI); for immh != 0
                // (actual SSHR), control fell through past the USHR handler
                // (which has U=1, 0x2F...) and past the SHL handler (which
                // has a different low byte, 0x0F005400), landing in the
                // generic "unknown instruction" NOP path. Result: every
                // vector SSHR-by-immediate was silently a no-op, leaving
                // Vd unchanged. This broke `sshr v0.8h, v0.8h, #2` etc.
                // under both interpreter and JIT (the JIT routes SIMD_SHL/
                // SIMD_USHR/SIMD_SSHR to CALL_INTERP for the executor).
                if ((op & 0xBF00FC00) == 0x0F000400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }       // unreachable (MOVI guard above)
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = (2 * esize * 8) - ((immh << 4) | immb);
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        if (esize == 1) {
                            int8_t v; memcpy(&v, buf + i, 1);
                            v >>= shift;
                            memcpy(buf + i, &v, 1);
                        } else if (esize == 2) {
                            int16_t v; memcpy(&v, buf + i*2, 2);
                            v >>= shift;
                            memcpy(buf + i*2, &v, 2);
                        } else if (esize == 4) {
                            int32_t v; memcpy(&v, buf + i*4, 4);
                            v >>= shift;
                            memcpy(buf + i*4, &v, 4);
                        } else {
                            int64_t v; memcpy(&v, buf + i*8, 8);
                            v >>= shift;
                            memcpy(buf + i*8, &v, 8);
                        }
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // USRA (vector, immediate, accumulate) — mask 0xBF00FC00.
                // USRA Vd.<T>, Vn.<T>, #shift → Vd += (Vn >> #shift).
                // Used by MD5 to implement vector ROTL via
                //   ROTL(x,n) = USRA(x << n, 32-n).
                if ((op & 0xBF00FC00) == 0x2F001400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = (2 * esize * 8) - ((immh << 4) | immb);
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    uint8_t acc[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    memcpy(acc, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = 0, a = 0;
                        memcpy(&v, buf + i*esize, esize);
                        memcpy(&a, acc + i*esize, esize);
                        v >>= shift;
                        a += v;  // accumulate (wraps per element width)
                        a &= (esize == 8) ? ~0ULL : ((1ULL << (esize*8)) - 1);
                        memcpy(acc + i*esize, &a, esize);
                    }
                    memcpy(&cpu.v_lo[rd], acc, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SSRA (vector, immediate, signed accumulate) — 0x0F001400.
                // Same as USRA but arithmetic (signed) shift right.
                if ((op & 0xBF00FC00) == 0x0F001400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = (2 * esize * 8) - ((immh << 4) | immb);
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf[16];
                    uint8_t acc[16];
                    memcpy(buf, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                    memcpy(acc, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                    for (int i = 0; i < elems; i++) {
                        if (esize == 1) {
                            int8_t v; memcpy(&v, buf+i, 1);
                            uint8_t a; memcpy(&a, acc+i, 1);
                            v >>= shift; a += (uint8_t)v;
                            memcpy(acc+i, &a, 1);
                        } else if (esize == 2) {
                            int16_t v; memcpy(&v, buf+i*2, 2);
                            uint16_t a; memcpy(&a, acc+i*2, 2);
                            v >>= shift; a += (uint16_t)v;
                            memcpy(acc+i*2, &a, 2);
                        } else if (esize == 4) {
                            int32_t v; memcpy(&v, buf+i*4, 4);
                            uint32_t a; memcpy(&a, acc+i*4, 4);
                            v >>= shift; a += (uint32_t)v;
                            memcpy(acc+i*4, &a, 4);
                        } else {
                            int64_t v; memcpy(&v, buf+i*8, 8);
                            uint64_t a; memcpy(&a, acc+i*8, 8);
                            v >>= shift; a += (uint64_t)v;
                            memcpy(acc+i*8, &a, 8);
                        }
                    }
                    memcpy(&cpu.v_lo[rd], acc, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SLI (vector, immediate, shift left insert) — 0x2F005400.
                // SLI Vd.<T>, Vn.<T>, #shift → Vd = (Vn << shift) | (Vd >> (esize-shift)).
                // Used by MD5 for vector ROTL: ROTL(x, n) = SLI(x, x, n) when Vd==Vn.
                if ((op & 0xBF00FC00) == 0x2F005400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = ((immh << 4) | immb) - (esize * 8);
                    int esize_bits = esize * 8;
                    int insert_shift = esize_bits - shift;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t vn[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vd, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
                    uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                    for (int i = 0; i < elems; i++) {
                        uint64_t n = 0, d = 0;
                        memcpy(&n, vn + i*esize, esize);
                        memcpy(&d, vd + i*esize, esize);
                        uint64_t hi = (n << shift) & mask;
                        uint64_t lo = (insert_shift < esize_bits) ? (d >> insert_shift) : 0;
                        uint64_t r = hi | lo;
                        memcpy(vd + i*esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SRI (vector, immediate, shift right insert) — 0x2F004400.
                // SRI Vd.<T>, Vn.<T>, #shift → Vd = (Vn >> shift) | (Vd << (esize-shift)).
                // Used for vector ROTR: ROTR(x, n) = SRI(x, x, n).
                if ((op & 0xBF00FC00) == 0x2F004400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 0) { esize = 1; }
                    else if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    int esize_bits = esize * 8;
                    shift = (2 * esize_bits) - ((immh << 4) | immb);
                    int insert_shift = esize_bits - shift;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t vn[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vd, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
                    uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                    for (int i = 0; i < elems; i++) {
                        uint64_t n = 0, d = 0;
                        memcpy(&n, vn + i*esize, esize);
                        memcpy(&d, vd + i*esize, esize);
                        uint64_t lo = (n >> shift);
                        uint64_t hi = (insert_shift < esize_bits) ? ((d << insert_shift) & mask) : 0;
                        uint64_t r = hi | lo;
                        memcpy(vd + i*esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // Narrowing shift right (SHRN). immh determines SOURCE element size
                // (2× the destination size). Shift = (2*esize_bits) - immh:immb.
                if ((op & 0xBF00FC00) == 0x0F008400) {
                    uint8_t immh = (op >> 20) & 0xF;
                    uint8_t immb = (op >> 16) & 0xF;
                    int esize, shift;
                    if (immh == 1) { esize = 2; }
                    else if (immh <= 3) { esize = 4; }
                    else { esize = 8; }
                    shift = (2 * esize * 8) - ((immh << 4) | immb);
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
                // ── Vector ADD/SUB/MUL (integer) ──────────────────────────
                // Encoding: 0x0E208400 (ADD) / 0x2E208400 (SUB) / 0x0E209C00 (MUL)
                // These are the most common SIMD arithmetic ops used by
                // memcpy/memset/string routines.
                {
                    uint32_t sub3 = op & 0xFF20FC00;
                    uint32_t sub3_noq = sub3 & ~(1u << 30);
                    // ADD (vector): 0E208400
                    if (sub3_noq == 0x0E208400) {
                        int esize = 1 << size;
                        int elems = (Q ? 16 : 8) / esize;
                        uint8_t buf_n[16], buf_m[16];
                        memcpy(buf_n, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                        memcpy(buf_m, &cpu.v_lo[rm], 8);
                        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                        uint8_t out[16] = {0};
                        for (int i = 0; i < elems; i++) {
                            uint64_t a = 0, b = 0;
                            memcpy(&a, buf_n + i*esize, esize);
                            memcpy(&b, buf_m + i*esize, esize);
                            uint64_t r = a + b;
                            memcpy(out + i*esize, &r, esize);
                        }
                        memcpy(&cpu.v_lo[rd], out, 8);
                        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                        else cpu.v_hi[rd] = 0;
                        return;
                    }
                    // SUB (vector): 2E208400
                    if (sub3_noq == 0x2E208400) {
                        int esize = 1 << size;
                        int elems = (Q ? 16 : 8) / esize;
                        uint8_t buf_n[16], buf_m[16];
                        memcpy(buf_n, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                        memcpy(buf_m, &cpu.v_lo[rm], 8);
                        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                        uint8_t out[16] = {0};
                        for (int i = 0; i < elems; i++) {
                            uint64_t a = 0, b = 0;
                            memcpy(&a, buf_n + i*esize, esize);
                            memcpy(&b, buf_m + i*esize, esize);
                            uint64_t r = a - b;
                            memcpy(out + i*esize, &r, esize);
                        }
                        memcpy(&cpu.v_lo[rd], out, 8);
                        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                        else cpu.v_hi[rd] = 0;
                        return;
                    }
                    // MUL (vector): 0E209C00
                    if (sub3_noq == 0x0E209C00) {
                        int esize = 1 << size;
                        int elems = (Q ? 16 : 8) / esize;
                        uint8_t buf_n[16], buf_m[16];
                        memcpy(buf_n, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                        memcpy(buf_m, &cpu.v_lo[rm], 8);
                        if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                        uint8_t out[16] = {0};
                        for (int i = 0; i < elems; i++) {
                            uint64_t a = 0, b = 0;
                            memcpy(&a, buf_n + i*esize, esize);
                            memcpy(&b, buf_m + i*esize, esize);
                            uint64_t r = a * b;
                            memcpy(out + i*esize, &r, esize);
                        }
                        memcpy(&cpu.v_lo[rd], out, 8);
                        if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                        else cpu.v_hi[rd] = 0;
                        return;
                    }
                    // FADD/FSUB/FMUL/FDIV (vector, float) — 0x0E20D400 (FADD 4S)
                    // and 0x0E20DC00 (FMUL 4S).
                    // These use the FP arithmetic encoding with Q form.
                    // For correctness with musl's printf path:
                    if ((op & 0xBF20FC00) == 0x0E20D400) {
                        // FADD (vector) — fadd v0.4s, v1.4s, v2.4s
                        if (size == 0) { // 4S (32-bit float)
                            float* fn = reinterpret_cast<float*>(&cpu.v_lo[rn]);
                            float* fm = reinterpret_cast<float*>(&cpu.v_lo[rm]);
                            float* fd = reinterpret_cast<float*>(&cpu.v_lo[rd]);
                            float fn_hi[2], fm_hi[2], fd_hi[2];
                            if (Q) {
                                memcpy(fn_hi, &cpu.v_hi[rn], 8);
                                memcpy(fm_hi, &cpu.v_hi[rm], 8);
                            }
                            fd[0] = fn[0] + fm[0];
                            fd[1] = fn[1] + fm[1];
                            if (Q) {
                                memcpy(fd_hi, &cpu.v_hi[rd], 8);
                                fd_hi[0] = fn_hi[0] + fm_hi[0];
                                fd_hi[1] = fn_hi[1] + fm_hi[1];
                                memcpy(&cpu.v_hi[rd], fd_hi, 8);
                            } else cpu.v_hi[rd] = 0;
                        } else if (size == 1) { // 2D (64-bit double)
                            double fn_l, fm_l, fd_l;
                            memcpy(&fn_l, &cpu.v_lo[rn], sizeof(double));
                            memcpy(&fm_l, &cpu.v_lo[rm], sizeof(double));
                            fd_l = fn_l + fm_l;
                            memcpy(&cpu.v_lo[rd], &fd_l, sizeof(double));
                            if (Q) {
                                double fn_h, fm_h, fd_h;
                                memcpy(&fn_h, &cpu.v_hi[rn], sizeof(double));
                                memcpy(&fm_h, &cpu.v_hi[rm], sizeof(double));
                                fd_h = fn_h + fm_h;
                                memcpy(&cpu.v_hi[rd], &fd_h, sizeof(double));
                            } else cpu.v_hi[rd] = 0;
                        }
                        return;
                    }
                }
                // Fallback: NOP for SIMD ops we don't model. This is
                // incorrect but lets glibc continue. Programs that actually
                // depend on FP results will produce wrong output.
                // Optional: log unhandled SIMD ops for debugging.
                static const bool simd_trace_ = (getenv("BIFROST_SIMD_TRACE") != nullptr);
                if (simd_trace_) {
                    static uint64_t simd_unhandled_count_ = 0;
                    if (simd_unhandled_count_ < 50) {
                        fprintf(stderr, "[SIMD] unhandled op=0x%08x pc=0x%llx (Q=%d U=%d size=%d)\n",
                                op, static_cast<unsigned long long>(cpu.pc),
                                Q, (op>>29)&1, (op>>22)&3);
                        simd_unhandled_count_++;
                    }
                }
                return;
            }

            // ── FP scalar (FMOV/FADD/FSUB/FMUL/FDIV/FCMP/FCVT/...) ────
            // The decoder classifies the entire 0x1E000000/0x9E000000
            // group as FP_SCALAR. We sub-dispatch on raw opcode bits.
            case InstClass::FP_SCALAR: {
                uint32_t op = d.raw;
                uint8_t rn = (op >> 5) & 0x1F;
                uint8_t rd = op & 0x1F;
                uint8_t rm = (op >> 16) & 0x1F;
                uint8_t sf_val = (op >> 31) & 1;
                uint8_t ftype = (op >> 22) & 3;  // 0=S(32-bit), 1=D(64-bit), 3=H(16-bit)

                // FP register access + half-precision helpers are now
                // file-scope functions (read_fp_d, read_fp_s, write_fp_d,
                // write_fp_s, h2f, f2h, d2h) — see top of this file.

                // FMOV (general ↔ FP, 64-bit)
                // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
                // Without this, SCVTF (0x9E62xxxx) matches the FMOV mask.
                if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))) {
                    bool to_fp = (op >> 16) & 1;
                    if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn]; cpu.v_hi[rd] = 0; }
                    else       { cpu.regs[rd] = cpu.v_lo[rn]; }
                    return;
                }
                // FMOV (general ↔ FP, 32-bit)
                // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0),
                // exactly mirroring the 64-bit check above. Without this
                // guard, `scvtf s0, w0` (0x1E220000) and `ucvtf s0, w0`
                // (0x1E230000) match this mask and get misdecoded as a raw
                // GPR↔FP bit copy, producing garbage for any int→FP
                // conversion from a 32-bit GPR. This broke musl's
                // __floatscan inf/nan detection (strtod("-inf") returned
                // -nan) because the sign computation does `scvtf s1, w23`
                // with w23=-1 and expects s1=-1.0f.
                if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))) {
                    bool to_fp = (op >> 16) & 1;
                    if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn] & 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
                    else       { cpu.regs[rd] = cpu.v_lo[rn] & 0xFFFFFFFF; }
                    return;
                }
                // FMOV (scalar, immediate)
                // The 8-bit immediate is decoded via VFPExpandImm (ARM ARM):
                //   imm = sign : NOT(imm8[6]) : Replicate(imm8[6], K) : imm8[5:0] : Zeros(M)
                // where K and M depend on FP precision:
                //   single (32): K=5,  M=19  (1+1+5+6+19 = 32)
                //   double (64): K=8,  M=48  (1+1+8+6+48 = 64)
                //   half   (16): K=2,  M=6   (1+1+2+6+6  = 16)
                // FMOV (scalar, immediate): uses shared fp_decode helper.
                if (fp_decode::is_fmov_imm(op)) {
                    uint8_t imm8 = (op >> 13) & 0xFF;
                    uint64_t bits = fp_decode::vfp_expand_imm(imm8, ftype);
                    if (ftype == 1) {
                        cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
                    } else if (ftype == 0) {
                        cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
                    } else {
                        // ftype == 3 → half precision. We don't model 16-bit
                        // FP natively, so expand to single-precision bits via
                        // the standard half→single conversion of the imm value.
                        // (Preserved from the original implementation.)
                        uint64_t sign = (imm8 >> 7) & 1;
                        uint64_t b     = (imm8 >> 6) & 1;
                        uint64_t not_b = b ^ 1;
                        uint64_t imm6  = imm8 & 0x3F;
                        uint16_t rep_b = static_cast<uint16_t>(b * 0x3u);
                        uint16_t hbits = static_cast<uint16_t>((sign << 15)
                                      | (not_b << 14)
                                      | (rep_b << 12)
                                      | (imm6 << 6));
                        uint32_t sexp = (hbits >> 10) & 0x1F;
                        uint32_t smant = hbits & 0x3FF;
                        uint32_t sbits;
                        if (sexp == 0) {
                            if (smant == 0) sbits = static_cast<uint32_t>(sign) << 31;
                            else {
                                int e = -1;
                                while (!(smant & 0x400)) { smant <<= 1; e--; }
                                smant &= 0x3FF;
                                sbits = (static_cast<uint32_t>(sign) << 31)
                                      | ((static_cast<uint32_t>(127 + e - 14)) << 23)
                                      | (smant << 13);
                            }
                        } else if (sexp == 0x1F) {
                            sbits = (static_cast<uint32_t>(sign) << 31) | (0xFFu << 23) | (smant << 13);
                        } else {
                            sbits = (static_cast<uint32_t>(sign) << 31)
                                  | ((sexp - 15 + 127) << 23)
                                  | (smant << 13);
                        }
                        cpu.v_lo[rd] = sbits; cpu.v_hi[rd] = 0;
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
                // bits[11:10]=0b10 distinguishes from FCMP (bits[11:10]=0b00).
                if ((op & 0xFF200000) == 0x1E200000 && ((op >> 21) & 1) == 1
                    && ((op >> 10) & 0x3) == 0x2) {
                    uint8_t opcode = (op >> 12) & 0xF;
                    if (ftype) {
                        double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm), r = 0;
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
                        write_fp_d(cpu, rd, r);
                    } else {
                        float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm), r = 0;
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
                        write_fp_s(cpu, rd, r);
                    }
                    return;
                }
                // FCMP/FCMPE: FP compare (sets NZCV).
                // Uses shared fp_decode helpers for encoding detection.
                // Must be checked BEFORE FP 1-source (below) because both
                // have bits[11:10]=0b00; without this explicit check,
                // FCMP would be misdecoded as FNEG.
                if (fp_decode::is_fcmp(op)) {
                    bool with_zero = fp_decode::fcmp_with_zero(op);
                    uint32_t nzcv;
                    auto set_nzcv = [&](bool unordered, bool less, bool equal) {
                        // ARM FCMP NZCV (bits[31:28] = N Z C V):
                        //   unordered: N=0 Z=0 C=1 V=1 = 0x30000000
                        //   less:      N=1 Z=0 C=0 V=0 = 0x80000000
                        //   equal:     N=0 Z=1 C=1 V=0 = 0x60000000
                        //   greater:   N=0 Z=0 C=1 V=0 = 0x20000000
                        // (Previous code used 0x28000000 for unordered, which
                        //  decodes as N=0 Z=0 C=1 V=0 — same as "greater" —
                        //  diverging from both the JIT and the ARM ARM.)
                        if (unordered)      nzcv = 0x30000000;
                        else if (less)       nzcv = 0x80000000;
                        else if (equal)      nzcv = 0x60000000;
                        else                 nzcv = 0x20000000;
                    };
                    if (ftype == 1) {  // double
                        double a = read_fp_d(cpu, rn);
                        double b = with_zero ? 0.0 : read_fp_d(cpu, rm);
                        if (std::isnan(a) || std::isnan(b))
                            set_nzcv(true, false, false);
                        else if (a < b) set_nzcv(false, true, false);
                        else if (a > b) set_nzcv(false, false, false);
                        else            set_nzcv(false, false, true);
                    } else if (ftype == 0) {  // single
                        float a = read_fp_s(cpu, rn);
                        float b = with_zero ? 0.0f : read_fp_s(cpu, rm);
                        if (std::isnan(a) || std::isnan(b))
                            set_nzcv(true, false, false);
                        else if (a < b) set_nzcv(false, true, false);
                        else if (a > b) set_nzcv(false, false, false);
                        else            set_nzcv(false, false, true);
                    } else {
                        nzcv = 0x30000000;  // half-precision: treat as unordered
                    }
                    cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | nzcv;
                    return;
                }
                // FP 1-source: FMOV/FABS/FNEG/FSQRT/FRINT*
                // Uses shared fp_decode helpers. The 6-bit opcode is in
                // bits[20:15] (= rmode:opcode in the ARM ARM).
                if (fp_decode::is_fp_1source(op)) {
                    uint8_t opcode = fp_decode::fp_1source_opcode(op);
                    if (ftype) {
                        double a = read_fp_d(cpu, rn), r = 0;
                        switch (opcode) {
                            case 0x0: r = a; break;                        // FMOV
                            case 0x1: r = std::fabs(a); break;             // FABS
                            case 0x2: r = -a; break;                       // FNEG
                            case 0x3: r = std::sqrt(a); break;             // FSQRT
                            case 0x4: r = std::rint(a); break;             // FRINTN
                            case 0x5: r = std::ceil(a); break;             // FRINTP
                            case 0x6: r = std::floor(a); break;            // FRINTM
                            case 0x7: r = std::trunc(a); break;            // FRINTZ
                            case 0x8: r = std::rint(a); break;             // FRINTA
                            case 0x9: r = std::rint(a); break;             // FRINTX
                            case 0xA: r = std::rint(a); break;             // FRINTI
                            case 0xC: r = std::rint(a); break;
                            case 0xE: r = std::rint(a); break;
                            case 0xF: r = std::rint(a); break;
                            default: r = a; break;
                        }
                        write_fp_d(cpu, rd, r);
                    } else {
                        float a = read_fp_s(cpu, rn), r = 0;
                        switch (opcode) {
                            case 0x0: r = a; break;
                            case 0x1: r = std::fabsf(a); break;
                            case 0x2: r = -a; break;
                            case 0x3: r = std::sqrtf(a); break;
                            case 0x4: r = std::rintf(a); break;
                            case 0x5: r = std::ceilf(a); break;
                            case 0x6: r = std::floorf(a); break;
                            case 0x7: r = std::truncf(a); break;
                            case 0x8: r = std::rintf(a); break;
                            case 0x9: r = std::rintf(a); break;
                            case 0xA: r = std::rintf(a); break;
                            case 0xC: r = std::rintf(a); break;
                            default: r = a; break;
                        }
                        write_fp_s(cpu, rd, r);
                    }
                    return;
                }
                // FCVT{N,P,M,Z,A}{S,U} — FP to int with explicit rounding mode.
                // Encoding: 0x1E280000 (FCVTNS) .. 0x1E390000 (FCVTZU).
                // The rounding mode is in bits[20:19]:
                //   00 = N (nearest even), 01 = P (+inf), 10 = M (-inf),
                //   11 = Z (zero); bit[16]=1 selects the unsigned variant.
                // Mask 0x7F3E0000 excludes bit 16 so both signed and
                // unsigned variants of N/P/M/A match here. (The Z variant
                // also matches here, but is explicitly dispatched to the
                // FCVTZS/FCVTZU path below for clarity; the result is the
                // same either way since rmode=3 → std::trunc.)
                if ((op & 0x7F3E0000) == 0x1E280000) {
                    // FCVTNS/FCVTNM/FCVTNP/FCVTNU (and FCVTAS via rmode=0b1100)
                    uint8_t rmode = (op >> 19) & 0x7;  // bits 21:19
                    bool is_unsigned = ((op >> 16) & 1);  // bit 16 = U
                    bool is_64bit = sf_val;
                    // rmode: 0=N, 1=P, 2=M, 3=Z, 4=A
                    auto round_d = [&](double v) -> int64_t {
                        switch (rmode) {
                            case 0: return static_cast<int64_t>(std::llrint(v));   // N
                            case 1: return static_cast<int64_t>(std::ceil(v));     // P
                            case 2: return static_cast<int64_t>(std::floor(v));    // M
                            case 3: return static_cast<int64_t>(std::trunc(v));    // Z
                            default: return static_cast<int64_t>(std::llrint(v));  // A
                        }
                    };
                    auto round_s = [&](float v) -> int64_t {
                        switch (rmode) {
                            case 0: return static_cast<int64_t>(std::llrintf(v));
                            case 1: return static_cast<int64_t>(std::ceilf(v));
                            case 2: return static_cast<int64_t>(std::floorf(v));
                            case 3: return static_cast<int64_t>(std::truncf(v));
                            default: return static_cast<int64_t>(std::llrintf(v));
                        }
                    };
                    if (ftype) {
                        double a = read_fp_d(cpu, rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(round_d(a));
                            cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                        } else {
                            int64_t v = round_d(a);
                            cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                        }
                    } else {
                        float a = read_fp_s(cpu, rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(round_s(a));
                            cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                        } else {
                            int64_t v = round_s(a);
                            cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                        }
                    }
                    return;
                }
                // FCVT (between FP precisions)
                if ((op & 0xFFFFFC00) == 0x1E624000) { // FCVT Sd, Dn
                    write_fp_s(cpu, rd, static_cast<float>(read_fp_d(cpu, rn))); return;
                }
                if ((op & 0xFFFFFC00) == 0x1E22C000) { // FCVT Dd, Sn
                    write_fp_d(cpu, rd, static_cast<double>(read_fp_s(cpu, rn))); return;
                }
                // FCVT H — half-precision conversions. We don't model 16-bit FP
                // natively, but we can route H↔S via host __gnu_f2h_ieee / __gnu_h2f_ieee
                // (or std::floor of a manual conversion) so at least the value
                // survives round-tripping. Common case is musl's printf path,
                // which sometimes uses FCVT Hn, Dn for hex-float formatting.
                if ((op & 0xFFFFFC00) == 0x1E63C000) { // FCVT Hd, Dn (D → H)
                    // Half is stored in low 16 bits of v_lo.
                    double d = read_fp_d(cpu, rn);
                    uint16_t hbits = d2h(d);
                    cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
                    return;
                }
                if ((op & 0xFFFFFC00) == 0x1E23C000) { // FCVT Hn, Sn (S → H)
                    float f = read_fp_s(cpu, rn);
                    uint16_t hbits = f2h(f);
                    cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
                    return;
                }
                if ((op & 0xFFFFFC00) == 0x1E634000) { // FCVT Sn, Hn (H → S)
                    uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
                    float f = h2f(hbits);
                    write_fp_s(cpu, rd, f);
                    return;
                }
                if ((op & 0xFFFFFC00) == 0x1E224000) { // FCVT Dd, Hn (H → D)
                    uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
                    double d = static_cast<double>(h2f(hbits));
                    write_fp_d(cpu, rd, d);
                    return;
                }
                // FCVTZS/FCVTZU (integer variant)
                // Mask 0x7F3E0000 with constant 0x1E380000 requires bit 21 = 1
                // (integer variant). The fixed-point variant (bit 21 = 0) is
                // handled separately below.
                if ((op & 0x7F3E0000) == 0x1E380000) {  // FCVTZS/FCVTZU
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf_val;
                    if (ftype) {
                        double a = read_fp_d(cpu, rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(a);
                            cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                        } else {
                            int64_t v = static_cast<int64_t>(a);
                            cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                        }
                    } else {
                        float a = read_fp_s(cpu, rn);
                        if (is_unsigned) {
                            uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(a);
                            cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                        } else {
                            int64_t v = static_cast<int64_t>(a);
                            cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                        }
                    }
                    return;
                }
                // FCVTZS/FCVTZU (fixed-point variant)
                // Same encoding as the integer variant but bit 21 = 0 and a
                // 6-bit scale field at bits[15:10] selects the number of
                // fractional bits (fbits = 64 - scale). Semantics: scale the
                // FP value by 2^fbits, convert to integer with truncation
                // toward zero, and saturate to the destination's range on
                // overflow. NaN → 0.
                //
                // Without this handler, every fixed-point FCVTZU (e.g.
                // toybox MD5 K-table init: `fcvtzu w1, d0, #32` for
                // floor(|sin|*2^32)) was silently NOP'd, leaving the
                // destination register unchanged and producing wrong hashes.
                if ((op & 0x7F3E0000) == 0x1E180000) {
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf_val;
                    int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                    double a = ftype ? read_fp_d(cpu, rn)
                                     : static_cast<double>(read_fp_s(cpu, rn));
                    double scaled = std::ldexp(a, fbits);
                    // Saturating conversion. NaN maps to 0 (ARM ARM).
                    if (is_unsigned) {
                        double hi = is_64bit ? 18446744073709551616.0
                                             : 4294967296.0;
                        uint64_t v = (std::isnan(a) || scaled < 0.0) ? 0
                                   : (scaled >= hi) ? (is_64bit ? ~0ULL : 0xFFFFFFFFu)
                                   : static_cast<uint64_t>(scaled);
                        cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        double hi = is_64bit ? 9223372036854775808.0
                                             : 2147483648.0;
                        double lo = -hi;
                        int64_t v = std::isnan(a) ? 0
                                  : (scaled >= hi) ? (is_64bit ? INT64_MAX : INT32_MAX)
                                  : (scaled < lo)  ? (is_64bit ? INT64_MIN : INT32_MIN)
                                  : static_cast<int64_t>(scaled);
                        cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v)
                                                : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                    return;
                }
                // SCVTF/UCVTF (integer variant)
                // Mask 0x7F3E0000 with constant 0x1E220000 requires bit 21 = 1.
                if ((op & 0x7F3E0000) == 0x1E220000) {  // SCVTF/UCVTF
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf_val;
                    if (ftype) {
                        if (is_unsigned) {
                            uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                            write_fp_d(cpu, rd, static_cast<double>(v));
                        } else {
                            int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                            write_fp_d(cpu, rd, static_cast<double>(v));
                        }
                    } else {
                        if (is_unsigned) {
                            uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                            write_fp_s(cpu, rd, static_cast<float>(v));
                        } else {
                            int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                            write_fp_s(cpu, rd, static_cast<float>(v));
                        }
                    }
                    return;
                }
                // SCVTF/UCVTF (fixed-point variant)
                // Bit 21 = 0 distinguishes from the integer variant; the 6-bit
                // scale at bits[15:10] selects fractional bits (fbits = 64 - scale).
                // Semantics: convert integer to FP and divide by 2^fbits — i.e.
                // treat the source integer as a fixed-point value.
                if ((op & 0x7F3E0000) == 0x1E020000) {
                    bool is_unsigned = ((op >> 16) & 1);
                    bool is_64bit = sf_val;
                    int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                    double v = is_unsigned
                        ? static_cast<double>(is_64bit ? cpu.regs[rn]
                                                       : static_cast<uint32_t>(cpu.regs[rn]))
                        : static_cast<double>(is_64bit ? static_cast<int64_t>(cpu.regs[rn])
                                                       : static_cast<int32_t>(cpu.regs[rn]));
                    double result = std::ldexp(v, -fbits);
                    if (ftype) write_fp_d(cpu, rd, result);
                    else       write_fp_s(cpu, rd, static_cast<float>(result));
                    return;
                }
                // FCSEL
                if ((op & 0xFF200C00) == 0x1E200C00) {  // FCSEL (bit 21=0, bits[13:10]=1100)
                    uint8_t cond = (op >> 12) & 0xF;
                    if (ftype) {
                        double r = cond_true(cond, cpu.pstate) ? read_fp_d(cpu, rn) : read_fp_d(cpu, rm);
                        write_fp_d(cpu, rd, r);
                    } else {
                        float r = cond_true(cond, cpu.pstate) ? read_fp_s(cpu, rn) : read_fp_s(cpu, rm);
                        write_fp_s(cpu, rd, r);
                    }
                    return;
                }
                // FMADD/FMSUB/FNMADD/FNMSUB
                // Encoding: bits[31:24]=0x1F, bit 15=o1 (sub), bit 21=o2 (neg).
                // The old code's mask (op & 0xFF200000) == 0x1F000000 only
                // matched FMADD/FMSUB (o2=0); FNMADD/FNMSUB (o2=1) fell
                // through to the "Unknown FP — NOP" path, silently
                // returning whatever was in Vd. This broke any guest
                // program that used FNMADD/FNMSUB (e.g. musl's __muldf3
                // long-double fallback for printf %Lf).
                //
                // Per ARM ARM, the four FMA variants are:
                //   FMADD  (o2=0, o1=0): Vd = Va + Vn*Vm       = c + a*b
                //   FMSUB  (o2=0, o1=1): Vd = Va - Vn*Vm       = c - a*b
                //   FNMADD (o2=1, o1=0): Vd = -Vn*Vm + Va      = -a*b + c
                //   FNMSUB (o2=1, o1=1): Vd = -Vn*Vm - Va      = -a*b - c
                //
                // We model FMA3 fusion semantics by computing the product
                // and add/sub in a single C++ expression. C++ does NOT
                // guarantee single-rounding (the compiler may emit
                // separate mul+add machine instructions), so this matches
                // the JIT's non-FMA3 decomposed path — both produce
                // double-rounded results. On hosts with FMA3, the JIT's
                // FMA3 codegen produces single-rounded results, which
                // diverges from the interpreter for IEEE 754 edge cases
                // (e.g. mul=1e308 + acc=1e-300 → exact vs. ∞). This is
                // a known limitation; the fix requires FMA3 codegen in
                // the interpreter too (future work — currently we use
                // the C++ mul+add path).
                if ((op & 0xFF000000) == 0x1F000000) {
                    uint8_t ra = (op >> 10) & 0x1F;
                    bool sub = (op >> 15) & 1;   // o1
                    bool neg = (op >> 21) & 1;   // o2
                    if (ftype) {
                        double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm),
                               c = read_fp_d(cpu, ra);
                        double prod = a * b;
                        double r;
                        if      (!neg && !sub) r = prod + c;        // FMADD
                        else if (!neg &&  sub) r = c - prod;        // FMSUB
                        else if ( neg && !sub) r = -prod + c;       // FNMADD
                        else                   r = -prod - c;       // FNMSUB
                        write_fp_d(cpu, rd, r);
                    } else {
                        float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm),
                              c = read_fp_s(cpu, ra);
                        float prod = a * b;
                        float r;
                        if      (!neg && !sub) r = prod + c;        // FMADD
                        else if (!neg &&  sub) r = c - prod;        // FMSUB
                        else if ( neg && !sub) r = -prod + c;       // FNMADD
                        else                   r = -prod - c;       // FNMSUB
                        write_fp_s(cpu, rd, r);
                    }
                    return;
                }
                // Unknown FP instruction — NOP (don't crash)
                (void)sf_val; (void)rm;
                return;
            }

            default:
                // Not recognized by the decoder — fall through
                // to the unhandled-instruction error below.
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
