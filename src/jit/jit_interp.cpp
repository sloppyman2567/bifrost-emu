// jit/jit_interp.cpp — interpreter-step trampoline for JIT fallback.
//
// v1.4.5-alpha: split out of frostjit.cpp. Holds the
// jit_interp_step() extern "C" trampoline, which is called from
// JIT-compiled code (via CALL_INTERP) to fall back to the interpreter
// for one instruction. This is used for:
//   - Instructions the JIT doesn't support (exotic SIMD, system regs)
//   - SVC (syscall) — the interpreter handles the syscall
//   - Debugging (BIFROST_STEP_TRACE=1 logs each fallback)
//
// CRITICAL: this is extern "C" — C++ exceptions cannot propagate
// through it. The interpreter's step() may throw UnmappedMemory; we
// catch it here and translate to SIGSEGV delivery.
#include "core/emulator.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "bifrost/types.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace arm64emu {
// Forward-decls of extern "C" slow-path helpers (defined in x86_backend.cpp).
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, int width);
    void     jit_store_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width);
}
} // namespace arm64emu
// ── jit_interp_step — called from JIT-compiled code ────────────────────
// Invalidates the CPU's page cache before stepping, then dispatches to
// the interpreter. Optional BIFROST_STEP_TRACE env var logs each step.
//
// CRITICAL: this is extern "C" — C++ exceptions cannot propagate
// through it. The interpreter's step() may throw UnmappedMemory; we
// catch it here and translate to SIGSEGV delivery.
extern "C" void jit_interp_step(arm64emu::Emulator* emu, arm64emu::CPU* cpu) {
    // Invalidate the CPU's page cache before stepping.
    cpu->page_cache.read_page = UINT64_MAX;
    cpu->page_cache.write_page = UINT64_MAX;
    // Cache the env lookup — this runs on every CALL_INTERP fallback.
    static const bool step_trace_ = (getenv("BIFROST_STEP_TRACE") != nullptr);
    if (step_trace_) {
        fprintf(stderr, "    [step] pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x24=0x%llx x27=0x%llx pstate=0x%x\n",
                static_cast<unsigned long long>(cpu->pc),
                static_cast<unsigned long long>(cpu->regs[0]),
                static_cast<unsigned long long>(cpu->regs[1]),
                static_cast<unsigned long long>(cpu->regs[2]),
                static_cast<unsigned long long>(cpu->regs[24]),
                static_cast<unsigned long long>(cpu->regs[27]),
                cpu->pstate);
    }
    try {
        emu->step(*cpu);
    } catch (arm64emu::UnmappedMemory& e) {
        // Deliver SIGSEGV with the fault address and proper si_code.
        int si_code = e.write ? arm64emu::SEGV_ACCERR_EMU : arm64emu::SEGV_MAPERR_EMU;
        arm64emu::deliver_signal(*emu, *cpu, emu->signals(), arm64emu::BIFROST_SIGSEGV,
                       si_code, e.addr);
    }
    if (step_trace_) {
        fprintf(stderr, "    [step] pc=0x%llx done x0=0x%llx x24=0x%llx pstate=0x%x\n",
                static_cast<unsigned long long>(cpu->pc),
                static_cast<unsigned long long>(cpu->regs[0]),
                static_cast<unsigned long long>(cpu->regs[24]),
                cpu->pstate);
    }
}
