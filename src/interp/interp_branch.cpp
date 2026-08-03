// interp/interp_branch.cpp — branch/system instruction handlers, extracted
// from interpreter.cpp.
//
// v1.4.5-alpha refactor: split out of interpreter.cpp. This file holds the
// B, BL, Bcond, CBZ, CBNZ, TBZ, TBNZ, BR, BLR, RET, SVC_IMM, BRK_IMM,
// HLT_IMM, CLREX_INST, HINT, MRS_SYS, MSR_SYS cases of the dispatch
// switch, extracted into a member function (execute_branch) for
// readability. The main switch in interpreter.cpp dispatches to this method.
//
// No behavior change — pure file split. The method is a member of Emulator
// (declared in src/core/emulator.h) so it has full access to mem_,
// brk_verbose_, syscall(), etc.
#include "core/emulator.h"
#include "decoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace arm64emu {
// execute_branch — handle branch and system instruction classes.
//
// Called from Emulator::execute() for:
//   InstClass::B, BL, Bcond, CBZ, CBNZ, TBZ, TBNZ, BR, BLR, RET,
//   InstClass::SVC_IMM, BRK_IMM, HLT_IMM, CLREX_INST, HINT,
//   InstClass::MRS_SYS, MSR_SYS
//
// The signature mirrors execute() (inst + next_pc + cpu + d) for
// consistency. `inst` is currently only used by the SVC debug path
// (indirectly via d.raw), but kept for symmetry with execute_fp().
void Emulator::execute_branch(uint32_t inst, uint64_t& next_pc, CPU& cpu, const DecodedInst& d) {
    (void)inst;  // raw bits are available via d.raw; inst kept for API symmetry
    switch (d.cls) {
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
            // RET Rn branches to regs[Rn]. Rn==31 (XZR) is encodable and
            // branches to address 0 (regs[31] is always 0) — do NOT fall
            // back to LR here; the IR path also emits BR regs[31].
            next_pc = cpu.regs[d.rn];
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
                // ── Guest call stack (AArch64 x29 frame chain) ─────
                // Standard frame record: [fp+0] = caller's fp, [fp+8] = LR.
                fprintf(stderr, "  call stack:\n");
                uint64_t fp_v = cpu.regs[29];
                fprintf(stderr, "   #0  pc=0x%llx lr=0x%llx\n",
                        (unsigned long long)cpu.pc,
                        (unsigned long long)cpu.regs[30]);
                for (int fr = 1; fr < 32; fr++) {
                    if (fp_v == 0 || fp_v == 0xffffffffffffffffULL) break;
                    uint64_t next_fp = 0, lr = 0;
                    try { mem_.read(fp_v, &next_fp, 8); } catch (...) { break; }
                    try { mem_.read(fp_v + 8, &lr, 8); } catch (...) { break; }
                    fprintf(stderr, "   #%d  fp=0x%llx lr=0x%llx\n",
                            fr, (unsigned long long)fp_v,
                            (unsigned long long)lr);
                    if (next_fp <= fp_v && next_fp != 0) break;  // stack grows down
                    fp_v = next_fp;
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
        default:
            // Not a branch/system class — should never be called here.
            // The dispatcher in interpreter.cpp only routes branch/system
            // cases to execute_branch(); reaching this default is a logic bug.
            break;
    }
}
} // namespace arm64emu
