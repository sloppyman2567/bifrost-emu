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
#include "frontend/dynamic_linker.h"  // allocate_thread_tls (SDL threads)
#include "jit/frostjit.hpp"  // needed for per-thread JIT + jit_.reset()
#include "frost/thunk.hpp"    // GraphicThunk::wake_sdl_semaphores (stop_sdl_threads)
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
    // ── JIT dispatch ──
    // Default (shared-JIT): spawned threads share the main's FrostJIT
    // (emu->jit_), saving 64 MiB per thread. blocks_mutex_ is held only
    // for table mutations, released before block execution.
    // Opt out via BIFROST_NO_SHARED_JIT=1 for per-thread JIT (lock-free,
    // 64 MiB per thread).
    FrostJIT* thread_jit = gt->jit.get();
    if (thread_jit == nullptr) {
        thread_jit = emu->jit_.get();  // shared mode
    }
    bool use_jit = (thread_jit != nullptr);
    constexpr uint64_t HANG_LIMIT = 50'000'000;
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;
    uint64_t count = 0;
    try {
        while (cpu.running) {
            if (use_jit) {
                // JIT dispatch — uses either the per-thread FrostJIT
                // (BIFROST_NO_SHARED_JIT=1) or the shared main FrostJIT
                // (default, Task 3). The shared mode serializes run_block
                // via blocks_mutex_; the per-thread mode is lock-free.
                thread_jit->run_block(cpu, *emu);
            } else {
                emu->step(cpu);
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
            // Also drain per-CPU pending signals (queued by cross-thread
            // tgkill/tkill/kill) — fix for the cross-thread CPU-mutation
            // race.
            if ((count & 0xFFF) == 0) {
                emu->drain_host_signals(cpu);
                emu->drain_pending_signals(cpu);
                emu->add_guest_instructions(4096);
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
    } catch (DecodeError& e) {
        // calls and illegal instructions in worker threads, matching the
        // main-thread behavior (see emulator.cpp's DecodeError handler).
        // pc < 4096 = NULL deref / zero page → SIGSEGV.
        // Otherwise = illegal instruction → SIGILL.
        uint64_t fault_pc = cpu.pc;
        int signo = (fault_pc < 4096) ? BIFROST_SIGSEGV : BIFROST_SIGILL;
        int si_code = (fault_pc < 4096) ? SEGV_MAPERR_EMU : ILL_ILLOPC_EMU;
        if (!deliver_signal(*emu, cpu, emu->signals(), signo, si_code, fault_pc)) {
            // No handler installed — default disposition terminates the
            // thread. deliver_signal already set cpu.running=false and
            // cpu.exit_code=128+signo.
            fprintf(stderr,
                "[%s] thread %d: %s at pc=0x%llx (no handler — terminating)\n",
                CODENAME, cpu.tid,
                (signo == BIFROST_SIGSEGV) ? "SIGSEGV (NULL deref)"
                                            : "SIGILL (illegal instruction)",
                static_cast<unsigned long long>(fault_pc));
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
    // CLONE_CHILD_CLEARTID / set_tid_address: zero the word at
    // clear_child_tid and perform a futex wake on it. This is how
    // pthread_join unblocks, AND — critically for musl — how an
    // orphaned __tl_lock is released when a thread exits while
    // holding it (musl's pthread_exit deliberately leaves __tl_lock
    // held on exit; the kernel's exit-time clear_child_tid handling
    // is what releases it).
    //
    // On real Linux, set_tid_address(2) and CLONE_CHILD_CLEARTID
    // share the same task->clear_child_tid field. We model the same
    // behavior in CPU state: set_tid_address() updates clear_child_tid
    // directly. So a single clear_child_tid write here covers both
    // the CLONE_CHILD_CLEARTID and set_tid_address contracts.
    //
    // path that wrote cpu.tid (not 0) to the address. When musl's
    // main thread called set_tid_address(&__thread_list_lock) and
    // then spawned a child with CLONE_CHILD_CLEARTID | ctid=&__thread_list_lock,
    // BOTH fields pointed to the same address. The clear_child_tid
    // path correctly wrote 0, but the set_tid_address_ptr path then
    // OVERWROTE it with the child's TID — leaving the lock orphaned
    // at value=tid after exit. This caused a deadlock in musl's
    // __tl_lock when the next pthread_create/pthread_join tried to
    // acquire it (CAS 0→tid failed, FUTEX_WAIT val=tid blocked
    // forever because no one would ever unlock).
    if (cpu.clear_child_tid) {
        emu->mem().store<uint32_t>(cpu.clear_child_tid, 0);
        auto* slot = emu->get_futex(cpu.clear_child_tid);
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->cv.notify_all();
        }
    }
    // set_tid_address_ptr is now redundant with clear_child_tid (they
    // share the same field per Linux semantics — see set_tid_address
    // syscall handler). The cleanup above already wrote 0 and woke
    // any waiters. We keep the field in CPU state only for debugging
    // / introspection; no second write is needed here.
    // state. We do NOT write cpu_id=-1 into the guest rseq area (the
    // area may already be unmapped if the thread's stack was torn down,
    // and the write raced with glibc's own cleanup in multi-threaded
    // tests causing hangs). Just clear the CPU-side bookkeeping.
    cpu.rseq_registered = false;
    cpu.rseq_addr = 0;
    cpu.rseq_sig = 0;
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
    //            but each thread has its own mask to be inherited from the parent.
    //
    // Copy the parent's architectural state (GPRs, FPRs, PSTATE, etc.)
    // without touching the pending queue or exclusive monitor. The reset
    // block below then explicitly clears sigpending, altstack, etc.
    // per Linux clone() semantics.
    gt->cpu.copy_arch_state_from(parent_cpu);
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
    // BUGFIX: per Linux semantics, a cloned thread starts with an EMPTY
    // pending signal set and a DISABLED altstack. The previous code
    // inherited the parent's sigpending and altstack verbatim, which
    // caused two bugs:
    //   (1) If the parent had a signal pending (e.g., SIGSEGV being
    //       delivered), the child would also "have it pending" and
    //       spuriously run the handler when it unblocked.
    //   (2) If the parent was ON the altstack when it called clone,
    //       the child would believe it's already on an altstack and
    //       deliver future signals to a stack it doesn't own.
    gt->cpu.sigpending = 0;
    gt->cpu.altstack = CPU::AltStack{};
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
    // ── Shared-JIT mode (default) ──
    // Spawned threads share the main thread's FrostJIT instance, saving
    // 64 MiB of code-cache memory per thread (8 threads = 512 MiB saved).
    // The block table is protected by blocks_mutex_ (held only for table
    // mutations, released before block execution so threads can block in
    // syscalls without deadlocking). Per-thread state (watchdog, hotness)
    // is thread-local. The code buffer is RWX (W^X disabled in shared
    // mode) so translation and execution can happen concurrently.
    //
    // Opt OUT via BIFROST_NO_SHARED_JIT=1: each spawned thread gets its
    // own FrostJIT (64 MiB code cache, lock-free execution). Use this
    // for compute-bound multi-threaded guests where lock contention on
    // blocks_mutex_ hurts throughput more than the memory cost.
    //
    // by releasing blocks_mutex_ before block execution.
    static bool no_shared_jit = (getenv("BIFROST_NO_SHARED_JIT") != nullptr);
    if (jit_enabled_ && jit_ && no_shared_jit) {
        gt->jit = std::make_unique<FrostJIT>();
        if (gt->jit) {
            gt->jit->set_direct_window(mem_.direct_window());
        }
    }
    alive_threads_.fetch_add(1);
    if (libc_single_threaded_addr_) {
        mem_.store<uint32_t>(libc_single_threaded_addr_, 0);
    }
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
void Emulator::stop_sdl_threads() {
    // Snapshot the SDL threads under the lock so we can set running=false
    // on each WITHOUT holding the lock while joining (the thread entry
    // takes sdl_threads_mu_ in wait_sdl_thread, so joining under it could
    // deadlock).
    std::vector<SdlThread*> pts;
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        pts.reserve(sdl_threads_.size());
        for (auto& [h, st] : sdl_threads_) {
            pts.push_back(st.get());
        }
    }
    if (pts.empty()) return;
    // Ask each SDL thread to stop: its interpreter loop exits on the next
    // step() return when cpu.running is false.
    for (SdlThread* st : pts) {
        st->cpu.running = false;
    }
    // Wake any thread blocked inside a host SDL wait (SDL_SemWait): post
    // every host SDL semaphore the guest created so the host call returns
    // and the loop can observe running==false.
    if (GraphicThunk* th = graphics_.thunk()) {
        th->wake_sdl_semaphores();
    }
    // Join each host thread now that it's signalled to stop. The worker
    // blocked in host SDL_SemWait returns (we just posted), the timer's
    // SDL_Delay(1) returns on its own, and the one-shot event thread has
    // already finished. Joining outside sdl_threads_mu_ avoids the
    // wait_sdl_thread lock ordering.
    for (SdlThread* st : pts) {
        if (st->host_thread.joinable()) {
            st->host_thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_.clear();
    }
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
        parent_cpu.is_fork_process = true;  // getpid() returns host PID, not 1
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
        // After fork(), the child inherits g_active_emu_ from the parent,
        // which points to the PARENT's Emulator — a dangling pointer in
        // the child's address space. When SIGINT (Ctrl-C) arrives in the
        // child, the host signal handler dereferences the dangling
        // pointer, either crashing or silently dropping the signal. This
        // is why Ctrl-C doesn't interrupt `toybox sh -c 'sleep 5'` —
        // the child (running sleep) gets SIGINT but can't forward it.
        // Fix: call install_host_signal_handlers() which sets
        // g_active_emu_ = this (the child's own Emulator).
        install_host_signal_handlers();
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
// ── SDL thunk threads (SDL_CreateThread / SDL_WaitThread) ─────────────
// 1.5.3-alpha. The game spawns worker threads (timer, music, event) via
// SDL_CreateThread and joins them with SDL_WaitThread. These are REAL
// concurrent guest threads: the guest SDL_Thread* handle is a small
// guest-addressable struct whose word 0 is a "done" futex flag and word 8
// holds the exit code. The host thread runs the guest function until it
// RETs to a sentinel LR (0x1000, the same unmapped sentinel the GLFW
// borrow-CPU runner uses), then records the exit code and futex-wakes the
// done word so SDL_WaitThread unblocks.
//
// Guest handle layout (allocated via mem_.mmap_alloc, all zeroed):
//   +0  u64 done   (0 = running, 1 = finished) — futex word
//   +8  u64 status (int32 exit code from the thread function)
void sdl_thread_entry(Emulator* emu, Emulator::SdlThread* st) {
    CPU& cpu = st->cpu;
    constexpr uint64_t SENTINEL_LR = 0x1000;
    uint64_t count = 0;
    int32_t exit_code = 0;
    try {
        // Run the guest thread function with the interpreter until it RETs
        // (pc == sentinel LR) or the thread is asked to stop. Per-instruction
        // stepping (mirroring the GLFW borrow-CPU runner) is REQUIRED here:
        // the game's setjmp/longjmp exception path re-enters the thread
        // function's continuation inside a nested jit_call_helper invocation,
        // whose loop only stops at its own return_pc and knows nothing about
        // the 0x1000 sentinel — a JIT-dispatched thread fn whose final RET
        // restores LR=0x1000 then gets dispatched as code (decode error).
        // With step() the sentinel check applies to every instruction, and
        // the thread bodies (timer/worker/event) are syscall/thunk-bound so
        // interpreter speed is fine.
        while (cpu.running && cpu.pc != SENTINEL_LR) {
            emu->step(cpu);
            count++;
            if ((count & 0xFFF) == 0) {
                emu->drain_host_signals(cpu);
                emu->drain_pending_signals(cpu);
                emu->add_guest_instructions(4096);
            }
            if ((count & 0xFFFFF) == 0) {
                if (!emu->mem().is_mapped(cpu.pc, 4)) break;
            }
        }
        // The thread function's return value is in x0/w0 (AArch64 ABI:
        // SDL_ThreadFunction returns int). Read it as the exit code.
        exit_code = static_cast<int32_t>(cpu.regs[0] & 0xFFFFFFFF);
    } catch (DecodeError& e) {
        uint64_t fault_pc = cpu.pc;
        int signo = (fault_pc < 4096) ? BIFROST_SIGSEGV : BIFROST_SIGILL;
        int si_code = (fault_pc < 4096) ? SEGV_MAPERR_EMU : ILL_ILLOPC_EMU;
        if (!deliver_signal(*emu, cpu, emu->signals(), signo, si_code, fault_pc)) {
            fprintf(stderr,
                "[%s] SDL thread 0x%llx: %s at pc=0x%llx (no handler — terminating)\n",
                CODENAME, static_cast<unsigned long long>(st->handle_addr),
                (signo == BIFROST_SIGSEGV) ? "SIGSEGV (NULL deref)"
                                            : "SIGILL (illegal instruction)",
                static_cast<unsigned long long>(fault_pc));
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] SDL thread 0x%llx: exception: %s\n",
                CODENAME, static_cast<unsigned long long>(st->handle_addr),
                e.what());
    }
    // Record the exit code + set the done flag, then futex-wake any
    // SDL_WaitThread waiter on the handle's done word.
    if (st->handle_addr) {
        Memory& mem = emu->mem();
        try {
            mem.store<uint64_t>(st->handle_addr + 8,
                                static_cast<uint64_t>(static_cast<uint32_t>(exit_code)));
            mem.store<uint64_t>(st->handle_addr + 0, 1);
        } catch (...) {}
        auto* slot = emu->get_futex(st->handle_addr);
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->cv.notify_all();
        }
    }
    emu->decrement_alive_threads();
}
uint64_t Emulator::spawn_sdl_thread(uint64_t fn, uint64_t data) {
    // Allocate a guest stack. 256 KiB is plenty for the game's worker
    // threads (they use only a few stack frames + call chains).
    constexpr uint64_t STACK_SIZE = 256 * 1024;
    uint64_t stack = mem_.mmap_alloc(STACK_SIZE);
    if (stack == 0) return 0;
    // Per-thread glibc TLS (TPIDR_EL0). Mirrors the clone path's TLS.
    uint64_t tls = 0;
    if (dyn_linker_) {
        tls = dyn_linker_->allocate_thread_tls(mem_);
    }
    // Guest SDL_Thread* handle: a 32-byte zeroed block.
    constexpr uint64_t HANDLE_SIZE = 32;
    uint64_t handle = mem_.mmap_alloc(HANDLE_SIZE);
    if (handle == 0) {
        mem_.untrack_allocation(stack, STACK_SIZE);
        return 0;
    }
    {
        std::vector<uint8_t> zeros(HANDLE_SIZE, 0);
        mem_.write(handle, zeros.data(), HANDLE_SIZE);
    }
    auto st = std::make_unique<SdlThread>();
    st->cpu.regs[0] = data;               // SDL_ThreadFunction(void* data)
    st->cpu.pc = fn;
    st->cpu.sp = stack + STACK_SIZE;
    st->cpu.regs[30] = 0x1000;            // sentinel LR: RET here = thread done
    st->cpu.running = true;
    st->cpu.pstate = 0;
    st->cpu.tid = next_tid_.fetch_add(1);
    st->tid = static_cast<uint64_t>(st->cpu.tid);
    st->stack_top = stack + STACK_SIZE;
    st->stack_size = STACK_SIZE;
    st->tls = tls;
    st->handle_addr = handle;
    if (tls) {
        st->cpu.tpidr_el0 = tls;
        st->cpu.tpidrro_el0 = tls;
    }
    alive_threads_.fetch_add(1);
    if (libc_single_threaded_addr_) {
        mem_.store<uint32_t>(libc_single_threaded_addr_, 0);
    }
    SdlThread* stp = st.get();
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_[handle] = std::move(st);
    }
    stp->host_thread = std::thread(sdl_thread_entry, this, stp);
    return handle;
}
int64_t Emulator::wait_sdl_thread(uint64_t handle, uint64_t status_ptr) {
    // Find the thread by its guest handle.
    SdlThread* st = nullptr;
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        auto it = sdl_threads_.find(handle);
        if (it == sdl_threads_.end()) {
            // Unknown handle (e.g. WaitThread on a NULL thread): no-op.
            if (status_ptr) {
                try { mem_.store<int32_t>(status_ptr, 0); } catch (...) {}
            }
            return 0;
        }
        st = it->second.get();
    }
    // Block until the done flag is set. The thread entry futex-wakes this
    // word after the guest function returns.
    {
        auto* slot = get_futex(handle);
        std::unique_lock<std::mutex> lk(slot->mu);
        slot->cv.wait(lk, [&]() {
            uint64_t done = 0;
            try { done = mem_.load<uint64_t>(handle); } catch (...) { return true; }
            return done != 0;
        });
    }
    // Read the exit code and write it through the status pointer.
    int32_t exit_code = 0;
    try {
        exit_code = static_cast<int32_t>(
            mem_.load<uint64_t>(handle + 8) & 0xFFFFFFFF);
    } catch (...) {}
    if (status_ptr) {
        try { mem_.store<int32_t>(status_ptr, exit_code); } catch (...) {}
    }
    // Join the host thread and free the guest resources (stack + handle).
    // TLS is intentionally not freed (matching clone() threads: their
    // TLS block is also not reclaimed on join).
    //
    // ORDER IS CRITICAL: join FIRST, erase AFTER. The thread entry sets
    // the done flag + futex-wakes BEFORE it returns from sdl_thread_entry,
    // so when the cv wait above returns the host thread may still be
    // running (decrement_alive_threads + return). erasing the SdlThread
    // first destroys its std::thread member while still joinable →
    // std::terminate ("terminate called without an active exception").
    // The join is cheap: the thread is one instruction from returning.
    uint64_t stack_top = st->stack_top, stack_size = st->stack_size;
    if (st->host_thread.joinable()) st->host_thread.join();
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_.erase(handle);
    }
    if (stack_top && stack_size) {
        mem_.untrack_allocation(stack_top - stack_size, stack_size);
    }
    if (handle) mem_.untrack_allocation(handle, 32);
    return 0;
}
} // namespace arm64emu
