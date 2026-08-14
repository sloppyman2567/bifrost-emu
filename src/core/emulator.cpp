// core/emulator.cpp — Emulator lifecycle, ELF loading, run loop.
//
// This file implements the high-level "drive the emulator" logic:
//   - Constructor / destructor
//   - load_elf_file() — load an ELF, set up the initial stack + TLS
//   - run() — main interpreter/JIT dispatch loop with hang watchdog,
//     periodic signal draining, and SDL2 refresh hooks
//   - step() — decode-cache-aware single-instruction step (with tracing)
//   - Host signal handlers (SIGINT/SIGTERM/SIGCHLD/SIGWINCH → guest)
//
// The per-instruction execute() body lives in src/interp/interpreter.cpp.
// The syscall() body lives in src/syscalls/syscalls.cpp. Thread spawn/join
// lives in src/core/thread_mgr.cpp. Each is a friend of Emulator.
#include "core/emulator.h"
#include "bifrost/version.hpp"
#include "core/memory.h"
#include "frontend/dynamic_linker.h"
#include "frontend/vdso_bytes.h"      // embedded AArch64 vDSO (1.5.2-alpha)
#include "frost/thunk.hpp"        // GraphicThunk full definition (for init/resolve)
#include "frost/audio_thunk.hpp"  // 1.5.2-alpha: AudioThunk
#include "frost/display_thunk.hpp"// 1.5.2-alpha: DisplayThunk
#include "jit/frostjit.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace arm64emu {
// ── Static active-emu pointer (defined in src/core/signal.cpp) ────────
// Emulator* Emulator::g_active_emu_ = nullptr;  // defined in signal.cpp
Emulator::Emulator() = default;
Emulator::~Emulator() {
    // Clear the active-emu pointer so host signal handlers don't
    // dereference freed memory. Without this, a signal arriving after
    // the destructor runs would crash with UAF.
    g_active_emu_ = nullptr;
}
void* Emulator::excl_monitor_shard_pub(uint64_t addr) {
    return &excl_monitor_shards_[excl_shard_idx(addr)];
}
// ── ELF loading ───────────────────────────────────────────────────────
void Emulator::load_elf_file(const std::string& path, std::vector<std::string>& argv) {
    elf_path_ = path;
    // Initialize the guest process name (comm) to the ELF basename.
    // This matches the Linux kernel behavior: the initial comm is the
    // executable's basename, truncated to 15 chars. prctl(PR_SET_NAME)
    // can override it later.
    {
        size_t slash = path.find_last_of('/');
        std::string base = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        set_guest_comm(base);
    }
    vfs_.set_elf_path(path);
    vfs_.set_argv(argv);
    vfs_.set_graphics(&graphics_);
    vfs_.set_audio(&audio_);
    // Wire up /proc/self/maps to query live memory state. The callback
    // captures `this` — the Emulator outlives the VFS, so this is safe.
    // BUGFIX: previously /proc/self/maps returned hardcoded 5-line string
    // that didn't reflect actual guest memory layout. Now we emit real
    // entries: ELF load range, brk (heap), stack, mmap region, and
    // dynamic linker range.
    vfs_.set_maps_provider([this]() {
        std::vector<yggdrasil::Yggdrasil::MapEntry> out;
        // ELF image: from end_addr_ min down to lowest PT_LOAD start.
        // We don't track the lowest PT_LOAD start, so use end_addr_ as
        // the upper bound and 0x400000 (typical PIE/static base) as the
        // lower bound heuristic. Conservative: covers all code/data.
        if (end_addr_ > 0) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = 0x400000;
            e.end   = end_addr_;
            std::snprintf(e.perms, sizeof(e.perms), "rwxp");
            out.push_back(e);
        }
        // Heap (brk): from brk_start_ to brk_.
        if (brk_start_ > 0 && brk_ >= brk_start_) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = brk_start_;
            e.end   = brk_;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            e.label = "[heap]";
            out.push_back(e);
        }
        // Dynamic linker range.
        if (interp_base_ > 0) {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = interp_base_;
            e.end   = interp_base_ + 0x10000000;  // 256 MiB upper bound
            std::snprintf(e.perms, sizeof(e.perms), "rwxp");
            e.label = "[interp]";
            out.push_back(e);
        }
        // All mmap_alloc'd regions (excluding the heap which is above).
        for (const auto& kv : mem_.allocations_snapshot()) {
            uint64_t a = kv.first, s = kv.second;
            // Skip the heap region (already emitted above).
            if (a == brk_start_) continue;
            // Skip the TLS scratch area near brk_start_ (it's part of the
            // mmap region but we want to show it as anon).
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = a;
            e.end   = a + s;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            out.push_back(e);
        }
        // Stack: fixed 64 MiB at Memory::STACK_TOP - 64 MiB.
        {
            yggdrasil::Yggdrasil::MapEntry e;
            e.start = Memory::STACK_TOP - Memory::STACK_SIZE;
            e.end   = Memory::STACK_TOP;
            std::snprintf(e.perms, sizeof(e.perms), "rw-p");
            e.label = "[stack]";
            out.push_back(e);
        }
        return out;
    });
    // Wire up guest cwd tracking. The host cwd is meaningless because
    // BIFROST_ROOT sandboxing decouples guest paths from host paths.
    // The guest starts at "/" by default; chdir/fchdir update this.
    vfs_.set_cwd_provider(
        [this]() -> std::string { return guest_cwd_; },
        [this](const std::string& p) -> bool {
            // Resolve relative paths against the current cwd.
            if (p.empty()) return false;
            if (p[0] == '/') {
                guest_cwd_ = p;
            } else {
                if (guest_cwd_.empty()) guest_cwd_ = "/";
                if (guest_cwd_.back() == '/') guest_cwd_.pop_back();
                guest_cwd_ += "/";
                guest_cwd_ += p;
            }
            // Normalize: collapse "." and ".." segments.
            std::vector<std::string> parts;
            std::stringstream ss(guest_cwd_);
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                if (seg.empty() || seg == ".") continue;
                if (seg == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else {
                    parts.push_back(seg);
                }
            }
            guest_cwd_ = "/";
            for (size_t i = 0; i < parts.size(); i++) {
                guest_cwd_ += parts[i];
                if (i + 1 < parts.size()) guest_cwd_ += "/";
            }
            return true;
        });
    // Wire up /proc/self/comm — returns the guest process name (set by
    // prctl PR_SET_NAME, defaults to ELF basename).
    vfs_.set_comm_provider([this]() -> std::string {
        return guest_comm_;
    });
    // Open the ELF through the BIFROST_ROOT sandbox: `path` is the guest
    // path (what /proc/self/exe reports), so the host file is the remapped
    // "$BIFROST_ROOT/<path>". Relative paths pass through unchanged, so
    // existing `bifrost-emu ctest/foo.elf` invocations are untouched.
    // This keeps /proc/self/exe consistent with the guest's view of the
    // filesystem (Qt derives applicationDirPath from it).
    const std::string host_elf_path = yggdrasil::Yggdrasil::remap_path(path);
    FILE* f = fopen(host_elf_path.c_str(), "rb");
    if (!f) throw EmuError("cannot open " + host_elf_path + ": " + strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); throw EmuError("empty or invalid ELF"); }
    std::vector<uint8_t> data(sz);
    if (fread(data.data(), 1, sz, f) != static_cast<size_t>(sz)) {
        fclose(f); throw EmuError("short read on " + host_elf_path);
    }
    fclose(f);
    auto info = ElfLoader::load(mem_, data);
    entry_ = info.entry;
    prog_entry_ = info.entry;  // save original entry for AT_ENTRY
    end_addr_ = info.end_addr;
    phdr_addr_ = info.phdr_addr;
    phnum_ = info.phnum;
    phent_ = info.phent;
    has_lse_ = info.has_lse;
    interp_base_ = 0;
    // If the binary has a PT_INTERP (dynamic linker), load it.
    // The dynamic linker's entry point becomes the real entry point;
    // the binary's entry is passed via AT_ENTRY in auxv.
    //
    // Two paths are supported:
    //   (a) If BIFROST_NATIVE_DYNLINK=1, we use our own DynamicLinker
    //       to process DT_NEEDED, apply relocations, and resolve
    //       symbols — no guest-side ld.so needed. Faster and works
    //       even when the host doesn't have the exact aarch64 ld.so.
    //   (b) Otherwise, load the guest-side dynamic linker (ld-musl /
    //       ld-linux) and let it run its own code to do dynamic linking.
    //       This works for simple dynamically-linked musl binaries but
    //       not yet for glibc.
    uint64_t interp_base = 0;
    if (!info.interp.empty()) {
        // First, try the native (in-emulator) dynamic linker. This is
        // faster and more reliable because we don't depend on the
        // guest-side ld.so working correctly under emulation.
        // Default: use the native (in-emulator) dynamic linker. It's faster
        // and more reliable than depending on the guest-side ld.so under
        // emulation. Set BIFROST_NO_NATIVE_DYNLINK=1 to fall back to the
        // guest-side ld.so (for debugging).
        bool native_dynlink = (getenv("BIFROST_NO_NATIVE_DYNLINK") == nullptr);
        if (native_dynlink) {
            dyn_linker_ = std::make_unique<DynamicLinker>(mem_);
            // ── Wire GraphicThunk into the dynamic linker ──
            // When BIFROST_THUNK_GRAPHICS=1 is set, the thunk forwards
            // guest GL/EGL/SDL2 calls to the host. We init the thunk
            // (allocates the guest trampoline page) and register a
            // resolver callback so the dynamic linker can populate the
            // global symbol table for graphic libraries that couldn't
            // be loaded from disk.
            //
            // We always wire the resolver (even if the thunk is
            // disabled) — when disabled, enumerate_symbols() returns 0
            // and the dynamic linker falls through to its existing
            // "library not found" path. This keeps the code path
            // uniform.
            if (auto* thunk = graphics_.thunk()) {
                if (thunk->enabled()) {
                    thunk->init(mem_);
                }
                // Capture the thunk pointer (not `this`) so the
                // callback doesn't depend on the Emulator's lifetime
                // beyond the thunk's. The thunk is owned by graphics_,
                // which is owned by the Emulator — they have the same
                // lifetime.
                GraphicThunk* thunk_ptr = thunk;
                dyn_linker_->set_thunk_resolver(
                    [thunk_ptr](const std::string& lib)
                        -> DynamicLinker::ThunkSymbolList {
                        DynamicLinker::ThunkSymbolList out;
                        if (!thunk_ptr->enabled()) return out;
                        thunk_ptr->enumerate_symbols(lib,
                            [&](const std::string& sym, uint64_t addr) {
                                out.emplace_back(sym, addr);
                            });
                        return out;
                    });
            }
            // 1.5.2-alpha: also init AudioThunk and DisplayThunk if
            // enabled. They share the thunk syscall (0x1000) with
            // GraphicThunk; the dispatcher in misc.cpp tries each in
            // order. The dynamic linker's thunk_resolver_ is set to
            // GraphicThunk's enumerate_symbols, so audio/display symbols
            // need a different wiring path — we extend the resolver to
            // also consult the other thunks. (See set_thunk_resolver
            // extension below.)
            if (auto* athunk = graphics_.audio_thunk()) {
                if (athunk->enabled()) athunk->init(mem_);
            }
            if (auto* dthunk = graphics_.display_thunk()) {
                if (dthunk->enabled()) dthunk->init(mem_);
            }
            // Extend the thunk resolver to consult all three thunks.
            // Each thunk has its own per-library symbol enumeration; we
            // merge their results so the dynamic linker sees the union.
            {
                GraphicThunk* gthunk = graphics_.thunk();
                AudioThunk*   athunk = graphics_.audio_thunk();
                DisplayThunk* dthunk = graphics_.display_thunk();
                dyn_linker_->set_thunk_resolver(
                    [gthunk, athunk, dthunk](const std::string& lib)
                        -> DynamicLinker::ThunkSymbolList {
                        DynamicLinker::ThunkSymbolList out;
                        auto add_all = [&](auto* t) {
                            if (!t || !t->enabled()) return;
                            t->enumerate_symbols(lib,
                                [&](const std::string& sym, uint64_t addr) {
                                    out.emplace_back(sym, addr);
                                });
                        };
                        add_all(gthunk);
                        add_all(athunk);
                        add_all(dthunk);
                        return out;
                    });
            }
            // Register the ifunc resolver callback. BUGFIX: the old
            // IRELATIVE handler just stored the resolver ADDRESS instead
            // of CALLING it. Now we run the resolver in a scratch CPU
            // state (borrowing the caller's parked CPU, which hasn't
            // been initialized yet during startup link() — its real init
            // happens later). The resolver is a small guest function that
            // returns a function pointer in X0; we run it via step
            // until it RETs to a sentinel address, then capture X0.
            dyn_linker_->set_ifunc_resolver([this](CPU& cpu, uint64_t resolver_addr) -> uint64_t {
                if (resolver_addr == 0) return 0;
                // Borrow `cpu` as scratch — this is the CALLING thread's
                // parked CPU (cpu during startup link(), which is
                // still-uninitialized and idle). Save its architectural
                // state (regs, sp, pc, pstate, FP regs, TLS, sigmask) so we
                // don't disturb the caller's syscall context. We can't copy
                // the whole CPU struct because it has mutex + atomic members
                // that are non-copyable. The pending-queue state is
                // irrelevant — ifunc resolution runs at load time before
                // any signals can be pending.
                struct SavedState {
                    uint64_t regs[32];
                    uint64_t sp, pc;
                    uint32_t pstate;
                    uint64_t v_lo[32], v_hi[32];
                    uint32_t fpcr, fpsr;
                    uint64_t tpidr_el0, tpidrro_el0;
                    uint64_t sigmask;
                    bool running;
                } saved;
                static_assert(sizeof(saved.regs) == sizeof(cpu.regs), "");
                std::memcpy(saved.regs, cpu.regs, sizeof(saved.regs));
                saved.sp = cpu.sp;
                saved.pc = cpu.pc;
                saved.pstate = cpu.pstate;
                std::memcpy(saved.v_lo, cpu.v_lo, sizeof(saved.v_lo));
                std::memcpy(saved.v_hi, cpu.v_hi, sizeof(saved.v_hi));
                saved.fpcr = cpu.fpcr;
                saved.fpsr = cpu.fpsr;
                saved.tpidr_el0 = cpu.tpidr_el0;
                saved.tpidrro_el0 = cpu.tpidrro_el0;
                saved.sigmask = cpu.sigmask;
                saved.running = cpu.running;
                auto restore = [&]() {
                    std::memcpy(cpu.regs, saved.regs, sizeof(saved.regs));
                    cpu.sp = saved.sp;
                    cpu.pc = saved.pc;
                    cpu.pstate = saved.pstate;
                    std::memcpy(cpu.v_lo, saved.v_lo, sizeof(saved.v_lo));
                    std::memcpy(cpu.v_hi, saved.v_hi, sizeof(saved.v_hi));
                    cpu.fpcr = saved.fpcr;
                    cpu.fpsr = saved.fpsr;
                    cpu.tpidr_el0 = saved.tpidr_el0;
                    cpu.tpidrro_el0 = saved.tpidrro_el0;
                    cpu.sigmask = saved.sigmask;
                    cpu.running = saved.running;
                };
                // Allocate a small scratch stack for the resolver (4 KiB
                // is plenty — resolvers are leaf-ish functions that don't
                // recurse deeply).
                uint64_t scratch_stack = mem_.mmap_alloc(4096);
                uint64_t stack_top = scratch_stack + 4096;
                // Sentinel return address — when PC == this, the resolver
                // has RET'd. Use 0x1000 (in the zero page, unmapped for
                // execution but a recognizable sentinel).
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = resolver_addr;
                cpu.sp = stack_top;
                cpu.regs[30] = SENTINEL_LR;  // LR
                cpu.running = true;
                cpu.pstate = 0;
                // Run the resolver. Cap at 1M instructions to avoid
                // infinite loops in buggy resolvers.
                constexpr uint64_t IRESOLVER_LIMIT = 1'000'000;
                uint64_t steps = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < IRESOLVER_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                } catch (const std::exception& e) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                e.what());
                    }
                    restore();
                    return 0;
                }
                uint64_t result = cpu.regs[0];
                if (steps >= IRESOLVER_LIMIT) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx ran >%llu "
                                "instructions; aborting (likely infinite loop)\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                static_cast<unsigned long long>(IRESOLVER_LIMIT));
                    }
                    result = 0;
                }
                if (getenv("BIFROST_IFUNC_TRACE")) {
                    fprintf(stderr, "[%s] ifunc resolver at 0x%llx returned 0x%llx "
                            "(steps=%llu)\n",
                            CODENAME,
                            static_cast<unsigned long long>(resolver_addr),
                            static_cast<unsigned long long>(result),
                            static_cast<unsigned long long>(steps));
                }
                // Restore cpu to its pre-resolver state.
                restore();
                return result;
            });
            // dynamic linker can invoke DT_INIT_ARRAY entries (C++ static
            // constructors, glibc hooks, etc.). Uses the same borrow-CPU
            // pattern as the ifunc resolver, but doesn't capture X0 —
            // just runs the function until it RETs.
            dyn_linker_->set_init_runner([this](CPU& cpu, uint64_t fn_addr) {
                if (fn_addr == 0) return;
                // Save architectural state (same pattern as ifunc resolver).
                // `cpu` is the caller's parked CPU (cpu at startup).
                struct SavedState {
                    uint64_t regs[32];
                    uint64_t sp, pc;
                    uint32_t pstate;
                    uint64_t v_lo[32], v_hi[32];
                    uint32_t fpcr, fpsr;
                    uint64_t tpidr_el0, tpidrro_el0;
                    uint64_t sigmask;
                    bool running;
                } saved;
                std::memcpy(saved.regs, cpu.regs, sizeof(saved.regs));
                saved.sp = cpu.sp;
                saved.pc = cpu.pc;
                saved.pstate = cpu.pstate;
                std::memcpy(saved.v_lo, cpu.v_lo, sizeof(saved.v_lo));
                std::memcpy(saved.v_hi, cpu.v_hi, sizeof(saved.v_hi));
                saved.fpcr = cpu.fpcr;
                saved.fpsr = cpu.fpsr;
                saved.tpidr_el0 = cpu.tpidr_el0;
                saved.tpidrro_el0 = cpu.tpidrro_el0;
                saved.sigmask = cpu.sigmask;
                saved.running = cpu.running;
                auto restore = [&]() {
                    std::memcpy(cpu.regs, saved.regs, sizeof(saved.regs));
                    cpu.sp = saved.sp;
                    cpu.pc = saved.pc;
                    cpu.pstate = saved.pstate;
                    std::memcpy(cpu.v_lo, saved.v_lo, sizeof(saved.v_lo));
                    std::memcpy(cpu.v_hi, saved.v_hi, sizeof(saved.v_hi));
                    cpu.fpcr = saved.fpcr;
                    cpu.fpsr = saved.fpsr;
                    cpu.tpidr_el0 = saved.tpidr_el0;
                    cpu.tpidrro_el0 = saved.tpidrro_el0;
                    cpu.sigmask = saved.sigmask;
                    cpu.running = saved.running;
                };
                // before running init functions. Constructors and
                // __libc_early_init access TLS variables (e.g., ctype
                // tables, locale pointers) via TPIDR_EL0. Without this,
                // TLS variables are at wrong addresses and initialization
                // fails silently.
                if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
                    cpu.tpidr_el0 == 0) {
                    // Borrowed CPU has no TLS pointer yet (startup path —
                    // main_cpu_ is still default-initialized). Set it to the
                    // static TLS block. A parked guest thread (runtime dl*)
                    // already has its own per-thread TPIDR_EL0; leave it so
                    // the guest constructors see ITS TLS, not the main
                    // thread's.
                    // Variant-I: TP = base + libc_size (TCB header start).
                    cpu.tpidr_el0 = dyn_linker_->thread_pointer();
                    cpu.tpidrro_el0 = cpu.tpidr_el0;
                }
                // Scratch stack for the constructor.
                uint64_t scratch_stack = mem_.mmap_alloc(4096);
                uint64_t stack_top = scratch_stack + 4096;
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = fn_addr;
                cpu.sp = stack_top;
                cpu.regs[30] = SENTINEL_LR;
                cpu.running = true;
                cpu.pstate = 0;
                // Constructors may call libc functions (e.g., printf for
                // debug logging), which may themselves use atomics / TLS.
                // Cap at 10M instructions to avoid infinite loops in
                // buggy constructors.
                constexpr uint64_t INIT_LIMIT = 10'000'000;
                uint64_t steps = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < INIT_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                } catch (const std::exception& e) {
                    if (getenv("BIFROST_DYNLINK_TRACE")) {
                        fprintf(stderr, "[%s] init_array @ 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(fn_addr),
                                e.what());
                    }
                }
                if (steps >= INIT_LIMIT && getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[%s] init_array @ 0x%llx ran >%llu "
                            "instructions; aborting (likely infinite loop)\n",
                            CODENAME,
                            static_cast<unsigned long long>(fn_addr),
                            static_cast<unsigned long long>(INIT_LIMIT));
                }
                restore();
            });
            // Argument-passing guest-call callback. Same borrow-CPU
            // pattern as init_runner_, but sets x0/x1/x2 before running
            // and returns x0. Used by dl_iterate_phdr (callback+data)
            // and dlopen init arrays (argc/argv/env). Step limit is
            // 50M (higher than init_runner_'s 10M) because dl_iterate_phdr
            // callbacks may do significant work (e.g., backtrace walking).
            dyn_linker_->set_guest_call_args([this](CPU& cpu,
                                                      uint64_t fn_addr,
                                                      uint64_t arg0,
                                                      uint64_t arg1,
                                                      uint64_t arg2) -> uint64_t {
                if (fn_addr == 0) return 0;
                struct SavedState {
                    uint64_t regs[32];
                    uint64_t sp, pc;
                    uint32_t pstate;
                    uint64_t v_lo[32], v_hi[32];
                    uint32_t fpcr, fpsr;
                    uint64_t tpidr_el0, tpidrro_el0;
                    uint64_t sigmask;
                    bool running;
                } saved;
                std::memcpy(saved.regs, cpu.regs, sizeof(saved.regs));
                saved.sp = cpu.sp;
                saved.pc = cpu.pc;
                saved.pstate = cpu.pstate;
                std::memcpy(saved.v_lo, cpu.v_lo, sizeof(saved.v_lo));
                std::memcpy(saved.v_hi, cpu.v_hi, sizeof(saved.v_hi));
                saved.fpcr = cpu.fpcr;
                saved.fpsr = cpu.fpsr;
                saved.tpidr_el0 = cpu.tpidr_el0;
                saved.tpidrro_el0 = cpu.tpidrro_el0;
                saved.sigmask = cpu.sigmask;
                saved.running = cpu.running;
                auto restore = [&]() {
                    std::memcpy(cpu.regs, saved.regs, sizeof(saved.regs));
                    cpu.sp = saved.sp;
                    cpu.pc = saved.pc;
                    cpu.pstate = saved.pstate;
                    std::memcpy(cpu.v_lo, saved.v_lo, sizeof(saved.v_lo));
                    std::memcpy(cpu.v_hi, saved.v_hi, sizeof(saved.v_hi));
                    cpu.fpcr = saved.fpcr;
                    cpu.fpsr = saved.fpsr;
                    cpu.tpidr_el0 = saved.tpidr_el0;
                    cpu.tpidrro_el0 = saved.tpidrro_el0;
                    cpu.sigmask = saved.sigmask;
                    cpu.running = saved.running;
                };
                // Set up TLS pointer (same as init_runner_). Only when the
                // borrowed CPU has none (startup). A parked guest thread
                // already has its own per-thread TPIDR_EL0.
                if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
                    cpu.tpidr_el0 == 0) {
                    cpu.tpidr_el0 = dyn_linker_->thread_pointer();
                    cpu.tpidrro_el0 = cpu.tpidr_el0;
                }
                uint64_t scratch_stack = mem_.mmap_alloc(4096);
                uint64_t stack_top = scratch_stack + 4096;
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = fn_addr;
                cpu.sp = stack_top;
                cpu.regs[0] = arg0;
                cpu.regs[1] = arg1;
                cpu.regs[2] = arg2;
                cpu.regs[30] = SENTINEL_LR;
                cpu.running = true;
                cpu.pstate = 0;
                constexpr uint64_t CALL_LIMIT = 50'000'000;
                uint64_t steps = 0;
                uint64_t result = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < CALL_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                    result = cpu.regs[0];
                } catch (const std::exception& e) {
                    if (getenv("BIFROST_DYNLINK_TRACE")) {
                        fprintf(stderr, "[%s] guest_call @ 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(fn_addr),
                                e.what());
                    }
                    result = 0;
                }
                if (steps >= CALL_LIMIT && getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[%s] guest_call @ 0x%llx ran >%llu "
                            "instructions; aborting\n",
                            CODENAME,
                            static_cast<unsigned long long>(fn_addr),
                            static_cast<unsigned long long>(CALL_LIMIT));
                    result = 0;
                }
                restore();
                return result;
            });
            if (!dyn_linker_->link(main_cpu_, data, info.base_addr, path, info.interp)) {
                fprintf(stderr, "[%s] native dynamic linking failed: %s; "
                        "falling back to guest ld.so\n",
                        CODENAME, dyn_linker_->error().c_str());
                dyn_linker_.reset();
            } else {
                libc_single_threaded_addr_ =
                    dyn_linker_->libc_single_threaded_addr();
            }
            if (dyn_linker_ && dyn_linker_->static_tls_size() > 0) {
                // Set up TPIDR_EL0 using variant-I TLS layout (glibc AArch64):
                //   TP = static_tls_base + lib_size
                // The TCB header (tcbhead_t) is at [TP, TP + tcb_size).
                // Main exe TLS is at [TP + tcb_size, ...) (positive offset).
                // Lib TLS is at [TP - lib_size, TP) (negative offset).
                //
                // END of the block, variant-II). This broke local-exec TLS
                // access for the main exe — the hardcoded positive TPREL
                // offset landed in the wrong place. With variant-I, TP points
                // to the TCB header start (= base + lib_size).
                uint64_t tp = dyn_linker_->thread_pointer();
                main_cpu_.tpidr_el0 = tp;
                main_cpu_.tpidrro_el0 = tp;
                if (verbose_) {
                    fprintf(stderr, "[%s] native dynlink: static TLS block "
                            "at 0x%llx (size %llu), TP=0x%llx (variant-I)\n",
                            CODENAME,
                            static_cast<unsigned long long>(dyn_linker_->static_tls_base()),
                            static_cast<unsigned long long>(dyn_linker_->static_tls_size()),
                            static_cast<unsigned long long>(tp));
                }
            }
            // Even with native dynlink, we still load the interpreter
            // (ld.so) because some programs call ld.so's _dl_*
            // functions directly. The interpreter's symbols are
            // available via the native linker's symbol table.
        }
        // Try to open the interpreter. Check common host paths for
        // aarch64 dynamic linkers (musl and glibc multiarch).
        std::vector<std::string> interp_paths = {
            info.interp,                                    // exact path from PT_INTERP
            "/usr/aarch64-linux-gnu" + info.interp,         // Debian/Ubuntu multiarch
            "/usr/lib/aarch64-linux-gnu" + info.interp,     // newer Debian multiarch
        };
        FILE* ifile = nullptr;
        std::string found_path;
        for (auto& ip : interp_paths) {
            ifile = fopen(ip.c_str(), "rb");
            if (ifile) { found_path = ip; break; }
        }
        if (ifile) {
            fseek(ifile, 0, SEEK_END);
            long isz = ftell(ifile);
            fseek(ifile, 0, SEEK_SET);
            if (isz <= 0) { fclose(ifile); throw EmuError("empty or invalid interpreter"); }
            std::vector<uint8_t> idata(isz);
            if (fread(idata.data(), 1, isz, ifile) != static_cast<size_t>(isz)) {
                fclose(ifile);
                throw EmuError("short read on interpreter");
            }
            fclose(ifile);
            // Load the interpreter at a high address to avoid conflicts.
            // Use 0x4000000000 (above the 16GB direct window).
            interp_base = 0x4000000000ULL;
            interp_base_ = interp_base;  // Save for AT_BASE in auxv.
            if (idata.size() >= 64 && idata[0] == 0x7f && idata[1] == 'E') {
                uint64_t i_entry, i_phoff;
                uint16_t i_phnum, i_phentsize;
                memcpy(&i_entry, idata.data() + 24, 8);
                memcpy(&i_phoff, idata.data() + 32, 8);
                memcpy(&i_phentsize, idata.data() + 54, 2);
                memcpy(&i_phnum, idata.data() + 56, 2);
                for (int i = 0; i < i_phnum; i++) {
                    const uint8_t* pp = idata.data() + i_phoff + i * i_phentsize;
                    uint32_t p_type;
                    uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
                    memcpy(&p_type, pp + 0, 4);
                    memcpy(&p_offset, pp + 8, 8);
                    memcpy(&p_vaddr, pp + 16, 8);
                    memcpy(&p_filesz, pp + 32, 8);
                    memcpy(&p_memsz, pp + 40, 8);
                    if (p_type != 1) continue;  // PT_LOAD
                    uint64_t addr = interp_base + p_vaddr;
                    mem_.map_range(addr, p_memsz);
                    if (p_filesz > 0) {
                        mem_.write(addr, idata.data() + p_offset, p_filesz);
                    }
                    uint64_t end = addr + p_memsz;
                    if (end > end_addr_) end_addr_ = end;
                }
                // The entry point is the interpreter's entry (relative to interp_base).
                // Skip this if we used native dynlink — we run the main
                // binary's entry directly.
                if (!native_dynlink || !dyn_linker_) {
                    entry_ = interp_base + i_entry;
                }
            }
        }
    }
    // Static binaries have no PT_INTERP, so the block above never runs —
    // but GraphicThunk / runtime dlopen (syscalls 0x1002/0x1003) still
    // need a DynamicLinker + thunk trampolines. Create a minimal one.
    if (!dyn_linker_) {
        dyn_linker_ = std::make_unique<DynamicLinker>(mem_);
        if (auto* thunk = graphics_.thunk()) {
            if (thunk->enabled()) thunk->init(mem_);
        }
        if (auto* athunk = graphics_.audio_thunk()) {
            if (athunk->enabled()) athunk->init(mem_);
        }
        if (auto* dthunk = graphics_.display_thunk()) {
            if (dthunk->enabled()) dthunk->init(mem_);
        }
        GraphicThunk* gthunk = graphics_.thunk();
        AudioThunk*   athunk = graphics_.audio_thunk();
        DisplayThunk* dthunk = graphics_.display_thunk();
        dyn_linker_->set_thunk_resolver(
            [gthunk, athunk, dthunk](const std::string& lib)
                -> DynamicLinker::ThunkSymbolList {
                DynamicLinker::ThunkSymbolList out;
                auto add_all = [&](auto* t) {
                    if (!t || !t->enabled()) return;
                    t->enumerate_symbols(lib,
                        [&](const std::string& sym, uint64_t addr) {
                            out.emplace_back(sym, addr);
                        });
                };
                add_all(gthunk);
                add_all(athunk);
                add_all(dthunk);
                return out;
            });
    }
    // brk starts just above the loaded image, page-aligned up
    brk_ = (info.end_addr + 0xFFF) & ~0xFFFULL;
    brk_start_ = brk_;
    // Set up the initial stack image. 1.5.2: STACK_TOP is inside the
    // 4 GiB direct window so stack accesses hit the JIT fast path.
    const uint64_t STACK_TOP = Memory::STACK_TOP;
    const uint64_t STACK_SIZE = Memory::STACK_SIZE;
    uint64_t stack_base = STACK_TOP - STACK_SIZE;
    mem_.map_range(stack_base, STACK_SIZE + 4096);  // +1 page guard at top
    // 1.5.2-alpha: load the embedded vDSO before building the stack so
    // AT_SYSINFO_EHDR can point to it. The vDSO provides
    // gettimeofday/clock_gettime/clock_getres/rt_sigreturn stubs.
    load_vdso();
    main_cpu_.sp = build_initial_stack(STACK_TOP, argv, info);
    // glibc's ld.so normally sets __environ during _dl_start_user; the
    // native dynlink path skips ld.so, so point __environ at the envp
    // array on the initial stack before the program's _start runs.
    if (dyn_linker_) dyn_linker_->set_guest_environ(guest_envp_addr_);
    // Pre-allocate a TLS scratch area and set TPIDR_EL0 to point into
    // its center. Many libc startup routines read TPIDR_EL0 before
    // __libc_setup_tls has set the real TCB. Pointing it to valid
    // zeroed memory prevents unmapped-read crashes.
    //
    // binaries, the dynamic linker already set TPIDR_EL0 to the static
    // TLS block (line ~507 above). Overwriting it here destroys libc's
    // TLS — libc reads errno, stdin/stdout/stderr FILE pointers, locale
    // pointers, etc. via TPIDR_EL0-relative loads. With the scratch
    // area, all those reads return 0, causing stdio functions to crash
    // (NULL vtable pointer → blr x16 with x16=0 → pc=0 → decode error).
    // The dynamic linker's TPIDR_EL0 is correct and must be preserved.
    //
    // Static binaries may still have a DynamicLinker for thunk/dlopen
    // support without a static TLS block — only skip scratch when the
    // linker actually installed a TLS TP.
    if (!dyn_linker_ || dyn_linker_->static_tls_size() == 0) {
        if (main_cpu_.tpidr_el0 == 0) {
            const uint64_t TLS_SCRATCH_SIZE = 65536;  // 64 KiB
            uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
            main_cpu_.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
            main_cpu_.tpidrro_el0 = main_cpu_.tpidr_el0;
        }
    }
    // Map the zero page so NULL dereferences return 0 instead of
    // crashing. libc code often has NULL checks that only work if
    // the load itself doesn't fault.
    mem_.map_range(0, 4096);
    main_cpu_.pc = entry_;
    main_cpu_.running = true;
    main_cpu_.tid = 1;  // main thread TID
    next_tid_ = 2;
    main_cpu_.set_tid_address_ptr = 0;
}
// ── Default guest environment ──────────────────────────────────────────
// Builds a minimal but realistic environment for the guest, propagating
// locale/timezone-related host env vars so programs like `toybox uptime`,
// `date`, `ls` display correct local time and language conventions.
//
// Propagated vars (if set on host):
//   TZ, LANG, LC_ALL, LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE,
//   LC_MONETARY, LC_MESSAGES, LC_PAPER, LC_NAME, LC_ADDRESS,
//   LC_TELEPHONE, LC_MEASUREMENT, LC_IDENTIFICATION,
//   LESSCHARSET, LESSUTFCHARDEF, COLORTERM, COLORFGBG
//
// NOT propagated (sandbox/security):
//   LD_PRELOAD, LD_LIBRARY_PATH (would break the emulated loader),
//   BIFROST_* (emulator-internal flags must not leak to guest).
std::vector<std::string> Emulator::build_default_guest_env() {
    std::vector<std::string> envs;
    // Core environment every guest needs.
    envs.push_back("PATH=/tmp/aarch64-bin:/bin:/usr/bin:/sbin:/usr/sbin");
    envs.push_back("HOME=/root");
    envs.push_back("SHELL=/bin/sh");
    envs.push_back("TERM=linux");
    envs.push_back("PWD=/");
    envs.push_back("SHLVL=1");
    envs.push_back("_=/bin/sh");
    // Propagate locale + timezone vars from the host so locale-aware
    // programs (date, uptime, ls -l's month names, etc.) work correctly.
    // Without TZ, musl defaults to UTC and `toybox uptime` shows UTC time
    // instead of the user's local time.
    static const char* const propagate[] = {
        "TZ",
        "LANG",
        "LC_ALL",
        "LC_CTYPE",
        "LC_NUMERIC",
        "LC_TIME",
        "LC_COLLATE",
        "LC_MONETARY",
        "LC_MESSAGES",
        "LC_PAPER",
        "LC_NAME",
        "LC_ADDRESS",
        "LC_TELEPHONE",
        "LC_MEASUREMENT",
        "LC_IDENTIFICATION",
        "LESSCHARSET",
        "LESSUTFCHARDEF",
        "COLORTERM",
        "COLORFGBG",
        "PAGER",
        "EDITOR",
        "VISUAL",
        // GUI/graphics-adjacent vars. Propagating DISPLAY lets guest X11/Qt
        // (xcb) clients reach a host X server; the QT_* vars select the Qt
        // platform plugin and its search path (offscreen/xcb/eglfs).
        "DISPLAY",
        "XAUTHORITY",
        // Propagate the host D-Bus session so guest Qt apps can reach the
        // session bus. Without this, Qt's AT-SPI accessibility bridge
        // (activated during QWidget::show) cannot find the bus and blocks
        // forever on its connection-ready semaphore.
        "DBUS_SESSION_BUS_ADDRESS",
        "XDG_RUNTIME_DIR",
        "QT_QPA_PLATFORM",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QT_PLUGIN_PATH",
        "QT_QPA_FONTDIR",
        "QT_DEBUG_PLUGINS",
        "QT_X11_NO_MITSHM",
        "QT_ACCESSIBILITY",
        "QT_LINUX_ACCESSIBILITY_ALWAYS_ON",
        nullptr,
    };
    for (size_t i = 0; propagate[i]; ++i) {
        const char* v = getenv(propagate[i]);
        if (v && v[0] != '\0') {
            envs.push_back(std::string(propagate[i]) + "=" + v);
        }
    }
    return envs;
}
// ── vDSO loader (1.5.2-alpha) ───────────────────────────────────────
// Maps the embedded AArch64 vDSO ELF into guest memory and returns the
// base address (the ELF header location = AT_SYSINFO_EHDR). The vDSO
// provides gettimeofday/clock_gettime/clock_getres/__kernel_rt_sigreturn
// stubs that trap to the emulator's syscall handler via svc #0.
uint64_t Emulator::load_vdso() {
    if (vdso_base_ != 0) return vdso_base_;  // already loaded
    const uint8_t* data = vdso_so_bytes;
    size_t size = vdso_so_len;
    if (size < 64) return 0;
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return 0;
    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_phoff, data + 32, 8);
    memcpy(&e_phentsize, data + 54, 2);
    memcpy(&e_phnum, data + 56, 2);
    if (e_phoff == 0 || e_phnum == 0 || e_phentsize < 56) return 0;
    uint64_t max_vaddr_end = 0;
    struct Phdr { uint32_t p_type; uint32_t p_flags; uint64_t p_offset,
        p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };
    std::vector<Phdr> loads;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + i * e_phentsize + 56 > size) break;
        Phdr h;
        memcpy(&h, data + e_phoff + i * e_phentsize, 56);
        if (h.p_type == 1) {  // PT_LOAD
            loads.push_back(h);
            uint64_t end = h.p_vaddr + h.p_memsz;
            if (end > max_vaddr_end) max_vaddr_end = end;
        }
    }
    if (loads.empty() || max_vaddr_end == 0) return 0;
    max_vaddr_end = (max_vaddr_end + 0xFFF) & ~0xFFFULL;
    uint64_t base = mem_.mmap_alloc(max_vaddr_end);
    if (base == 0) return 0;
    size_t hdr_size = std::min<size_t>(e_phoff + e_phnum * e_phentsize, size);
    try {
        mem_.write(base, data, hdr_size);
    } catch (...) { return 0; }
    for (const auto& h : loads) {
        if (h.p_filesz == 0) continue;
        if (h.p_offset + h.p_filesz > size) continue;
        try {
            mem_.write(base + h.p_vaddr, data + h.p_offset,
                       static_cast<size_t>(h.p_filesz));
        } catch (...) { return 0; }
    }
    vdso_base_ = base;
    vdso_size_ = max_vaddr_end;
    if (getenv("BIFROST_DYNLINK_TRACE")) {
        fprintf(stderr, "[vdso] loaded at 0x%llx (size=%llu, %zu segments)\n",
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(max_vaddr_end),
                loads.size());
    }
    return base;
}
// ── Initial stack: argc, argv[], NULL, envp[], NULL, auxv[], NULL ─────
uint64_t Emulator::build_initial_stack(uint64_t stack_top,
                                       std::vector<std::string>& argv,
                                       ElfLoader::Loaded& info) {
    (void)info;  // reserved for future use (AT_PHDR, etc.)
    uint64_t sp = stack_top;
    // Push argv strings
    std::vector<uint64_t> argv_addrs;
    for (auto& a : argv) {
        sp -= a.size() + 1;
        mem_.write(sp, a.data(), a.size() + 1);
        argv_addrs.push_back(sp);
    }
    // Push envp. If guest_env_ was set via set_guest_env(), use it.
    // Otherwise, build a default environment that propagates locale/
    // timezone-related host env vars (TZ, LANG, LC_*, etc.) so programs
    // like `toybox uptime`, `date`, `ls` display correct local time and
    // language conventions.
    if (guest_env_.empty()) {
        guest_env_ = build_default_guest_env();
    }
    std::vector<uint64_t> envp_addrs;
    for (const auto& e : guest_env_) {
        sp -= e.size() + 1;
        mem_.write(sp, e.data(), e.size() + 1);
        envp_addrs.push_back(sp);
    }
    // AT_RANDOM — 16 random bytes used by glibc for stack canary init and
    // pointer guard. MUST be high-quality random; never fall back to rand()
    // (which is unseeded by default → predictable canary → stack-overflow
    // exploits in the guest become trivial).
    sp -= 16;
    uint8_t rnd[16];
    bool got_random = false;
    // Prefer the host kernel's getrandom(2) (no fd needed, never blocks
    // after boot, doesn't fail under seccomp unless explicitly blocked).
#ifdef SYS_getrandom
    long gr = ::syscall(SYS_getrandom, rnd, 16, 0);
    if (gr == 16) {
        got_random = true;
    }
#endif
    if (!got_random) {
        FILE* ur = fopen("/dev/urandom", "rb");
        if (ur) {
            size_t nread = fread(rnd, 1, 16, ur);
            fclose(ur);
            if (nread == 16) got_random = true;
        }
    }
    if (!got_random) {
        // Last-resort fallback: mix a stack address (ASLR'd) with the
        // monotonic clock. Better than rand() but still not great.
        uint64_t mix = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rnd))
                     ^ static_cast<uint64_t>(std::chrono::steady_clock::now()
                                              .time_since_epoch().count());
        for (int i = 0; i < 16; i += 8) {
            uint64_t v = mix;
            // Simple LCG mixing
            v = v * 6364136223846793005ULL + 1442695040888963407ULL;
            mix ^= v;
            memcpy(rnd + i, &mix, 8);
        }
    }
    mem_.write(sp, rnd, 16);
    uint64_t random_addr = sp;
    // AT_HWCAP bits for AArch64:
    //   bit 0: FP, bit 1: ASIMD, bit 7: CRC32, bit 8: LSE atomics.
    // We advertise FP + ASIMD + CRC32 + LSE atomics (glibc needs these).
    // We do NOT advertise BTI or PAC/PTRAUTH.
    const uint64_t HWCAP_FP      = 1ULL << 0;
    const uint64_t HWCAP_ASIMD   = 1ULL << 1;
    const uint64_t HWCAP_CRC32   = 1ULL << 7;
    const uint64_t HWCAP_ATOMICS = 1ULL << 8;
    uint64_t hwcap = HWCAP_FP | HWCAP_ASIMD | HWCAP_CRC32 | HWCAP_ATOMICS;
    // AT_EXECFN: pointer to the program name string on the stack
    uint64_t execfn_addr = argv_addrs[0];
    // AT_PLATFORM: aarch64 string (some libc ifunc resolvers consult it).
    sp -= 8;
    const char platform_str[] = "aarch64";
    mem_.write(sp, platform_str, sizeof(platform_str));
    uint64_t platform_addr = sp;
    std::vector<uint64_t> auxv = {
        6,  4096,            // AT_PAGESZ
        3,  phdr_addr_,      // AT_PHDR
        4,  phent_,          // AT_PHENT
        5,  phnum_,          // AT_PHNUM
        9,  prog_entry_,     // AT_ENTRY (original binary's entry, not interp's)
        25, random_addr,     // AT_RANDOM
        16, hwcap,           // AT_HWCAP
        26, 0,               // AT_HWCAP2 (no BTI, no PAC)
        23, 0,               // AT_SECURE (not setuid)
        31, execfn_addr,     // AT_EXECFN (program name)
        7,  interp_base_,    // AT_BASE (interpreter load address, 0 if static)
        33, vdso_base_,     // AT_SYSINFO_EHDR (vDSO ELF header address)
        // BUGFIX: AT_MINSIGSTKSZ was 0, which glibc uses to size altstacks.
        // A zero value can cause glibc to allocate an undersized altstack
        // and overflow into unmapped memory in signal handlers. The kernel
        // reports ~6 KiB on AArch64; we use the same value.
        51, 6144,            // AT_MINSIGSTKSZ
        // BUGFIX: missing AT_UID/AT_EUID/AT_GID/AT_EGID/AT_PLATFORM/AT_CLKTCK.
        // glibc reads these in __libc_start_main / _dl_aux_init; musl reads
        // AT_UID/AT_EUID to set the getuid/geteuid caches. AT_CLKTCK backs
        // sysconf(_SC_CLK_TCK); without it, sysconf returns -1 and tools
        // like top/ps miscompute CPU%. We return 0 for UID/GID (sandbox
        // is single-user root) and 100 for CLKTCK (the standard value).
        11, 0,               // AT_UID
        12, 0,               // AT_EUID
        13, 0,               // AT_GID
        14, 0,               // AT_EGID
        15, platform_addr,   // AT_PLATFORM ("aarch64")
        17, 100,             // AT_CLKTCK (sysconf(_SC_CLK_TCK))
        0,  0,               // AT_NULL
    };
    // Compute total table size and align SP to 16
    uint64_t argc = argv.size();
    uint64_t table_size = 8                            // argc
                        + 8 * (argc + 1)               // argv[]
                        + 8 * (envp_addrs.size() + 1)  // envp[]
                        + 8 * auxv.size();             // auxv
    sp -= table_size;
    sp &= ~0xFULL;  // 16-byte align
    uint64_t p = sp;
    auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
    push(argc);
    for (auto a : argv_addrs) push(a);
    push(0);
    // Record the guest address of the envp array so the native dynlink
    // path can point glibc's __environ at it (ld.so never runs to do it).
    guest_envp_addr_ = p;
    for (auto e : envp_addrs) push(e);
    push(0);
    for (auto v : auxv) push(v);
    return sp;
}
// ── Run loop ──────────────────────────────────────────────────────────
int Emulator::run() {
    uint64_t count = 0;
    auto t0 = std::chrono::steady_clock::now();
    run_start_time_ = t0;
    // ── Hang watchdog ───────────────────────────────────────────────
    // Detects infinite loops where the same PC is executed over and
    // over without making progress (a common symptom of mallocng init
    // recursion, softfloat loops, or atomic-CAS loops where STXR
    // always fails).
    // Also detects tight multi-PC loops (e.g., a 4-instruction cycle
    // that never exits) by tracking the set of recently-seen PCs.
    constexpr uint64_t HANG_LIMIT = 50'000'000;  // ~50M instructions
    constexpr uint64_t LOOP_DETECT_WINDOW = 16;  // track last 16 PCs
    constexpr uint64_t LOOP_DETECT_LIMIT = 5'000'000;  // 5M in tight loop
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;
    // Tight-loop detection: track PCs in a small ring buffer. If we
    // see the same set of PCs repeat for too long without a syscall,
    // it's a tight loop.
    uint64_t recent_pcs[LOOP_DETECT_WINDOW] = {};
    uint64_t pc_index = 0;
    uint64_t tight_loop_count = 0;
    // BUGFIX: the old watchdog sampled ONLY x2 as the progress
    // indicator. x2 is the third argument register and is routinely
    // unchanged in legitimate tight compute loops (e.g., a hot inner
    // loop using x0/x1 only). The new scheme samples a hash of x0+x1+x2
    //+x3+x19+x20+x21+x22+x23+x24+x25+x26+x27+x28 + sp + pc. Any of
    // those changing means the loop is making progress. This eliminates
    // false positives while still catching true linked-list-cycle /
    // infinite-spin bugs.
    uint64_t last_progress_hash = 0;
    while (main_cpu_.running) {
        try {
            // Optional PC trace for debugging. Gated by env var so
            // it's a no-op in production. Set BIFROST_TRACE_PC=1 to
            // log every instruction's PC + first 16 register values.
            // Use BIFROST_TRACE_PC_MAX=N to cap the trace at N
            // instructions (default 10000).
            static const bool trace_pc_ = (getenv("BIFROST_TRACE_PC") != nullptr);
            static const uint64_t trace_pc_max_ =
                getenv("BIFROST_TRACE_PC_MAX") ?
                    strtoull(getenv("BIFROST_TRACE_PC_MAX"), nullptr, 10) : 10000ULL;
            static uint64_t trace_pc_count_ = 0;
            if (trace_pc_ && trace_pc_count_ < trace_pc_max_) {
                fprintf(stderr, "[pc] 0x%llx sp=0x%llx x0=%llx x1=%llx x2=%llx x8=%llx x19=%llx x30=%llx\n",
                        (unsigned long long)main_cpu_.pc,
                        (unsigned long long)main_cpu_.sp,
                        (unsigned long long)main_cpu_.regs[0],
                        (unsigned long long)main_cpu_.regs[1],
                        (unsigned long long)main_cpu_.regs[2],
                        (unsigned long long)main_cpu_.regs[8],
                        (unsigned long long)main_cpu_.regs[19],
                        (unsigned long long)main_cpu_.regs[30]);
                trace_pc_count_++;
            }
            // JIT warmup threshold: use the interpreter for the first
            // `jit_threshold_` instructions, then switch to JIT. This
            // avoids JIT compilation overhead for short programs.
            bool use_jit_now = jit_enabled_ && jit_ &&
                               (jit_threshold_ == 0 || interp_count_ >= jit_threshold_);
            if (use_jit_now) {
                // JIT dispatch — defined in src/jit/jit_glue.cpp so the
                // FrostJIT definition is available. Falls back to
                // step() for any instruction it can't handle.
                jit_step(main_cpu_);
            } else {
                step(main_cpu_);
                interp_count_++;
            }
        } catch (UnmappedMemory& e) {
            // If the guest has installed a SIGSEGV handler, deliver the
            // signal with the fault address and si_code (MAPERR for read,
            // ACCERR for write) and continue. Otherwise, stop emulation.
            int si_code = e.write ? SEGV_ACCERR_EMU : SEGV_MAPERR_EMU;
            if (deliver_signal(*this, main_cpu_, signals_, BIFROST_SIGSEGV,
                               si_code, e.addr)) {
                count++;
                continue;
            }
            break;
        } catch (DecodeError& e) {
            // pointer calls and genuinely illegal instructions.
            //
            // On real ARM64 Linux:
            //   - Jumping to NULL (pc=0) or into the zero page (0x0-0xFFF)
            //     delivers SIGSEGV with si_code=SEGV_MAPERR (address not
            //     mapped for execution). The zero page is normally not
            //     mapped on Linux (vm.mmap_min_addr >= 4096).
            //   - Encountering an illegal/undefined instruction delivers
            //     SIGILL with si_code=ILL_ILLOPC.
            //
            // The emulator maps the zero page (mem_.map_range(0, 4096))
            // so NULL dereferences return 0 instead of faulting. This
            // means instruction fetch at pc=0 reads 0x00000000, which
            // is UDF #0 — the decoder throws DecodeError.
            //
            // Previously, DecodeError propagated to main() and killed
            // the emulator with "bifrost-emu: decode error at pc=0x0".
            // signal handler can recover) or terminate with the correct
            // exit code (128+11=139), not an emulator crash.
            //
            // pc=0 means a NULL function pointer was called (e.g., an
            // unresolved weak symbol or ifunc). pc in [1, 4095] means
            // a jump into the zero page. Both are SIGSEGV.
            uint64_t fault_pc = main_cpu_.pc;
            if (fault_pc < 4096) {
                // NULL pointer execution / jump into zero page → SIGSEGV
                if (deliver_signal(*this, main_cpu_, signals_,
                                   BIFROST_SIGSEGV, SEGV_MAPERR_EMU,
                                   fault_pc)) {
                    count++;
                    continue;
                }
                // No handler — default disposition terminates with SIGSEGV
                // (exit code 128+11=139). deliver_signal already set
                // cpu.running=false and cpu.exit_code=139.
                break;
            }
            // Genuinely illegal instruction at a valid PC → SIGILL.
            // This lets JITs and dynamic code generators that use SIGILL
            // for trap-on-overflow patterns to work.
            if (deliver_signal(*this, main_cpu_, signals_,
                               BIFROST_SIGILL, ILL_ILLOPC_EMU, fault_pc)) {
                count++;
                continue;
            }
            // No SIGILL handler — terminate with SIGILL (exit 128+4=132).
            break;
        }
        count++;
        // Periodic reporter (BIFROST_STATS_PERIOD=seconds). Prints real
        // guest throughput + block-structure + SIGPROF buckets every period
        // so steady-state AND phase-local behavior (startup/worldgen/load)
        // can be measured without a clean guest exit. Inert unless set.
        {
            static const double period_ = []() {
                const char* s = getenv("BIFROST_STATS_PERIOD");
                return s ? atof(s) : 0.0;
            }();
            if (period_ > 0.0) {
                static auto last_t_ = std::chrono::steady_clock::now();
                auto now_t = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now_t - last_t_).count();
                if (dt >= period_) {
                    dump_periodic_stats(dt);
                    dump_prof_snapshot();
                    last_t_ = now_t;
                }
            }
        }
        // Watchdog: if PC hasn't changed, increment same_pc_count.
        if (main_cpu_.pc == last_pc) {
            same_pc_count++;
            if (same_pc_count > HANG_LIMIT) {
                fprintf(stderr,
                    "[%s] hang watchdog: PC=0x%llx executed %llu times "
                    "without progress; aborting (likely mallocng init "
                    "recursion or atomic loop)\n",
                    CODENAME,
                    static_cast<unsigned long long>(main_cpu_.pc),
                    static_cast<unsigned long long>(same_pc_count));
                main_cpu_.running = false;
                main_cpu_.exit_code = 70;  // EX_SOFTWARE
                break;
            }
        } else {
            last_pc = main_cpu_.pc;
            same_pc_count = 0;
        }
        // Tight-loop detection: track recent PCs in a ring buffer.
        // If we've been cycling through a small set of PCs for too long
        // without hitting a syscall AND registers aren't changing, it's
        // likely a bug-induced tight loop. We sample a hash of multiple
        // registers as a progress indicator — if any of them changes,
        // the loop is making progress (e.g., a tight compute loop
        // working on x0/x1 only). Only trigger if registers are frozen
        // (true bug). Sample every 256 instructions to reduce overhead.
        if ((count & 0xFF) == 0) {
            recent_pcs[pc_index % LOOP_DETECT_WINDOW] = main_cpu_.pc;
            pc_index++;
            // Check if all recent PCs are the same small set (≤4 unique)
            // AND the progress register hasn't changed.
            if ((pc_index % LOOP_DETECT_WINDOW) == 0) {
                std::set<uint64_t> unique_pcs(recent_pcs, recent_pcs + LOOP_DETECT_WINDOW);
                if (unique_pcs.size() <= 4) {
                    // Compute a progress hash from the most volatile
                    // registers. We pick x0-x3 (argument registers) and
                    // x19-x28 (callee-saved scratch) plus sp and pc —
                    // any of those changing means the loop is making
                    // progress. The old code only checked x2, which is
                    // the third argument register and routinely
                    // unchanged in legitimate tight compute loops.
                    uint64_t current_hash =
                          main_cpu_.regs[0] ^ main_cpu_.regs[1]
                        ^ main_cpu_.regs[2] ^ main_cpu_.regs[3]
                        ^ main_cpu_.regs[19] ^ main_cpu_.regs[20]
                        ^ main_cpu_.regs[21] ^ main_cpu_.regs[22]
                        ^ main_cpu_.regs[23] ^ main_cpu_.regs[24]
                        ^ main_cpu_.regs[25] ^ main_cpu_.regs[26]
                        ^ main_cpu_.regs[27] ^ main_cpu_.regs[28]
                        ^ main_cpu_.sp ^ main_cpu_.pc;
                    if (current_hash == last_progress_hash) {
                        tight_loop_count += LOOP_DETECT_WINDOW * 16;
                        if (tight_loop_count > LOOP_DETECT_LIMIT) {
                            fprintf(stderr,
                                "[%s] tight-loop watchdog: %zu unique PCs, "
                                "registers frozen for %llu instructions; aborting "
                                "(likely linked-list cycle or similar bug)\n",
                                CODENAME, unique_pcs.size(),
                                static_cast<unsigned long long>(tight_loop_count));
                            main_cpu_.running = false;
                            main_cpu_.exit_code = 70;
                            break;
                        }
                    } else {
                        tight_loop_count = 0;
                    }
                    last_progress_hash = current_hash;
                } else {
                    tight_loop_count = 0;
                }
            }
        }
        // Drain the host-signal queue every ~4K instructions.
        if ((count & 0xFFF) == 0) {
            drain_host_signals(main_cpu_);
            // Also drain per-CPU pending signals (queued by cross-thread
            // tgkill/tkill/kill). This is the fix for the cross-thread
            // CPU-mutation race.
            drain_pending_signals(main_cpu_);
            // rusage/times. Add the ~4K instructions since last drain to
            // the total. (Not exact — we add 4096 each time, but close
            // enough for CPU time estimation.)
            guest_instructions_total_.fetch_add(4096, std::memory_order_relaxed);
        }
        // so guest_instr_to_seconds() has a real conversion factor.
        if ((count & 0xFFFFF) == 0 && count > 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - t0).count();
            if (elapsed > 0.001) {  // avoid div-by-zero
                double mips = static_cast<double>(count) / elapsed / 1e6;
                mips_estimate_.store(mips, std::memory_order_relaxed);
            }
        }
        if ((count & 0xFFFFF) == 0) {
            if (!mem_.is_mapped(main_cpu_.pc, 4)) {
                throw EmuError("PC ran into unmapped memory at 0x"
                    + to_hex(main_cpu_.pc));
            }
            // SDL2 real-time refresh (~1M instructions ≈ 6 fps at 6 MIPS).
            if (graphics_.ready() && graphics_.guest_fb_addr() != 0) {
                uint64_t gaddr = graphics_.guest_fb_addr();
                std::vector<uint8_t> buf(graphics_.size());
                try {
                    mem_.read(gaddr, buf.data(), buf.size());
                    graphics_.sync_from(buf.data());
                    graphics_.refresh();
                    if (!graphics_.poll_events()) {
                        main_cpu_.running = false;
                        main_cpu_.exit_code = 0;
                        break;
                    }
                } catch (const std::exception&) {
                    // guest fb address no longer mapped — skip
                }
            }
        }
    }
    // Wait for any spawned threads to exit
    join_threads();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    if (verbose_) {
        fprintf(stderr, "[%s] executed %llu instructions in %.3fs (%.2f MIPS)\n",
                CODENAME, static_cast<unsigned long long>(count), secs, count / 1e6 / secs);
        fprintf(stderr, "[%s] mem pages: %zu (%.1f MB)\n",
                CODENAME, mem_.page_count(),
                mem_.page_count() * 4096 / 1048576.0);
        uint64_t hits = main_cpu_.decode_cache_hits;
        uint64_t misses = main_cpu_.decode_cache_misses;
        {
            std::lock_guard<std::mutex> g(threads_mu_);
            for (auto& gt : threads_) {
                hits   += gt->cpu.decode_cache_hits;
                misses += gt->cpu.decode_cache_misses;
            }
        }
        uint64_t total = hits + misses;
        if (total > 0) {
            fprintf(stderr, "[%s] decode cache: %llu hits, %llu misses (%.1f%% hit rate)\n",
                    CODENAME, static_cast<unsigned long long>(hits),
                    static_cast<unsigned long long>(misses),
                    100.0 * hits / total);
        }
        if (jit_ && jit_enabled_) {
            print_jit_stats();
        }
    }
    return main_cpu_.exit_code;
}
// ── Per-instruction step (decode cache + trace) ───────────────────────
void Emulator::step(CPU& cpu) {
    // BIFROST_PROF sampling-flag toggle (see jit_glue.cpp): the
    // SIGPROF handler buckets time spent in the interpreter vs JIT.
    extern thread_local bool prof_in_interp;
    extern bool bifrost_prof_active();
    const bool prof_active = bifrost_prof_active();
    if (prof_active) prof_in_interp = true;
    if (trace_) {
        fprintf(stderr,
            "[trace tid=%d] pc=0x%08llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
            "x4=0x%llx x5=0x%llx x8=0x%llx x16=0x%llx x17=0x%llx x19=0x%llx "
            "x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx x25=0x%llx "
            "x26=0x%llx x27=0x%llx x28=0x%llx x29=0x%llx x30=0x%llx pstate=0x%x "
            "sp=0x%llx v0lo=0x%llx v0hi=0x%llx v1lo=0x%llx v1hi=0x%llx v2lo=0x%llx v2hi=0x%llx\n",
            cpu.tid,
            static_cast<unsigned long long>(cpu.pc),
            static_cast<unsigned long long>(cpu.regs[0]), static_cast<unsigned long long>(cpu.regs[1]),
            static_cast<unsigned long long>(cpu.regs[2]), static_cast<unsigned long long>(cpu.regs[3]),
            static_cast<unsigned long long>(cpu.regs[4]), static_cast<unsigned long long>(cpu.regs[5]),
            static_cast<unsigned long long>(cpu.regs[8]), static_cast<unsigned long long>(cpu.regs[16]),
            static_cast<unsigned long long>(cpu.regs[17]), static_cast<unsigned long long>(cpu.regs[19]),
            static_cast<unsigned long long>(cpu.regs[20]), static_cast<unsigned long long>(cpu.regs[21]),
            static_cast<unsigned long long>(cpu.regs[22]), static_cast<unsigned long long>(cpu.regs[23]),
            static_cast<unsigned long long>(cpu.regs[24]), static_cast<unsigned long long>(cpu.regs[25]),
            static_cast<unsigned long long>(cpu.regs[26]), static_cast<unsigned long long>(cpu.regs[27]),
            static_cast<unsigned long long>(cpu.regs[28]), static_cast<unsigned long long>(cpu.regs[29]),
            static_cast<unsigned long long>(cpu.regs[30]), cpu.pstate,
            static_cast<unsigned long long>(cpu.sp),
            static_cast<unsigned long long>(cpu.v_lo[0]), static_cast<unsigned long long>(cpu.v_hi[0]),
            static_cast<unsigned long long>(cpu.v_lo[1]), static_cast<unsigned long long>(cpu.v_hi[1]),
            static_cast<unsigned long long>(cpu.v_lo[2]), static_cast<unsigned long long>(cpu.v_hi[2]));
    }
    uint32_t inst = mem_.fetch_inst(cpu.pc, &cpu.page_cache);
    uint64_t next_pc = cpu.pc + 4;
    execute(inst, next_pc, cpu);
    cpu.pc = next_pc;
    if (prof_active) prof_in_interp = false;
}
// Host-to-guest signal forwarding (install_host_signal_handlers,
// queue_host_signal, host_signal_handler, drain_host_signals) is
// implemented in src/core/signal.cpp — see that file for the full
// disposition table.
} // namespace arm64emu
