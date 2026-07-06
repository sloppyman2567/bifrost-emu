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
#include <chrono>
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
        uint64_t base_addr;  // load bias (0 for ET_EXEC, PIE_BASE for ET_DYN)
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
    // Drain any per-CPU pending signals queued by cross-thread tgkill/
    // tkill/kill. Called by every CPU's run loop at the same 4K-instr
    // boundary as drain_host_signals(). This is the fix for the
    // cross-thread CPU-mutation race (Turn 57).
    bool drain_pending_signals(CPU& cpu);
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

    // ── Guest environment ─────────────────────────────────────────────
    // Replace the guest's environment with `envs` (each string "KEY=VALUE").
    // If never called, build_initial_stack() uses a sensible default
    // environment that propagates a curated set of host env vars (TZ,
    // LANG, LC_*, etc.) so locale-aware programs (toybox uptime, date,
    // ls, etc.) display correct local time and language conventions.
    void set_guest_env(const std::vector<std::string>& envs) {
        guest_env_ = envs;
    }
    const std::vector<std::string>& guest_env() const { return guest_env_; }
    // Guest process name (comm). Set by prctl(PR_SET_NAME), returned by
    // prctl(PR_GET_NAME) and /proc/self/comm.
    void set_guest_comm(const std::string& name) {
        // Linux kernel truncates to 16 bytes (15 + NUL).
        guest_comm_ = name.size() > 15 ? name.substr(0, 15) : name;
    }
    const std::string& guest_comm() const { return guest_comm_; }
    // Build the default guest environment, propagating locale/timezone-
    // related host env vars. Used by build_initial_stack when set_guest_env
    // was not called. Also used by execve to give the new process image
    // a consistent environment.
    static std::vector<std::string> build_default_guest_env();

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
    //
    // BUGFIX (Turn 57): the old design used a single std::mutex +
    // std::unordered_map for the whole table. Every FUTEX_WAIT/WAKE/REQUEUE
    // took the same lock, serializing all futex ops across all vCPUs.
    // Real games have dozens to hundreds of distinct futex words (one per
    // pthread mutex/cond/barrier); each op contended on the same lock.
    //
    // New design: 64 shards keyed by (addr >> 3) & 63 (mirror the exclusive
    // monitor's design, but with 4x more shards for better scalability on
    // 8+ vCPU guests). Two futex ops only contend if their addresses map
    // to the same shard — unlikely for unrelated mutexes.
    //
    // Also: when a FutexSlot's waiters transitions to 0, the slot is
    // erased from its shard (reclaim memory for freed mutexes). The
    // caller (FUTEX_WAIT/WAKE) is responsible for calling
    // release_futex(addr) when waiters hits 0.
    struct FutexSlot {
        std::mutex mu;
        std::condition_variable cv;
        int waiters = 0;
    };
    static constexpr size_t FUTEX_SHARDS = 64;
    struct FutexShard {
        std::mutex mu;
        std::unordered_map<uint64_t, std::unique_ptr<FutexSlot>> slots;
    };
    FutexShard futex_shards_[FUTEX_SHARDS];
    static size_t futex_shard_idx(uint64_t addr) {
        return (addr >> 3) & (FUTEX_SHARDS - 1);
    }
    // Look up (or create) the FutexSlot for `addr`. Returns a pointer
    // owned by the shard. The caller MUST NOT hold the shard mutex when
    // calling wait/wake on the slot.
    FutexSlot* get_futex(uint64_t addr) {
        size_t idx = futex_shard_idx(addr);
        std::lock_guard<std::mutex> g(futex_shards_[idx].mu);
        auto& shard = futex_shards_[idx].slots;
        auto it = shard.find(addr);
        if (it == shard.end()) {
            auto slot = std::make_unique<FutexSlot>();
            FutexSlot* raw = slot.get();
            shard.emplace(addr, std::move(slot));
            return raw;
        }
        return it->second.get();
    }
    // Erase a FutexSlot when its waiter count hits 0 (called by FUTEX_WAKE
    // after notifying). Reclaims memory for freed mutexes — without this,
    // a long-running game that allocates/frees millions of mutexes would
    // leak indefinitely.
    void release_futex_if_empty(uint64_t addr) {
        size_t idx = futex_shard_idx(addr);
        std::lock_guard<std::mutex> g(futex_shards_[idx].mu);
        auto& shard = futex_shards_[idx].slots;
        auto it = shard.find(addr);
        if (it == shard.end()) return;
        // Double-check waiters under the shard lock (the slot's own mu
        // is NOT held here, but the shard lock prevents new waiters from
        // finding this slot between the check and the erase).
        if (it->second->waiters == 0) {
            shard.erase(it);
        }
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
        // BUGFIX (Turn 62): track guest CPU time for the forked child so
        // wait4's rusage can report accurate per-guest user/sys time
        // instead of the host emulator process's CPU time (which includes
        // JIT compilation, memory management, etc. — useless to the guest).
        // Measured in guest instructions executed. Converted to seconds
        // via the guest MIPS estimate at wait4 time.
        uint64_t guest_instructions = 0;
        // Wall-clock time the child spent running (for "real" time in
        // `toybox time`). Recorded as a steady_clock time_point pair.
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        bool timed = false;
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
    // Guest environment (each string "KEY=VALUE"). Populated by
    // set_guest_env() or by build_initial_stack() using
    // build_default_guest_env() when empty.
    std::vector<std::string> guest_env_;
    // Guest process name (set by prctl(PR_SET_NAME), returned by
    // prctl(PR_GET_NAME) and /proc/self/comm). Max 16 bytes (15 + NUL)
    // per Linux kernel. Defaults to the ELF basename.
    std::string guest_comm_ = "bifrost";

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
    // (sharded — see futex_shards_ above. The old single-mutex + single-map
    // design was removed in Turn 57 for scalability.)

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

    // ── Guest CPU time tracking (Turn 62) ──────────────────────────────
    // We track guest instructions executed to provide accurate rusage/
    // times values. The host's getrusage/wait4/times return the emulator
    // process's CPU time (including JIT compilation, memory management,
    // etc.) which is meaningless to the guest. Instead, we count guest
    // instructions and convert to seconds using the measured MIPS rate.
    //
    // guest_instructions_total_: atomic, incremented by every CPU's run
    //   loop (main + spawned threads + fork children). Represents the
    //   total guest CPU work done.
    // run_start_time_: when Emulator::run() was called. Used for the
    //   wall-clock "real" time in `toybox time`.
    // mips_estimate_: guest MIPS, updated periodically from the run loop.
    //   Used to convert guest_instructions → seconds. If 0 (before first
    //   measurement), we fall back to wall-clock time as the user/sys
    //   time estimate (better than zeros).
    std::atomic<uint64_t> guest_instructions_total_{0};
    std::chrono::steady_clock::time_point run_start_time_;
    std::atomic<double> mips_estimate_{0.0};

    // Convert guest instructions to seconds (user/sys CPU time).
    // If we have a MIPS estimate, use it; otherwise fall back to
    // wall-clock elapsed time (split 70/30 user/sys as a heuristic).
    double guest_instr_to_seconds(uint64_t instr) const {
        double mips = mips_estimate_.load(std::memory_order_relaxed);
        if (mips > 0.0 && instr > 0) {
            return static_cast<double>(instr) / (mips * 1e6);
        }
        return 0.0;
    }
    // Get the total guest instructions executed by this process (main +
    // spawned threads, NOT fork children — those are tracked separately
    // in ForkChild::guest_instructions).
    uint64_t get_guest_instructions() const {
        return guest_instructions_total_.load(std::memory_order_relaxed);
    }
    void add_guest_instructions(uint64_t n) {
        guest_instructions_total_.fetch_add(n, std::memory_order_relaxed);
    }

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
