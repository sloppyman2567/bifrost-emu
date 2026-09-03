// core/crash_report.cpp — shared guest-crash reporter impl.
#include "core/crash_report.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "debug_flags.h"
#include "syscalls/syscalls.h"
#include <cstdio>

namespace arm64emu {

void report_crash(const CPU& cpu, const Memory& mem, const char* reason,
                  uint64_t fault_addr, const uint32_t* inst,
                  const char* mod_name, uint64_t mod_offset) {
    // short line: always on, one line per crash.
    if (mod_name) {
        if (inst) {
            fprintf(stderr, "[crash] %s pc=0x%llx (%s+0x%llx) sp=0x%llx x30=0x%llx fault=0x%llx inst=0x%08x\n",
                    reason, (unsigned long long)cpu.pc, mod_name, (unsigned long long)mod_offset,
                    (unsigned long long)cpu.sp, (unsigned long long)cpu.regs[30],
                    (unsigned long long)fault_addr, *inst);
        } else {
            fprintf(stderr, "[crash] %s pc=0x%llx (%s+0x%llx) sp=0x%llx x30=0x%llx fault=0x%llx\n",
                    reason, (unsigned long long)cpu.pc, mod_name, (unsigned long long)mod_offset,
                    (unsigned long long)cpu.sp, (unsigned long long)cpu.regs[30],
                    (unsigned long long)fault_addr);
        }
    } else if (inst) {
        fprintf(stderr, "[crash] %s pc=0x%llx sp=0x%llx x30=0x%llx fault=0x%llx inst=0x%08x\n",
                reason, (unsigned long long)cpu.pc, (unsigned long long)cpu.sp,
                (unsigned long long)cpu.regs[30], (unsigned long long)fault_addr,
                *inst);
    } else {
        fprintf(stderr, "[crash] %s pc=0x%llx sp=0x%llx x30=0x%llx fault=0x%llx\n",
                reason, (unsigned long long)cpu.pc, (unsigned long long)cpu.sp,
                (unsigned long long)cpu.regs[30], (unsigned long long)fault_addr);
    }
    // last syscall/thunk: what the guest last asked for.
    if (cpu.has_last_svc) {
        const char* name = syscall_name_for_crash(cpu.last_svc_num);
        if (cpu.last_svc_num == 0x1000) {
            fprintf(stderr, "[crash] last=thunk sym=%u @0x%llx\n",
                    (unsigned)(cpu.last_thunk_sym & 0xFFFFFFFFu),
                    (unsigned long long)cpu.last_svc_pc);
        } else {
            fprintf(stderr, "[crash] last=%s(%llu) @0x%llx\n", name,
                    (unsigned long long)cpu.last_svc_num,
                    (unsigned long long)cpu.last_svc_pc);
        }
    }
    fflush(stderr);

    // detail: only with explicit flags.
    if (!dbg().crash_dump && !dbg().dbg_guard && !dbg().trace_crash) return;

    for (int i = 0; i < 31; i++) {
        fprintf(stderr, "  x%d=0x%llx", i, (unsigned long long)cpu.regs[i]);
        if ((i & 3) == 3) fprintf(stderr, "\n");
    }
    fprintf(stderr, "  sp=0x%llx pc=0x%llx pstate=0x%x\n",
            (unsigned long long)cpu.sp, (unsigned long long)cpu.pc, cpu.pstate);

    // stack: 16 words at sp.
    fprintf(stderr, "  stack @0x%llx:\n", (unsigned long long)cpu.sp);
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        try {
            mem.read(cpu.sp + (uint64_t)i * 8, &v, 8);
        } catch (...) { v = 0; }
        fprintf(stderr, "   [sp+0x%02x] 0x%llx\n", i * 8, (unsigned long long)v);
    }

    // fp chain.
    fprintf(stderr, "  call stack:\n");
    fprintf(stderr, "   #0 pc=0x%llx lr=0x%llx\n",
            (unsigned long long)cpu.pc, (unsigned long long)cpu.regs[30]);
    uint64_t fp = cpu.regs[29];
    for (int fr = 1; fr < 24; fr++) {
        if (fp == 0 || fp == ~0ULL) break;
        uint64_t next_fp = 0, lr = 0;
        try {
            mem.read(fp, &next_fp, 8);
            mem.read(fp + 8, &lr, 8);
        } catch (...) { break; }
        fprintf(stderr, "   #%d fp=0x%llx lr=0x%llx\n",
                fr, (unsigned long long)fp, (unsigned long long)lr);
        if (next_fp <= fp && next_fp != 0) break;
        fp = next_fp;
    }
    fflush(stderr);
}

}  // namespace arm64emu
