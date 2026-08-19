// api/bifrost_capi.cpp — C API implementation for libbifrost.
//
// Wraps the C++ Emulator class in a stable C interface so the library
// can be embedded in other programs — debuggers, IDE plugins, test
// harnesses, CI runners, static analyzers, and other tooling — without
// depending on C++ ABI details.
//
// All functions validate their arguments and return meaningful error codes.
// NULL pointers and out-of-range indices are rejected with -1 or NULL.
#include "bifrost.h"
#include "bifrost/emulator.hpp"
#include "core/emulator.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "frontend/dynamic_linker.h"
#include "jit/frostjit.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
// ── Opaque handle ──────────────────────────────────────────────────────
// bifrost_emu_t is defined as an incomplete type in bifrost.h. Here we
// provide the concrete definition: a thin wrapper around Emulator plus
// per-instance state (exit code, running flag, last error message).
struct bifrost_emu {
    arm64emu::Emulator emu;
    bool    running   = false;
    int     exit_code = 0;
    bool    jit_enabled = true;  // JIT is the default (matches bifrost.h)
    bool    jit_verify   = false;
    char    last_error[256] = {};
    std::vector<uint64_t> breakpoints;
};
// Helper: set the last error message (truncated to fit).
static void set_error(bifrost_emu* e, const char* msg) {
    if (!e || !msg) return;
    size_t n = std::strlen(msg);
    if (n >= sizeof(e->last_error)) n = sizeof(e->last_error) - 1;
    std::memcpy(e->last_error, msg, n);
    e->last_error[n] = '\0';
}
// ── Lifecycle ──────────────────────────────────────────────────────────
bifrost_emu_t* bifrost_create(void) {
    try {
        auto* e = new bifrost_emu();
        return reinterpret_cast<bifrost_emu_t*>(e);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}
void bifrost_destroy(bifrost_emu_t* emu) {
    if (!emu) return;
    delete reinterpret_cast<bifrost_emu*>(emu);
}
// ── Loading ────────────────────────────────────────────────────────────
int bifrost_load_elf(bifrost_emu_t* emu, const char* path,
                     int argc, const char* const* argv) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || !path) return -1;
    try {
        std::vector<std::string> args;
        if (argc > 0 && argv) {
            args.reserve(static_cast<size_t>(argc));
            for (int i = 0; i < argc; i++) {
                args.emplace_back(argv[i] ? argv[i] : "");
            }
        }
        e->emu.load_elf_file(path, args);
        e->running = true;
        return 0;
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return -1;
    } catch (...) {
        set_error(e, "unknown error");
        return -1;
    }
}
// Helper: apply the JIT verify env var (read by FrostJIT at first block
// dispatch). Must be set BEFORE the first block runs, so bifrost_set_jit
// and bifrost_run both apply it when the JIT is being enabled.
static void apply_jit_verify_env(bifrost_emu* e) {
    if (e->jit_verify) {
        setenv("BIFROST_JIT_VERIFY", "1", 1);
    } else {
        unsetenv("BIFROST_JIT_VERIFY");
    }
}
// ── Execution ──────────────────────────────────────────────────────────
int bifrost_run(bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    try {
        // JIT is the default execution mode (matches bifrost.h). If the
        // caller enabled it (or left the default), make sure a FrostJIT
        // instance exists before running.
        if (e->jit_enabled && !e->emu.jit()) {
            apply_jit_verify_env(e);
            e->emu.enable_jit();
        }
        e->exit_code = e->emu.run();
        e->running = false;
        return e->exit_code;
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        e->running = false;
        return -1;
    } catch (...) {
        set_error(e, "unknown error");
        e->running = false;
        return -1;
    }
}
int bifrost_step(bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    try {
        auto& cpu = e->emu.main_cpu();
        // Breakpoint check: stop BEFORE executing the instruction at a
        // breakpoint address.
        if (std::find(e->breakpoints.begin(), e->breakpoints.end(), cpu.pc)
            != e->breakpoints.end()) {
            return 1;
        }
        e->emu.step(cpu);
        return 0;
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        e->running = false;
        return -1;
    } catch (...) {
        set_error(e, "unknown error");
        e->running = false;
        return -1;
    }
}
int bifrost_step_n(bifrost_emu_t* emu, uint64_t count) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    try {
        auto& cpu = e->emu.main_cpu();
        for (uint64_t i = 0; i < count; i++) {
            if (std::find(e->breakpoints.begin(), e->breakpoints.end(), cpu.pc)
                != e->breakpoints.end()) {
                return 1;
            }
            e->emu.step(cpu);
        }
        return 0;
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return -1;
    } catch (...) {
        set_error(e, "unknown error");
        return -1;
    }
}
int bifrost_is_running(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    return e && e->running ? 1 : 0;
}
int bifrost_get_exit_code(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    return e ? e->exit_code : -1;
}
// ── Register access ────────────────────────────────────────────────────
uint64_t bifrost_get_reg(const bifrost_emu_t* emu, int reg) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || reg < 0 || reg > 31) return 0;
    auto& cpu = const_cast<arm64emu::Emulator&>(e->emu).main_cpu();
    if (reg == 31) return 0;  // XZR
    return cpu.regs[reg];
}
void bifrost_set_reg(bifrost_emu_t* emu, int reg, uint64_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || reg < 0 || reg > 31) return;
    auto& cpu = e->emu.main_cpu();
    if (reg == 31) return;  // XZR — discard
    cpu.regs[reg] = value;
}
uint64_t bifrost_get_sp(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().sp;
}
void bifrost_set_sp(bifrost_emu_t* emu, uint64_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.main_cpu().sp = value;
}
uint64_t bifrost_get_pc(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().pc;
}
void bifrost_set_pc(bifrost_emu_t* emu, uint64_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.main_cpu().pc = value;
}
// ── FP / SIMD register access ──────────────────────────────────────────
// Each FP register is 128 bits. We expose the low 64 bits (v_lo) and
// high 64 bits (v_hi) separately for portability.
uint64_t bifrost_get_fp_reg_lo(const bifrost_emu_t* emu, int reg) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || reg < 0 || reg > 31) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().v_lo[reg];
}
uint64_t bifrost_get_fp_reg_hi(const bifrost_emu_t* emu, int reg) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || reg < 0 || reg > 31) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().v_hi[reg];
}
void bifrost_set_fp_reg(bifrost_emu_t* emu, int reg,
                        uint64_t lo, uint64_t hi) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || reg < 0 || reg > 31) return;
    auto& cpu = e->emu.main_cpu();
    cpu.v_lo[reg] = lo;
    cpu.v_hi[reg] = hi;
}
// ── PSTATE / flag access ───────────────────────────────────────────────
// NZCV flags are in bits [31:28] of pstate.
uint32_t bifrost_get_pstate(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().pstate;
}
void bifrost_set_pstate(bifrost_emu_t* emu, uint32_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.main_cpu().pstate = value;
}
int bifrost_get_flag(const bifrost_emu_t* emu, int flag) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || flag < 0 || flag > 3) return 0;
    // flag: 0=N, 1=Z, 2=C, 3=V
    auto& cpu = const_cast<arm64emu::Emulator&>(e->emu).main_cpu();
    switch (flag) {
        case 0: return cpu.flag_n() ? 1 : 0;
        case 1: return cpu.flag_z() ? 1 : 0;
        case 2: return cpu.flag_c() ? 1 : 0;
        case 3: return cpu.flag_v() ? 1 : 0;
    }
    return 0;
}
void bifrost_set_flag(bifrost_emu_t* emu, int flag, int value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || flag < 0 || flag > 3) return;
    auto& cpu = e->emu.main_cpu();
    bool v = value != 0;
    switch (flag) {
        case 0: cpu.set_flag_n(v); break;
        case 1: cpu.set_flag_z(v); break;
        case 2: cpu.set_flag_c(v); break;
        case 3: cpu.set_flag_v(v); break;
    }
}
// ── FPSR / FPCR access ─────────────────────────────────────────────────
uint32_t bifrost_get_fpsr(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().fpsr;
}
void bifrost_set_fpsr(bifrost_emu_t* emu, uint32_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.main_cpu().fpsr = value;
}
uint32_t bifrost_get_fpcr(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return 0;
    return const_cast<arm64emu::Emulator&>(e->emu).main_cpu().fpcr;
}
void bifrost_set_fpcr(bifrost_emu_t* emu, uint32_t value) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.main_cpu().fpcr = value;
}
// ── Memory access ──────────────────────────────────────────────────────
int bifrost_read_mem(const bifrost_emu_t* emu, uint64_t addr,
                     void* buf, size_t len) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || !buf) return -1;
    try {
        auto& mem = const_cast<arm64emu::Emulator&>(e->emu).mem();
        mem.read(addr, buf, len);
        return 0;
    } catch (const std::exception& ex) {
        set_error(const_cast<bifrost_emu*>(e), ex.what());
        return -1;
    } catch (...) {
        return -1;
    }
}
int bifrost_write_mem(bifrost_emu_t* emu, uint64_t addr,
                      const void* buf, size_t len) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || !buf) return -1;
    try {
        e->emu.mem().write(addr, buf, len);
        return 0;
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return -1;
    } catch (...) {
        return -1;
    }
}
// ── Configuration ──────────────────────────────────────────────────────
void bifrost_set_trace(bifrost_emu_t* emu, int enable) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.set_trace(enable != 0);
}
void bifrost_set_verbose(bifrost_emu_t* emu, int enable) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.set_verbose(enable != 0);
}
void bifrost_set_jit(bifrost_emu_t* emu, int enable) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    if (enable) {
        // If the JIT isn't initialized yet (e.g. set_jit called before
        // run), initialize it now so the flag takes effect immediately.
        if (!e->emu.jit()) {
            apply_jit_verify_env(e);
            e->emu.enable_jit();
        } else {
            e->emu.set_jit_enabled(true);
        }
    } else {
        e->emu.set_jit_enabled(false);
    }
    e->jit_enabled = (enable != 0);
}
int bifrost_get_jit(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    return (e && e->jit_enabled) ? 1 : 0;
}
void bifrost_set_jit_threshold(bifrost_emu_t* emu, uint64_t n) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->emu.set_jit_threshold(n);
}
void bifrost_set_jit_verify(bifrost_emu_t* emu, int enable) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return;
    e->jit_verify = (enable != 0);
    // Apply immediately so a caller that enabled the JIT earlier (or
    // left the default) still gets verify on the next run.
    apply_jit_verify_env(e);
}
// ── JIT statistics ─────────────────────────────────────────────────────
int bifrost_get_jit_stats(const bifrost_emu_t* emu, bifrost_jit_stats_t* stats) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    auto& emu_ref = const_cast<arm64emu::Emulator&>(e->emu);
    auto* jit = emu_ref.jit();
    if (!jit) return -1;
    // Populate from FrostJIT's public counters.
    stats->blocks_translated     = jit->blocks_translated;
    stats->blocks_executed       = jit->blocks_executed;
    stats->cache_hits            = jit->cache_hits;
    stats->cache_misses          = jit->cache_misses;
    stats->interpreter_fallbacks = jit->interpreter_fallbacks;
    stats->block_chains_patched  = jit->block_chains_patched;
    stats->code_cache_used       = jit->code_buf_used();
    stats->code_cache_size       = jit->code_buf_size();
    stats->cache_entries         = jit->cache_entries();
    return 0;
}
// ── Breakpoints ────────────────────────────────────────────────────────
// Breakpoints are evaluated on the interpreter step path (bifrost_step /
// bifrost_step_n); the main run loop does NOT honor them. See bifrost.h.
int bifrost_set_breakpoint(bifrost_emu_t* emu, uint64_t addr) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    if (std::find(e->breakpoints.begin(), e->breakpoints.end(), addr)
        == e->breakpoints.end()) {
        e->breakpoints.push_back(addr);
    }
    return 0;
}
int bifrost_remove_breakpoint(bifrost_emu_t* emu, uint64_t addr) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    e->breakpoints.erase(
        std::remove(e->breakpoints.begin(), e->breakpoints.end(), addr),
        e->breakpoints.end());
    return 0;
}
// ── Guest function calls ───────────────────────────────────────────────
uint64_t bifrost_call(bifrost_emu_t* emu, uint64_t fn,
                      const int64_t* args, size_t n_args,
                      const double* fargs, size_t n_fargs) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || fn == 0) return 0;
    try {
        auto& cpu = e->emu.main_cpu();
        return e->emu.call_guest_function(cpu, fn, args, n_args,
                                          fargs, n_fargs);
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return 0;
    } catch (...) {
        set_error(e, "unknown error");
        return 0;
    }
}
double bifrost_call_f(bifrost_emu_t* emu, uint64_t fn,
                      const int64_t* args, size_t n_args,
                      const double* fargs, size_t n_fargs) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || fn == 0) return 0.0;
    double fres = 0.0;
    try {
        auto& cpu = e->emu.main_cpu();
        e->emu.call_guest_function(cpu, fn, args, n_args,
                                   fargs, n_fargs, &fres);
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return 0.0;
    } catch (...) {
        set_error(e, "unknown error");
        return 0.0;
    }
    return fres;
}
uint64_t bifrost_lookup_symbol(bifrost_emu_t* emu, const char* name) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || !name || !*name) return 0;
    try {
        auto* dl = e->emu.dyn_linker();
        if (!dl) return 0;
        return dl->resolve_symbol(name);
    } catch (...) {
        return 0;
    }
}
// ── Guest dynamic linking ──────────────────────────────────────────────
uint64_t bifrost_dlopen(bifrost_emu_t* emu, const char* path, int flags) {
    (void)flags;  // accepted for API compatibility; resolution is eager
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || !path || !*path) return 0;
    try {
        auto* dl = e->emu.dyn_linker();
        if (!dl) return 0;
        auto& cpu = e->emu.main_cpu();
        return dl->load_library(cpu, path);
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return 0;
    } catch (...) {
        set_error(e, "unknown error");
        return 0;
    }
}
uint64_t bifrost_dlsym(bifrost_emu_t* emu, uint64_t handle, const char* name) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || !name || !*name || handle == 0) return 0;
    try {
        auto* dl = e->emu.dyn_linker();
        if (!dl) return 0;
        return dl->resolve_symbol_in(handle, name);
    } catch (...) {
        return 0;
    }
}
int bifrost_dlclose(bifrost_emu_t* emu, uint64_t handle) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e || handle == 0) return -1;
    try {
        auto* dl = e->emu.dyn_linker();
        if (!dl) return -1;
        auto& cpu = e->emu.main_cpu();
        return dl->close_library(cpu, handle);
    } catch (const std::exception& ex) {
        set_error(e, ex.what());
        return -1;
    } catch (...) {
        set_error(e, "unknown error");
        return -1;
    }
}
// ── Syscall hook ───────────────────────────────────────────────────────
int bifrost_set_svc_hook(bifrost_emu_t* emu, bifrost_svc_hook_fn fn,
                         void* userdata) {
    auto* e = reinterpret_cast<bifrost_emu*>(emu);
    if (!e) return -1;
    e->emu.set_svc_hook(fn, userdata);
    return 0;
}
// ── Error reporting ────────────────────────────────────────────────────
const char* bifrost_get_error(const bifrost_emu_t* emu) {
    auto* e = reinterpret_cast<const bifrost_emu*>(emu);
    if (!e) return "null emulator handle";
    return e->last_error;
}
// ── Version ────────────────────────────────────────────────────────────
const char* bifrost_version(void) {
    return arm64emu::VERSION;
}
