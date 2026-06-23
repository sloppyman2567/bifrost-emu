// core/emulator.h — Emulator class definition (private header).
//
// The Emulator ties everything together: it owns a Memory, a pool of vCPUs
// (one main + N cloned via clone()), a futex table, signal table, graphics
// backend, and (optionally) a FrostJIT instance.
//
// Two headers expose Emulator:
//   - include/bifrost/emulator.hpp   — public API surface (lifecycle + run)
//   - src/core/emulator.h            — full class definition (private members)
//
// Translation units inside src/ include this private header. External
// consumers (libbifrost users, the JIT glue layer) include the public one.
#pragma once

#include "bifrost/types.hpp"
#include "bifrost/version.hpp"
#include "core/cpu.h"
#include "core/memory.h"
#include "core/signal.h"
#include "graphics.hpp"
#include "audio/audio.h"
#include "vfs/vfs.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward-declare FrostJIT (full definition in include/jit/frostjit.hpp).
// We only need a forward decl here because FrostJIT is held via unique_ptr.
namespace arm64emu { class FrostJIT; }

// Forward-declare syscall handlers (defined in src/syscalls/*.cpp) so
// they can be declared as friends of Emulator below.
namespace arm64emu {
    int64_t syscall_fs(Emulator&, CPU&, uint64_t);
    int64_t syscall_mem(Emulator&, CPU&, uint64_t);
    int64_t syscall_threads(Emulator&, CPU&, uint64_t);
    int64_t syscall_time(Emulator&, CPU&, uint64_t);
    int64_t syscall_ioctls(Emulator&, CPU&, uint64_t);
    int64_t syscall_misc(Emulator&, CPU&, uint64_t);
}

namespace arm64emu {

// ── ELF loader (definition lives in src/frontend/elf_loader.cpp) ──────
class ElfLoader {
public:
    struct Loaded {
        uint64_t entry;
        uint64_t phdr_addr;
        uint64_t phnum;
        uint64_t phent;
        uint64_t end_addr;   // highest mapped addr (for brk baseline)
        bool     has_lse;    // ELF declared AArch64 LSE atomic feature
        std::string interp;  // PT_INTERP path (dynamic linker), empty if static
    };

    static Loaded load(Memory& mem, const std::vector<uint8_t>& data);
};

// ── Emulator ──────────────────────────────────────────────────────────
class Emulator {
public:
    Emulator();
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    // ── Lifecycle (public) ────────────────────────────────────────────
    void load_elf_file(const std::string& path, std::vector<std::string>& argv);
    int  run();
    void step_public(CPU& cpu) { step(cpu); }
    void syscall_public(CPU& cpu) { syscall(cpu); }

    // ── Configuration (public) ────────────────────────────────────────
    void set_verbose(bool v) { verbose_ = v; }
    void set_trace(bool v)   { trace_  = v; }
    void set_brk_verbose(bool v) { brk_verbose_ = v; }
    void set_jit_enabled(bool v) { jit_enabled_ = v; }
    bool jit_enabled() const { return jit_enabled_; }

    // ── Accessors (public) ────────────────────────────────────────────
    Memory&         mem()      { return mem_; }
    GraphicsBackend& graphics() { return graphics_; }
    Audio&          audio()    { return audio_; }
    SignalTable&    signals()  { return signals_; }
    VFS&            vfs()      { return vfs_; }
    FdTable&        fds()      { return fds_; }
    const std::string& elf_path() const { return elf_path_; }
    CPU&            main_cpu_public() { return main_cpu_; }
    FrostJIT*       jit() { return jit_.get(); }
    void            enable_jit();

    // Defined in src/jit/jit_glue.cpp so the FrostJIT definition is visible.
    void jit_step(CPU& cpu);
    void print_jit_stats();

    void install_host_signal_handlers_public() { install_host_signal_handlers(); }

    // ── vCPU management ───────────────────────────────────────────────
    // Futex table: maps a guest address → (mutex, condvar, waiter count).
    // Used by clone-spawned threads for synchronization.
    struct FutexSlot {
        std::mutex mu;
        std::condition_variable cv;
        int waiters = 0;
    };
    FutexSlot* get_futex(uint64_t addr) {
        std::lock_guard<std::mutex> g(futex_table_mu_);
        return &futex_table_[addr];
    }
    void decrement_alive_threads() { alive_threads_.fetch_sub(1); }

    // Per-thread lookup (for tgkill, etc.)
    CPU* find_cpu_by_tid(int tid);

private:
    // ── Owned state ───────────────────────────────────────────────────
    Memory mem_;
    CPU    main_cpu_;          // main thread's CPU state
    uint64_t entry_ = 0;
    uint64_t end_addr_ = 0;
    uint64_t brk_ = 0;
    uint64_t brk_start_ = 0;   // initial brk (set at ELF load time)
    std::mutex brk_mu_;        // serializes concurrent brk() calls
    uint64_t phdr_addr_ = 0;
    uint64_t phnum_ = 0;
    uint64_t phent_ = 0;
    uint64_t interp_base_ = 0;  // dynamic linker load address (0 if static)
    uint64_t prog_entry_ = 0;   // original program entry (for AT_ENTRY)
    bool     has_lse_ = false;  // ELF declared LSE feature; affects LDUR/LSE decode
    bool verbose_ = false;
    bool trace_ = false;
    bool brk_verbose_ = true;
    std::string elf_path_;

    // Decode cache type alias (constants + CacheEntry type live on CPU).
    using CacheEntry = CPU::CacheEntry;

    // ── Thread management (for clone()) ───────────────────────────────
    struct GuestThread {
        CPU cpu;
        std::thread host_thread;
        uint64_t stack_top = 0;
        uint64_t stack_size = 0;
        uint64_t tls_base = 0;
        int tid = 0;
    };
    std::vector<std::unique_ptr<GuestThread>> threads_;
    std::mutex threads_mu_;
    std::atomic<int> next_tid_{2};
    std::atomic<int> alive_threads_{0};

    // ── Futex table ───────────────────────────────────────────────────
    std::mutex futex_table_mu_;
    std::unordered_map<uint64_t, FutexSlot> futex_table_;

    // ── Graphics backend (virtual /dev/fb0) ───────────────────────────
    GraphicsBackend graphics_;

    // ── Audio backend (virtual /dev/dsp, /dev/snd) ────────────────────
    Audio audio_;

    // ── VFS + fd table ────────────────────────────────
    // Replaces the inline /proc//dev/ chains in syscalls.cpp.
    // VFS resolves guest paths to VNodes; FdTable maps guest fds to
    // VNodes. Owned by Emulator so all vCPUs share the same view.
    VFS vfs_;
    FdTable fds_;

    // ── Signal delivery ───────────────────────────────────────────────
    SignalTable signals_;

    // ── Host-to-guest signal forwarding ───────────────────────────────
    std::mutex host_signal_mu_;
    std::vector<int> host_signal_queue_;
    static Emulator* g_active_emu_;  // for host signal handler (single active emu)
    static void host_signal_handler(int signo);
    void install_host_signal_handlers();
    void queue_host_signal(int signo);
    bool drain_host_signals(CPU& cpu);

    // ── frostJIT ──────────────────────────────────────────────────────
    std::unique_ptr<FrostJIT> jit_;
    bool jit_enabled_ = false;

    // Friend declaration must come AFTER GuestThread is defined.
    friend void thread_entry(Emulator* emu, GuestThread* gt);

    // ── Syscall layer friends (so handlers can access private state) ──
    // Each handler lives in src/syscalls/{fs,mem,threads,time,ioctls}.cpp
    // and needs to read/write brk_, brk_mu_, phdr_addr_, etc. directly.
    friend int64_t syscall_fs(Emulator&, CPU&, uint64_t);
    friend int64_t syscall_mem(Emulator&, CPU&, uint64_t);
    friend int64_t syscall_threads(Emulator&, CPU&, uint64_t);
    friend int64_t syscall_time(Emulator&, CPU&, uint64_t);
    friend int64_t syscall_ioctls(Emulator&, CPU&, uint64_t);
    friend int64_t syscall_misc(Emulator&, CPU&, uint64_t);

    // ── Internal helpers ──────────────────────────────────────────────
    uint64_t build_initial_stack(uint64_t stack_top,
                                 std::vector<std::string>& argv,
                                 ElfLoader::Loaded& info);

    void step(CPU& cpu);
    void execute(uint32_t inst, uint64_t& next_pc, CPU& cpu);
    void syscall(CPU& cpu);

    int  spawn_thread(CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                      uint64_t entry_pc, uint64_t arg, uint64_t tls);
    void join_threads();

    static std::string to_hex(uint64_t v) {
        char b[32]; snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(v));
        return b;
    }
};

// Implemented in src/core/thread_mgr.cpp — friend of Emulator.
void thread_entry(Emulator* emu, Emulator::GuestThread* gt);

} // namespace arm64emu
