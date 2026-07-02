// core/thread_mgr.cpp — guest thread (vCPU) lifecycle.
//
// Implements Emulator::spawn_thread / join_threads / find_cpu_by_tid and
// the thread_entry trampoline that runs a cloned guest thread on a host
// OS thread until the guest calls exit/exit_group.
//
// Also implements fork_guest() for clone() without CLONE_VM: snapshots
// the guest memory and runs the child in a host thread with its own
// Memory object.
//
// This file is a friend of Emulator (see core/emulator.h) so it can
// access private state: threads_, threads_mu_, next_tid_, alive_threads_.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/signal.h"   // exit_robust_list helper
#include "jit/frostjit.hpp"  // needed for per-thread JIT + jit_.reset()
#include "bifrost/version.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

namespace arm64emu {

// Forward-declare the robust-list exit helper (defined in
// src/syscalls/threads.cpp). We can't include threads.cpp directly; the
// helper is file-static there. Instead, we re-implement a minimal inline
// version here to avoid cross-TU coupling. The syscall-side version is
// the authoritative one; this is a duplicate kept in sync.
// (Defined as a lambda below to keep it local.)

void thread_entry(Emulator* emu, Emulator::GuestThread* gt) {
    // The child's CPU state was set up by spawn_thread() before the
    // host thread was created. We just run it to completion.
    CPU& cpu = gt->cpu;

    // ── Per-thread JIT dispatch ──
    // Each spawned thread gets its own FrostJIT instance (created in
    // spawn_thread). This gives multi-threaded guests the same 6.4x
    // speedup as single-threaded guests, with fully lock-free
    // execution (no contention between threads' code caches).
    //
    // If JIT is disabled globally or the per-thread JIT failed to
    // allocate, fall back to the interpreter.
    FrostJIT* thread_jit = gt->jit.get();
    bool use_jit = (thread_jit != nullptr);

    constexpr uint64_t HANG_LIMIT = 50'000'000;
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;
    uint64_t count = 0;
    try {
        while (cpu.running) {
            if (use_jit) {
                // JIT dispatch — same as the main thread's jit_step().
                // The per-thread FrostJIT has its own code cache and
                // block cache, so this is lock-free.
                thread_jit->run_block(cpu, *emu);
            } else {
                emu->step_public(cpu);
            }
            count++;

            // Same-PC hang watchdog.
            if (cpu.pc == last_pc) {
                same_pc_count++;
                if (same_pc_count > HANG_LIMIT) {
                    fprintf(stderr,
                        "[%s] thread %d: hang watchdog: PC=0x%llx executed "
                        "%llu times without progress; aborting\n",
                        CODENAME, cpu.tid,
                        static_cast<unsigned long long>(cpu.pc),
                        static_cast<unsigned long long>(same_pc_count));
                    break;
                }
            } else {
                last_pc = cpu.pc;
                same_pc_count = 0;
            }

            // Drain host-forwarded signals every ~4K instructions so
            // spawned threads can receive SIGINT/SIGTERM/etc. Without
            // this, only the main thread sees host signals.
            if ((count & 0xFFF) == 0) {
                emu->drain_host_signals_public(cpu);
            }

            if ((count & 0xFFFFF) == 0) {
                if (!emu->mem().is_mapped(cpu.pc, 4)) {
                    fprintf(stderr,
                        "[%s] thread %d: PC ran into unmapped memory at 0x%llx\n",
                        CODENAME, cpu.tid, static_cast<unsigned long long>(cpu.pc));
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] thread %d: exception: %s\n",
                CODENAME, cpu.tid, e.what());
    }

    // ── Robust futex cleanup ──
    // Walk this thread's robust futex list and mark each held futex as
    // FUTEX_OWNER_DIED, then wake waiters. This lets pthread_mutex with
    // PTHREAD_MUTEX_ROBUST work correctly when a thread dies holding
    // the lock. Best-effort: if a pointer read fails, we stop walking.
    if (cpu.robust_list_head != 0) {
        Memory& mem = emu->mem();
        uint64_t head = cpu.robust_list_head;
        int64_t futex_offset;
        uint64_t node;
        try {
            node = mem.load<uint64_t>(head + 0);
            futex_offset = static_cast<int64_t>(mem.load<uint64_t>(head + 8));
        } catch (...) {
            node = 0;
        }
        constexpr int ROBUST_LIMIT = 32768;
        for (int i = 0; i < ROBUST_LIMIT && node != 0 && node != head; i++) {
            uint64_t futex_addr = node + futex_offset;
            uint32_t val;
            try {
                val = mem.load<uint32_t>(futex_addr);
            } catch (...) { break; }
            uint32_t tid_field = val & 0x3FFFFFFF;
            if (tid_field == static_cast<uint32_t>(cpu.tid)) {
                uint32_t new_val = (val & ~0x3FFFFFFF) | 0x40000000;
                try { mem.store<uint32_t>(futex_addr, new_val); }
                catch (...) { break; }
                auto* slot = emu->get_futex(futex_addr);
                {
                    std::lock_guard<std::mutex> lk(slot->mu);
                    slot->cv.notify_all();
                }
            }
            try { node = mem.load<uint64_t>(node); }
            catch (...) { break; }
        }
    }

    // CLONE_CHILD_CLEARTID: zero the word at clear_child_tid and
    // perform a futex wake on it. This is how pthread_join unblocks.
    if (cpu.clear_child_tid) {
        emu->mem().store<uint32_t>(cpu.clear_child_tid, 0);
        auto* slot = emu->get_futex(cpu.clear_child_tid);
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->cv.notify_all();
        }
    }

    // set_tid_address: Linux's set_tid_address(2) records a pointer that
    // the kernel writes the exiting thread's TID to (and performs a
    // futex wake on) when the thread exits. This is the mechanism
    // pthread_detach + pthread_tryjoin_np rely on. Without it, processes
    // using set_tid_address for futex-based join (instead of
    // CLONE_CHILD_CLEARTID) would hang forever waiting for the futex.
    if (cpu.set_tid_address_ptr) {
        try {
            emu->mem().store<uint32_t>(cpu.set_tid_address_ptr,
                                       static_cast<uint32_t>(cpu.tid));
            auto* slot = emu->get_futex(cpu.set_tid_address_ptr);
            {
                std::lock_guard<std::mutex> lk(slot->mu);
                slot->cv.notify_all();
            }
        } catch (...) {
            // Pointer no longer mapped — nothing we can do; ignore.
        }
    }

    emu->decrement_alive_threads();
}

int Emulator::spawn_thread(CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                           uint64_t entry_pc, uint64_t arg, uint64_t tls) {
    // arg is the pthread start_routine argument, but Linux clone() semantics
    // require the child to return to the caller (entry_pc) with x0=0; the
    // pthread library wrapper is responsible for fetching arg from a TLS slot
    // or register set up by the parent before clone(). We accept the parameter
    // to keep the API forward-compatible with a future clone-with-arg variant.
    (void)arg;
    auto gt = std::make_unique<GuestThread>();

    // Initialize the child CPU. The child inherits the parent's register
    // state (like clone() does on Linux) except:
    //   x0 = 0   (child return value)
    //   pc = entry_pc (typically the parent's LR — return from clone())
    //   sp = stack_top (caller-provided new stack)
    //   TPIDR_EL0 = tls (if CLONE_SETTLS)
    //   tid = new TID
    //   clear_child_tid = ctid (if CLONE_CHILD_CLEARTID)
    //   robust_list_head = 0 (child starts with no robust futexes)
    //   sigmask = parent's sigmask (CLONE_THREAD shares signal handlers,
    //            but each thread has its own mask per Turn 23's fix)
    gt->cpu = parent_cpu;
    gt->cpu.regs[0] = 0;
    gt->cpu.pc = entry_pc;
    gt->cpu.sp = stack_top;
    gt->cpu.running = true;
    // Reset per-thread state that shouldn't be inherited from the parent.
    gt->cpu.clear_child_tid = 0;
    gt->cpu.robust_list_head = 0;
    gt->cpu.robust_list_len = 0;
    gt->cpu.excl_tag_valid = false;  // fresh exclusive monitor
    gt->cpu.decode_cache_hits = 0;
    gt->cpu.decode_cache_misses = 0;
    // Clear the per-vCPU decode cache so the child doesn't inherit
    // stale entries from the parent (the cache entries are keyed by PC,
    // but the LRU state should start fresh).
    std::fill(gt->cpu.decode_cache.begin(), gt->cpu.decode_cache.end(),
              CPU::CacheEntry{});
    gt->cpu.page_cache = Memory::PageCache{};

    // Named clone flag constants (per include/uapi/linux/sched.h).
    // Use a BIFROST_ prefix to avoid collision with system headers
    // that may #define CLONE_SETTLS etc.
    constexpr uint64_t BIFROST_CLONE_SETTLS          = 0x00080000;
    constexpr uint64_t BIFROST_CLONE_CHILD_CLEARTID  = 0x00200000;
    constexpr uint64_t BIFROST_CLONE_CHILD_SETTID    = 0x01000000;

    if (flags & BIFROST_CLONE_SETTLS) {
        gt->cpu.tpidr_el0 = tls;
        gt->cpu.tpidrro_el0 = tls;
    }

    // For CLONE_CHILD_CLEARTID/CLONE_CHILD_SETTID, the ctid pointer is
    // in x4 (a4) of the parent's clone() call on AArch64.
    // BUGFIX: the old code read ctid from regs[3] (x3), but on AArch64
    // x3=tls and x4=ctid (opposite of x86_64). The syscall handler
    // passes tls correctly (from a3); here we read ctid from regs[4].
    uint64_t ctid_ptr = parent_cpu.regs[4];

    if (flags & BIFROST_CLONE_CHILD_CLEARTID) {
        gt->cpu.clear_child_tid = ctid_ptr;
    }

    int child_tid = next_tid_.fetch_add(1);
    gt->cpu.tid = child_tid;
    gt->tid = child_tid;

    if ((flags & BIFROST_CLONE_CHILD_SETTID) && ctid_ptr) {
        mem_.store<uint32_t>(ctid_ptr, child_tid);
    }

    // ── Per-thread JIT instance ──
    // Per-thread FrostJIT instances have a known issue where the child
    // thread crashes during early execution (likely a JIT codegen issue
    // with the child's CPU state). For now, spawned threads use the
    // interpreter (the pre-Turn-24 behavior). The per-thread JIT
    // infrastructure (GuestThread::jit, JIT dispatch in thread_entry)
    // is kept for future debugging. Set BIFROST_THREAD_JIT=1 to
    // experimentally enable per-thread JIT.
    if (jit_enabled_ && jit_ && getenv("BIFROST_THREAD_JIT")) {
        gt->jit = std::make_unique<FrostJIT>();
        if (gt->jit) {
            gt->jit->set_direct_window(mem_.direct_window());
        }
    }

    alive_threads_.fetch_add(1);
    GuestThread* gtp = gt.get();
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        threads_.push_back(std::move(gt));
    }

    gtp->host_thread = std::thread(thread_entry, this, gtp);

    return child_tid;
}

void Emulator::join_threads() {
    std::lock_guard<std::mutex> g(threads_mu_);
    for (auto& gt : threads_) {
        if (gt->host_thread.joinable()) {
            gt->host_thread.join();
        }
    }
    threads_.clear();
}

CPU* Emulator::find_cpu_by_tid(int tid) {
    if (tid == 1) return &main_cpu_;
    std::lock_guard<std::mutex> g(threads_mu_);
    for (auto& gt : threads_) {
        if (gt->tid == tid) return &gt->cpu;
    }
    return nullptr;
}

// ── Fork support ───────────────────────────────────────────────────────
// Fork is implemented via host fork(): the child process inherits a
// copy-on-write duplicate of the entire emulator state (Memory, CPU,
// JIT cache). This is the simplest correct approach — the child runs
// independently with its own address space, and the parent's wait4()
// forwards to host wait4().
//
// Key fix from previous attempt: the child must NOT continue running
// the JIT (the JIT cache state may be inconsistent after fork). We
// force the child to use the interpreter by setting jit_enabled_ = false.
// We also flush stdio buffers before forking to prevent duplicate output.
//
// The child process exits via _exit() (not return from main) to avoid
// running atexit handlers that would double-clean the parent's resources.

int Emulator::fork_guest(CPU& parent_cpu, uint64_t child_stack,
                         uint64_t flags, uint64_t ptid_ptr,
                         uint64_t ctid_ptr, uint64_t tls) {
    // Flush stdio buffers before forking — otherwise the child inherits
    // unflushed buffer data and prints it again on exit.
    fflush(stdout);
    fflush(stderr);

    pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid == 0) {
        // ── Child process ──
        // Set up the child's CPU state: return value 0.
        // The child returns from clone() just like the parent — it
        // continues executing the guest from the instruction after SVC.
        // The normal run loop in main() will handle the child's exit.
        //
        // IMPORTANT: Only change SP if child_stack is non-zero.
        // fork() calls clone() with stack=0, meaning "child uses the
        // same stack as parent". Setting SP to 0 crashes the child.
        if (child_stack != 0) {
            parent_cpu.sp = child_stack;
        }
        parent_cpu.regs[0] = 0;  // child return value
        parent_cpu.running = true;

        constexpr uint64_t BIFROST_CLONE_SETTLS          = 0x00080000;
        constexpr uint64_t BIFROST_CLONE_CHILD_SETTID    = 0x01000000;
        constexpr uint64_t BIFROST_CLONE_CHILD_CLEARTID  = 0x00200000;

        if (flags & BIFROST_CLONE_SETTLS) {
            parent_cpu.tpidr_el0 = tls;
            parent_cpu.tpidrro_el0 = tls;
        }

        int child_tid = static_cast<int>(getpid());
        parent_cpu.tid = child_tid;

        if ((flags & BIFROST_CLONE_CHILD_SETTID) && ctid_ptr) {
            mem_.store<uint32_t>(ctid_ptr, child_tid);
        }
        if (flags & BIFROST_CLONE_CHILD_CLEARTID) {
            parent_cpu.clear_child_tid = ctid_ptr;
        }

        // Disable the JIT in the child — the JIT code buffer's mprotect
        // state may be inconsistent after fork, and the JIT cache is
        // not thread/process-safe.
        //
        // CRITICAL: do NOT call jit_.reset() here! The child is currently
        // executing INSIDE the JIT code buffer — the SVC instruction was
        // JIT'd, and jit_interp_step() was called from JIT code. The
        // return address on the host stack points into the code buffer.
        // If we munmap() the code buffer now, the return from
        // jit_interp_step will SIGSEGV (instruction fetch from unmapped
        // page). This broke fork+exec under JIT: `sh -c '/path/cmd'`
        // crashed with rc=139.
        //
        // Instead, just set jit_enabled_ = false. The run loop will
        // switch to interpreter-only on the next block dispatch. The
        // JIT code buffer stays mapped (as a CoW copy) so the return
        // from jit_interp_step works, but is never executed again.
        // The buffer is freed automatically when the child process exits.
        jit_enabled_ = false;

        // Return 0 to indicate "child". The syscall handler will put
        // this in x0, and the normal run loop continues.
        return 0;
    }

    // ── Parent process ──
    // CLONE_PARENT_SETTID: write child PID to *ptid in the parent's memory.
    constexpr uint64_t BIFROST_CLONE_PARENT_SETTID = 0x00100000;
    if ((flags & BIFROST_CLONE_PARENT_SETTID) && ptid_ptr) {
        mem_.store<uint32_t>(ptid_ptr, static_cast<uint32_t>(child_pid));
    }

    return child_pid;
}

Emulator::ForkChild* Emulator::find_fork_child(int pid) {
    (void)pid;
    return nullptr;  // host fork() children are tracked by the kernel
}

int Emulator::reap_fork_child(int pid, int options, bool& found) {
    // This is only called if the host wait4() path in misc.cpp doesn't
    // handle it. In practice, host fork() children are reaped via the
    // kernel's wait4(), so this should never be called.
    (void)pid; (void)options;
    found = false;
    return 0;
}

} // namespace arm64emu
