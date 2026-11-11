// CrashReport.cpp — shared guest-crash reporter impl.
#include "System/Core/CrashReport.hpp"
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include <cstdio>
#include <string>

namespace arm64emu {

void report_crash(const CPU& cpu, const Memory& mem, const char* reason,
                  uint64_t fault_addr, const uint32_t* inst,
                  const char* mod_name, uint64_t mod_offset) {
    // short line: always on, one line per crash. Names the file each address lies in: from the caller when it knows
    // (the emulator's own linker), else from the guest's mappings, which is how an Android app's libraries show up
    // (Bionic's linker maps them itself).
    std::string pc_mod, lr_mod;
    char buf[320];
    if (mod_name) {
        snprintf(buf, sizeof(buf), " (%s+0x%llx)", mod_name, (unsigned long long)mod_offset);
        pc_mod = buf;
    }
    try {
        const auto maps = mem.mappings_snapshot();
        auto where = [&](uint64_t addr) -> std::string {
            for (const auto& v : maps) {
                if (addr < v.start || addr >= v.end || !v.file) continue;
                const std::string& path = v.file->path;
                const size_t slash = path.rfind('/');
                snprintf(buf, sizeof(buf), " (%s+0x%llx)", path.c_str() + (slash == std::string::npos ? 0 : slash + 1),
                         (unsigned long long)(v.offset + (addr - v.start)));
                return buf;
            }
            return {};
        };
        if (pc_mod.empty()) pc_mod = where(cpu.pc);
        lr_mod = where(cpu.regs[30]);
    } catch (...) {}
    if (inst) {
        fprintf(stderr, "[crash] %s pc=0x%llx%s sp=0x%llx x30=0x%llx%s fault=0x%llx inst=0x%08x\n",
                reason, (unsigned long long)cpu.pc, pc_mod.c_str(), (unsigned long long)cpu.sp,
                (unsigned long long)cpu.regs[30], lr_mod.c_str(), (unsigned long long)fault_addr, *inst);
    } else {
        fprintf(stderr, "[crash] %s pc=0x%llx%s sp=0x%llx x30=0x%llx%s fault=0x%llx\n",
                reason, (unsigned long long)cpu.pc, pc_mod.c_str(), (unsigned long long)cpu.sp,
                (unsigned long long)cpu.regs[30], lr_mod.c_str(), (unsigned long long)fault_addr);
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
            (unsigned long long)cpu.sp, (unsigned long long)cpu.pc, cpu.nzcv());

    // stack: 16 words at sp.
    fprintf(stderr, "  stack @0x%llx:\n", (unsigned long long)cpu.sp);
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        uint64_t a = 0;
        uint64_t off = (uint64_t)i * 8;
        if (!__builtin_add_overflow(cpu.sp, off, &a)) {
            try {
                mem.read(a, &v, 8);
            } catch (...) { v = 0; }
        }
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
        uint64_t fp8 = 0;
        if (__builtin_add_overflow(fp, 8, &fp8)) break;
        try {
            mem.read(fp, &next_fp, 8);
            mem.read(fp8, &lr, 8);
        } catch (...) { break; }
        fprintf(stderr, "   #%d fp=0x%llx lr=0x%llx\n",
                fr, (unsigned long long)fp, (unsigned long long)lr);
        if (next_fp <= fp && next_fp != 0) break;
        fp = next_fp;
    }
    fflush(stderr);
}

}  // namespace arm64emu
