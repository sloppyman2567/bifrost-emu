// core/thread_mgr.cpp — guest thread (vCPU) lifecycle.
//
// Implements Emulator::spawn_thread / join_threads / find_cpu_by_tid and
// the thread_entry trampoline that runs a cloned guest thread on a host
// OS thread until the guest calls exit/exit_group.
//
// Also implements fork_guest() via host fork(): CoW duplicate of the
// emulator; the child disables the JIT, fixes g_active_emu_, and
// returns 0 into the syscall handler (parent wait4s via host wait4).
// find_fork_child/reap_fork_child are stubs.
//
// This file is a friend of Emulator (see core/emulator.h) so it can
// access private state: threads_, threads_mu_, next_tid_, alive_threads_.
#include "core/emulator.h"
#include "core/crash_report.h"
#include "core/memory.h"
#include "core/signal.h"   // exit_robust_list helper
#include "frontend/dynamic_linker.h"  // allocate_thread_tls (SDL threads)
#include "jit/frostjit.hpp"  // needed for per-thread JIT + jit_.reset()
#include "frost/thunk.hpp"    // GraphicThunk::wake_sdl_semaphores (stop_sdl_threads)
#include "frost/audio_thunk.hpp"
#include "bifrost/version.hpp"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>
namespace arm64emu {
// SMC / mapping invalidation fan-out (see emulator.h). Pointer snapshot
// under threads_mu_, invalidation after release: a target thread may be
// inside run_block (which takes blocks_mutex_ briefly on its slow path),
// so holding threads_mu_ across invalidate_range risks lock-order issues
// with paths that take threads_mu_ while holding a blocks lock (none
// today — this ordering keeps it that way).
void Emulator::invalidate_jit_range(uint64_t addr, uint64_t size) {
    std::vector<std::shared_ptr<GuestThread>> per_thread;
    FrostJIT* main_jit = nullptr;
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        main_jit = jit_.get();
        for (auto& gt : threads_) {
            if (gt && gt->jit) per_thread.push_back(gt);
        }
    }
    if (main_jit) main_jit->invalidate_range(addr, size);
    for (auto& gt : per_thread) gt->jit->invalidate_range(addr, size);
}
void Emulator::invalidate_jit_all() {
    std::vector<std::shared_ptr<GuestThread>> per_thread;
    FrostJIT* main_jit = nullptr;
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        main_jit = jit_.get();
        for (auto& gt : threads_) {
            if (gt && gt->jit) per_thread.push_back(gt);
        }
    }
    if (main_jit) main_jit->invalidate_all();
    for (auto& gt : per_thread) gt->jit->invalidate_all();
}
// The robust-list exit walk is inlined in thread_entry below; the
// syscall-side version in threads.cpp is authoritative — keep in sync.
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
                    std::string mod;
                    uint64_t off = 0;
                    try {
                        if (auto* dl = emu->dyn_linker()) {
                            if (const LoadedObject* obj = dl->find_object_by_addr(cpu.pc)) {
                                mod = obj->name;
                                off = cpu.pc - obj->base_addr;
                            }
                        }
                    } catch (...) {}
                    report_crash(cpu, emu->mem(), "thread pc unmapped", cpu.pc, nullptr,
                                 mod.empty() ? nullptr : mod.c_str(), off);
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
            // cpu.exit_code=128+signo. throw site is quiet by default,
            // so this is the crash line.
            std::string mod;
            uint64_t off = 0;
            try {
                if (auto* dl = emu->dyn_linker()) {
                    if (const LoadedObject* obj = dl->find_object_by_addr(fault_pc)) {
                        mod = obj->name;
                        off = fault_pc - obj->base_addr;
                    }
                }
            } catch (...) {}
            report_crash(cpu, emu->mem(),
                         (signo == BIFROST_SIGSEGV) ? "thread sigsegv null pc"
                                                    : "thread sigill illegal inst",
                         fault_pc, nullptr,
                         mod.empty() ? nullptr : mod.c_str(), off);
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
        // Must not throw: this cleanup is outside the run loop's try
        // block. An unmapped/vanished ctid page used to escape as
        // std::terminate (and skip decrement_alive_threads).
        try {
            emu->mem().store<uint32_t>(cpu.clear_child_tid, 0);
            auto* slot = emu->get_futex(cpu.clear_child_tid);
            {
                std::lock_guard<std::mutex> lk(slot->mu);
                slot->cv.notify_all();
            }
        } catch (...) {
            // Guest tore down the ctid page before exit — nothing to wake.
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
    // Drop any LL/SC reservation this CPU still holds in the global
    // exclusive monitor. LDXR registers a raw CPU* that is otherwise only
    // removed by a later store to the same address; if the GuestThread is
    // reaped first, that store dereferences a freed CPU (ASan: heap-use-
    // after-free in interpreter.cpp's STXR/STLR reservation invalidation).
    // Must complete before `finished` so the reaper can't free us first.
    emu->excl_remove_cpu(&cpu);
    emu->decrement_alive_threads();
    // Final action: mark reapable, then return. Must be the LAST statement
    // (reap_finished_threads may join and drop the last shared_ptr).
    gt->finished.store(true, std::memory_order_release);
}
int Emulator::spawn_thread(CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                           uint64_t entry_pc, uint64_t arg, uint64_t tls) {
    // arg is the pthread start_routine argument, but Linux clone() semantics
    // require the child to return to the caller (entry_pc) with x0=0; the
    // pthread library wrapper is responsible for fetching arg from a TLS slot
    // or register set up by the parent before clone(). We accept the parameter
    // to keep the API forward-compatible with a future clone-with-arg variant.
    (void)arg;
    // Reap exited threads before allocating a new one, bounding per-thread
    // state (decode cache + optional JIT) under thread churn. Safe now that
    // thread_entry drops its exclusive-monitor reservation before it can be
    // reaped (the dangling CPU* that corrupted the host heap).
    reap_finished_threads();
    auto gt = std::make_shared<GuestThread>();
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
    gt->cpu.sigpending.store(0);
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
            gt->jit->set_direct_window(mem_.direct_window(), &mem_);
        }
    }
    // Shared-JIT mode safety: before a second vCPU can execute, unpatch all
    // chain slots and permanently disable runtime code writes, so the RWX
    // shared code buffer is never modified under a running core (x86 needs
    // the executing core to serialize after a code write). No-op in
    // per-thread-JIT mode and after the first spawn.
    if (gt->jit == nullptr && jit_) jit_->enter_multithreaded();
    alive_threads_.fetch_add(1);
    if (libc_single_threaded_addr_) {
        mem_.store<uint32_t>(libc_single_threaded_addr_, 0);
    }
    GuestThread* gtp = gt.get();
    // Start the host thread and install its handle BEFORE publishing the
    // GuestThread to threads_. std::thread's constructor starts the thread
    // immediately, so the guest can run to completion and set `finished`
    // before the handle assignment; publishing first let a concurrent
    // reap_finished_threads()/kill_other_threads() observe (and join) a
    // half-assigned std::thread — host-heap corruption. Installing the
    // handle first means any published GuestThread is fully initialised.
    gtp->host_thread = std::thread(thread_entry, this, gtp);
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        threads_.push_back(std::move(gt));
    }
    return child_tid;
}
void Emulator::join_threads() {
    // Snapshot under the lock, then join OUTSIDE it. A finishing worker
    // can take threads_mu_ on some exit paths (find_cpu_by_tid,
    // invalidate_jit_range) — joining while holding it deadlocks. Same
    // reasoning as kill_other_threads' comment.
    std::vector<std::shared_ptr<GuestThread>> victims;
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        victims.swap(threads_);
    }
    for (auto& gt : victims) {
        if (gt->host_thread.joinable())
            ::pthread_kill(gt->host_thread.native_handle(), KICK_SIGNAL);
    }
    for (auto& gt : victims) {
        if (gt->host_thread.joinable()) gt->host_thread.join();
    }
}
void Emulator::reap_finished_threads() {
    // Detached SDL threads are joined + freed here too.
    reap_retired_sdl_threads();
    // Collect finished threads under the lock, then join outside it. The
    // shared_ptr in `dead` pins each GuestThread (and its CPU/JIT) until
    // its host thread has fully returned, so a concurrent find_cpu_by_tid
    // or invalidate_jit_* holder can never touch freed state.
    std::vector<std::shared_ptr<GuestThread>> dead;
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        auto it = threads_.begin();
        while (it != threads_.end()) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                dead.push_back(std::move(*it));
                it = threads_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& gt : dead) {
        if (gt->host_thread.joinable()) gt->host_thread.join();
    }
}
void Emulator::excl_remove_cpu(CPU* cpu) {
    for (size_t i = 0; i < EXCL_MONITOR_SHARDS; i++) {
        ExclMonitorShard& shard = excl_monitor_shards_[i];
        std::lock_guard<std::mutex> g(shard.mu);
        for (auto it = shard.reservations.begin();
             it != shard.reservations.end();) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), cpu), vec.end());
            if (vec.empty()) {
                it = shard.reservations.erase(it);
            } else {
                ++it;
            }
        }
    }
}
void Emulator::wake_all_futexes() {
    for (size_t i = 0; i < FUTEX_SHARDS; i++) {
        std::lock_guard<std::mutex> g(futex_shards_[i].mu);
        for (auto& kv : futex_shards_[i].slots) {
            std::lock_guard<std::mutex> lk(kv.second->mu);
            if (kv.second->waiters > 0) kv.second->cv.notify_all();
        }
    }
}
void Emulator::kill_other_threads(CPU& caller) {
    // Snapshot victims under the lock; stop + join outside it (a victim
    // in thread exit takes futex shard locks; joining under threads_mu_
    // while another path takes threads_mu_ inside would deadlock).
    std::vector<std::shared_ptr<GuestThread>> victims;
    {
        std::lock_guard<std::mutex> g(threads_mu_);
        auto it = threads_.begin();
        while (it != threads_.end()) {
            if (&(*it)->cpu == &caller) { ++it; continue; }
            (*it)->cpu.running = false;
            victims.push_back(std::move(*it));
            it = threads_.erase(it);
        }
    }
    if (victims.empty()) return;
    // Wake futex sleepers so they observe running==false instead of
    // sleeping through the join below.
    wake_all_futexes();
    // Interrupt any victim blocked in a host syscall (read/nanosleep/…)
    // so it returns EINTR and exits instead of hanging the join.
    for (auto& gt : victims) {
        if (gt->host_thread.joinable())
            ::pthread_kill(gt->host_thread.native_handle(), KICK_SIGNAL);
    }
    for (auto& gt : victims) {
        if (gt->host_thread.joinable()) gt->host_thread.join();
        // No alive_threads_ fixup: each victim runs the normal thread
        // exit path (robust cleanup + decrement) on its way out.
    }
}
void Emulator::stop_sdl_threads() {
    // Snapshot the SDL threads (live + detached) under the lock, PINNING each
    // with a shared_ptr, then stop+join outside it (the thread entry takes
    // sdl_threads_mu_ in wait_sdl_thread, so joining under it could deadlock).
    std::vector<std::shared_ptr<SdlThread>> pts;
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        pts.reserve(sdl_threads_.size() + retired_sdl_threads_.size());
        for (auto& [h, st] : sdl_threads_) pts.push_back(st);
        for (auto& st : retired_sdl_threads_) pts.push_back(st);
    }
    if (pts.empty()) return;
    // Ask each SDL thread to stop: its interpreter loop exits on the next
    // step() return when cpu.running is false.
    for (auto& st : pts) st->cpu.running = false;
    // Wake any thread blocked inside a host SDL wait (SDL_SemWait): post
    // every host SDL semaphore the guest created so the host call returns
    // and the loop can observe running==false.
    if (GraphicThunk* th = graphics_.thunk()) {
        th->wake_sdl_semaphores();
    }
    // Interrupt host syscalls, then join + free exactly once per record.
    for (auto& st : pts) {
        if (st->host_thread.joinable())
            ::pthread_kill(st->host_thread.native_handle(), KICK_SIGNAL);
    }
    for (auto& st : pts) {
        std::lock_guard<std::mutex> fg(st->finish_mu);
        if (st->reaped) continue;
        st->reaped = true;
        if (st->host_thread.joinable()) st->host_thread.join();
        if (st->stack_top && st->stack_size)
            mem_.untrack_allocation(st->stack_top - st->stack_size, st->stack_size);
        if (st->handle_addr) mem_.untrack_allocation(st->handle_addr, 32);
    }
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_.clear();
        retired_sdl_threads_.clear();
    }
}
void Emulator::reap_retired_sdl_threads() {
    // Join detached threads that have finished and free their guest
    // resources. Keeps a detached thread's stack/handle/record from leaking
    // (the old detach release()d + leaked the record and never reclaimed the
    // guest stack).
    std::vector<std::shared_ptr<SdlThread>> ready;
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        auto it = retired_sdl_threads_.begin();
        while (it != retired_sdl_threads_.end()) {
            if ((*it)->finished.load(std::memory_order_acquire)) {
                ready.push_back(std::move(*it));
                it = retired_sdl_threads_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& st : ready) {
        std::lock_guard<std::mutex> fg(st->finish_mu);
        if (st->reaped) continue;
        st->reaped = true;
        if (st->host_thread.joinable()) st->host_thread.join();
        if (st->stack_top && st->stack_size)
            mem_.untrack_allocation(st->stack_top - st->stack_size, st->stack_size);
        if (st->handle_addr) mem_.untrack_allocation(st->handle_addr, 32);
    }
}
std::shared_ptr<CPU> Emulator::find_cpu_by_tid(int tid) {
    if (tid == 1) return std::shared_ptr<CPU>(&main_cpu_, [](CPU*){});
    std::lock_guard<std::mutex> g(threads_mu_);
    for (auto& gt : threads_) {
        if (gt->tid == tid) return std::shared_ptr<CPU>(gt, &gt->cpu);
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
// The child returns 0 from fork_guest into the syscall handler and
// continues in the normal run loop; no _exit() here.
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
        // After fork(), only the calling thread exists in the child. Host
        // threads the parent owned (guest vCPUs, SDL workers, the stats
        // reporter) did NOT survive; their std::thread objects are still
        // joinable but refer to nonexistent threads, so join()/detach()
        // can throw and destroying them calls std::terminate. Leak the
        // handles instead — a forked child normally execve()s immediately,
        // and a child that doesn't gets a clean single-threaded state
        // (alive_threads_=0 re-enables the single-thread futex fast path).
        {
            std::lock_guard<std::mutex> gt_lock(threads_mu_);
            (void)new std::vector<std::shared_ptr<GuestThread>>(std::move(threads_));
        }
        {
            std::lock_guard<std::mutex> st_lock(sdl_threads_mu_);
            (void)new std::unordered_map<uint64_t, std::shared_ptr<SdlThread>>(
                std::move(sdl_threads_));
            (void)new std::vector<std::shared_ptr<SdlThread>>(
                std::move(retired_sdl_threads_));
        }
        if (stats_reporter_thread_.joinable()) {
            stats_reporter_stop_.store(true, std::memory_order_relaxed);
            (void)new std::thread(std::move(stats_reporter_thread_));
        }
        alive_threads_.store(0);
        // Host threads don't survive fork(): release thunk-owned pump
        // threads (audio etc.) so their shutdown join() can't hang the
        // child, and let them spawn fresh ones.
        if (graphics_.audio_thunk())
            graphics_.audio_thunk()->detach_pump_for_fork_child();
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
// 1.5.4-alpha. The game spawns worker threads (timer, music, event) via
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
        // Nested JIT call helpers and post-call exits honor this boundary,
        // including a setjmp/longjmp continuation that returns to the thread
        // sentinel instead of the helper's original LR.
        cpu.jit_stop_pc = SENTINEL_LR;
        while (cpu.running && cpu.pc != SENTINEL_LR) {
            if (emu->jit_) emu->jit_->run_block(cpu, *emu);
            else emu->step(cpu);
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
        fprintf(stderr, "[%s] SDL thread 0x%llx: exception: %s "
                "pc=0x%llx sp=0x%llx lr=0x%llx\n",
                CODENAME, static_cast<unsigned long long>(st->handle_addr),
                e.what(), static_cast<unsigned long long>(cpu.pc),
                static_cast<unsigned long long>(cpu.sp),
                static_cast<unsigned long long>(cpu.regs[30]));
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
    // Final action: mark host-side completion so the reaper (detached
    // threads) or a racing stop can join us without a half-dead record.
    st->finished.store(true, std::memory_order_release);
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
    auto st = std::make_shared<SdlThread>();
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
    // SDL threads run guest code through the shared main JIT — same
    // MT-safe gate as spawn_thread (no-op after the first spawn).
    if (jit_) jit_->enter_multithreaded();
    alive_threads_.fetch_add(1);
    if (libc_single_threaded_addr_) {
        mem_.store<uint32_t>(libc_single_threaded_addr_, 0);
    }
    SdlThread* stp = st.get();
    // Start the host thread and install its handle BEFORE publishing to the
    // map: std::thread's ctor starts the thread immediately, so a wait/
    // detach/stop could otherwise observe a half-assigned std::thread.
    stp->host_thread = std::thread(sdl_thread_entry, this, stp);
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_[handle] = std::move(st);
    }
    return handle;
}
int64_t Emulator::wait_sdl_thread(uint64_t handle, uint64_t status_ptr) {
    // Find the thread by its guest handle, PINNING the record (shared_ptr)
    // so a concurrent detach/stop can't free it while we wait/join.
    std::shared_ptr<SdlThread> st;
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
        st = it->second;  // pin
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
    // Join + free guest resources exactly once. stop_sdl_threads()
    // (shutdown) and the detached-thread reaper may act on the same record;
    // finish_mu + reaped make exactly one of them join/free.
    {
        std::lock_guard<std::mutex> fg(st->finish_mu);
        if (!st->reaped) {
            st->reaped = true;
            if (st->host_thread.joinable()) st->host_thread.join();
            if (st->stack_top && st->stack_size)
                mem_.untrack_allocation(st->stack_top - st->stack_size, st->stack_size);
            if (st->handle_addr) mem_.untrack_allocation(st->handle_addr, 32);
        }
    }
    {
        std::lock_guard<std::mutex> g(sdl_threads_mu_);
        sdl_threads_.erase(handle);
    }
    return 0;
}
void Emulator::detach_sdl_thread(uint64_t handle) {
    std::lock_guard<std::mutex> g(sdl_threads_mu_);
    auto it = sdl_threads_.find(handle);
    if (it == sdl_threads_.end()) return;  // unknown/NULL: SDL no-op
    // Move to the retired list: no waiter will join it, so the run-loop
    // reaper joins it once `finished` and frees the guest stack/handle (and
    // stop_sdl_threads covers shutdown). This REPLACES the old
    // release()-and-leak, which left the guest stack live while the detached
    // thread ran on it and raced a concurrent SDL_WaitThread's join.
    retired_sdl_threads_.push_back(std::move(it->second));
    sdl_threads_.erase(it);
}
} // namespace arm64emu
