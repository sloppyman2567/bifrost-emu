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
#include "frost/graphics.hpp"
#include "audio/audio.h"
#include "yggdrasil/yggdrasil.hpp"

// Bring Yggdrasil and FdTable into the arm64emu namespace so existing
// callers (Emulator::vfs(), Emulator::fds(), syscall handlers) can refer
// to them unqualified. The yggdrasil:: prefix is still required in the
// .cpp files that #include the node headers directly.
namespace arm64emu {
    using yggdrasil::Yggdrasil;
    using yggdrasil::FdTable;
    using yggdrasil::Node;
}

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

// Forward-declare DynamicLinker (defined in src/frontend/dynamic_linker.h).
class DynamicLinker;

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
    // Execute one instruction on the given CPU. Used by the interpreter,
    // JIT CALL_INTERP fallback, IR executor, and thread manager.
    void step(CPU& cpu);
    // Execute a syscall on the given CPU (reads x8 for number, x0-x5 for
    // args, writes return to x0). Used by the IR executor for SVC.
    void syscall(CPU& cpu);
    // Drain any host-forwarded signals (SIGINT/SIGTERM/SIGCHLD) to the
    // guest. Called by spawned threads at syscall boundaries.
    bool drain_host_signals(CPU& cpu);
    // Install host signal handlers for forwarding to the guest.
    void install_host_signal_handlers();
    // Helper for blocking syscalls that returned -EINTR. Drains pending
    // host signals (so any guest signal handler runs before the guest
    // sees -EINTR) and always returns false (the caller should return
    // -EINTR to the guest). The guest's libc wrapper then decides
    // whether to retry or propagate the error.
    bool handle_eintr(CPU& cpu);

    // ── Configuration (public) ────────────────────────────────────────
    void set_verbose(bool v) { verbose_ = v; }
    void set_trace(bool v)   { trace_  = v; }
    void set_brk_verbose(bool v) { brk_verbose_ = v; }
    void set_jit_enabled(bool v) { jit_enabled_ = v; }
    bool jit_enabled() const { return jit_enabled_; }
    // Set the JIT warmup threshold: use the interpreter for the first
    // `n` instructions, then switch to JIT. This avoids JIT compilation
    // overhead for short programs (e.g., `toybox seq 1 10`). Set to 0
    // (default) to use the JIT from the start.
    void set_jit_threshold(uint64_t n) { jit_threshold_ = n; }
    uint64_t jit_threshold() const { return jit_threshold_; }

    // ── Accessors (public) ────────────────────────────────────────────
    Memory&         mem()      { return mem_; }
    FrostGraphics& graphics() { return graphics_; }
    Audio&          audio()    { return audio_; }
    SignalTable&    signals()  { return signals_; }
    Yggdrasil&      vfs()      { return vfs_; }
    FdTable&        fds()      { return fds_; }
    const std::string& elf_path() const { return elf_path_; }
    CPU&            main_cpu() { return main_cpu_; }
    FrostJIT*       jit() { return jit_.get(); }
    void            enable_jit();

    // Defined in src/jit/jit_glue.cpp so the FrostJIT definition is visible.
    void jit_step(CPU& cpu);
    void print_jit_stats();

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

    // Public accessor for JIT fast LL/SC helpers.
    struct ExclMonitorShardAccess {
        std::mutex mu;
        std::unordered_map<uint64_t, std::vector<CPU*>> reservations;
    };
    static size_t excl_shard_idx_pub(uint64_t addr) {
        return (addr >> 3) & 15;
    }
    void* excl_monitor_shard_pub(uint64_t addr);

    // ── Fork support (clone without CLONE_VM) ────────────────────────
    // Fork the guest: snapshot the current memory + CPU state and
    // create a child "process" that runs in a host thread with its own
    // Memory. The child gets a new TID (PID). The parent returns the
    // child's PID; the child returns 0.
    //
    // The child runs independently until it calls exit/exit_group, at
    // which point its exit code is stored in the fork_children_ table
    // for the parent's wait4() to retrieve.
    struct ForkChild {
        std::unique_ptr<Memory> mem;
        CPU cpu;
        int pid = 0;
        int exit_code = 0;
        bool exited = false;
        bool waited = false;
        std::thread host_thread;
        std::mutex mu;
        std::condition_variable cv;
    };
    int fork_guest(CPU& parent_cpu, uint64_t child_stack, uint64_t flags,
                   uint64_t ptid_ptr, uint64_t ctid_ptr, uint64_t tls);
    // Look up a forked child by PID. Returns nullptr if not found.
    // The caller should hold fork_children_mu_ while accessing the
    // returned pointer.
    ForkChild* find_fork_child(int pid);
    // Reap a forked child (called by wait4). Returns the exit code,
    // or -ECHILD if the PID doesn't exist.
    int reap_fork_child(int pid, int options, bool& found);

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
    bool brk_verbose_ = true; // Turn 40: always true
    std::string elf_path_;
    // Guest-side current working directory. Decoupled from the host cwd
    // because BIFROST_ROOT sandboxing remaps guest paths. Updated by
    // chdir/fchdir; returned by getcwd. Defaults to "/".
    std::string guest_cwd_ = "/";

    // Decode cache type alias (constants + CacheEntry type live on CPU).
    using CacheEntry = CPU::CacheEntry;

    // ── Thread management (for clone()) ───────────────────────────────
    struct GuestThread {
        CPU cpu;
        std::thread host_thread;
        // Per-thread FrostJIT instance. Each spawned thread gets its own
        // 64 MiB code cache + block cache + regalloc state, so JIT
        // execution is fully lock-free (no contention with other threads
        // or with the main thread's JIT). Translation work is duplicated
        // across threads, but the simplicity and lock-free execution
        // outweigh the memory cost for typical 1-8 thread guests.
        // nullptr if JIT is disabled (thread uses interpreter only).
        std::unique_ptr<FrostJIT> jit;
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

    // ── Global exclusive monitor (LL/SC atomics) ──────────────────────
    // AArch64's LDXR/STXR (load-linked / store-conditional) atomics rely
    // on a GLOBAL exclusive monitor that tracks all CPUs' reservations.
    // When any CPU stores to a reserved address, OTHER CPUs' reservations
    // for that address are invalidated. This is what makes LL/SC correct.
    //
    // The monitor is SHARDED into 16 stripes by address bits ((addr>>3)&0xF)
    // so independent atomics on different addresses proceed in parallel.
    // Two CPUs only contend if they map to the same stripe.
    static constexpr size_t EXCL_MONITOR_SHARDS = 16;
    struct ExclMonitorShard {
        std::mutex mu;
        // Map: address → set of CPU* with a reservation at that address.
        std::unordered_map<uint64_t, std::vector<CPU*>> reservations;
    };
    ExclMonitorShard excl_monitor_shards_[EXCL_MONITOR_SHARDS];

    static size_t excl_shard_idx(uint64_t addr) {
        return (addr >> 3) & (EXCL_MONITOR_SHARDS - 1);
    }
    // jit_ldxr/jit_stxr/jit_stlr are extern "C" functions defined in
    // x86_backend.cpp. They access excl_monitor_shards_ directly.
    // No friend declaration needed — the functions are in the global
    // namespace (extern "C") and access the shards via the public
    // excl_shard_idx + the shard array (both in the private section,
    // but extern "C" functions can't be friends in standard C++).
    // Instead, we provide a public accessor.

    // ── Fork children (clone without CLONE_VM) ───────────────────────
    std::mutex fork_children_mu_;
    std::vector<std::unique_ptr<ForkChild>> fork_children_;

    // ── Graphics backend (virtual /dev/fb0) ───────────────────────────
    FrostGraphics graphics_;

    // ── Audio backend (virtual /dev/dsp, /dev/snd) ────────────────────
    Audio audio_;

    // ── Yggdrasil VFS + fd table ────────────────────────────────────
    // Yggdrasil (the world-tree) resolves guest paths to Nodes; FdTable
    // maps guest fds to Nodes. Owned by Emulator so all vCPUs share the
    // same view. (v1.4.5-alpha: renamed from VFS/VNode to Yggdrasil/Node.)
    Yggdrasil vfs_;
    FdTable   fds_;

    // ── Signal delivery ───────────────────────────────────────────────
    SignalTable signals_;

    // ── Host-to-guest signal forwarding ───────────────────────────────
    // BUGFIX: the old code used std::mutex to protect host_signal_queue_.
    // std::mutex::lock is NOT async-signal-safe — calling it from a host
    // signal handler is undefined behavior (can deadlock if the main
    // thread holds the mutex when the signal arrives). We now use a
    // fixed-size lock-free SPSC ring buffer: the host signal handler
    // (producer) writes to tail with memory_order_release; the run loop
    // (consumer) reads from head with memory_order_acquire. No locks,
    // no UB.
    static constexpr size_t HOST_SIGNAL_QUEUE_CAP = 64;
    struct HostSignalQueue {
        std::atomic<size_t> head{0};  // consumer index
        std::atomic<size_t> tail{0};  // producer index
        int signals[HOST_SIGNAL_QUEUE_CAP];
    };
    HostSignalQueue host_signal_queue_;
    static Emulator* g_active_emu_;  // for host signal handler (single active emu)
    static void host_signal_handler(int signo);
    void queue_host_signal(int signo);

    // ── frostJIT ──────────────────────────────────────────────────────
    std::unique_ptr<FrostJIT> jit_;
    bool jit_enabled_ = false;
    uint64_t jit_threshold_ = 0;  // 0 = use JIT from start; N = interp for N instructions
    uint64_t interp_count_  = 0;  // instructions run under interpreter (for threshold)

    // ── Dynamic linker (for dynamically-linked binaries) ────────────
    // Owned via unique_ptr so we don't need the full DynamicLinker
    // definition in this header. Created on first load_elf_file() if
    // the binary has a PT_INTERP.
    std::unique_ptr<DynamicLinker> dyn_linker_;

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

    void execute(uint32_t inst, uint64_t& next_pc, CPU& cpu);

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
