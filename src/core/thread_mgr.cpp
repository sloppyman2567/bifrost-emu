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
#include "jit/frostjit.hpp"  // needed for jit_.reset() (full destructor)
#include "bifrost/version.hpp"

#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

namespace arm64emu {

void thread_entry(Emulator* emu, Emulator::GuestThread* gt) {
    // The child's CPU state was set up by spawn_thread() before the
    // host thread was created. We just run it to completion.
    CPU& cpu = gt->cpu;

    // BUGFIX: spawned threads previously had no signal draining, no
    // watchdog, and no graphics refresh — only a PC-mapped check every
    // 1 Mi instructions. Now we drain host signals periodically and
    // run a lightweight same-PC watchdog. (We still don't refresh SDL2
    // graphics from spawned threads — that's the main thread's job in
    // single-threaded mode. JIT also stays off for spawned threads per
    // the existing fork safety constraint.)
    constexpr uint64_t HANG_LIMIT = 50'000'000;
    uint64_t last_pc = static_cast<uint64_t>(-1);
    uint64_t same_pc_count = 0;
    uint64_t count = 0;
    try {
        while (cpu.running) {
            emu->step_public(cpu);
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
    gt->cpu = parent_cpu;
    gt->cpu.regs[0] = 0;
    gt->cpu.pc = entry_pc;
    gt->cpu.sp = stack_top;
    gt->cpu.running = true;

    if (flags & 0x80000) {  // CLONE_SETTLS
        gt->cpu.tpidr_el0 = tls;
        gt->cpu.tpidrro_el0 = tls;
    }

    uint64_t ctid_ptr = parent_cpu.regs[3];

    if (flags & 0x2000000) {  // CLONE_CHILD_CLEARTID
        gt->cpu.clear_child_tid = ctid_ptr;
    } else {
        gt->cpu.clear_child_tid = 0;
    }

    int child_tid = next_tid_.fetch_add(1);
    gt->cpu.tid = child_tid;
    gt->tid = child_tid;

    if ((flags & 0x1000000) && ctid_ptr) {  // CLONE_CHILD_SETTID
        mem_.store<uint32_t>(ctid_ptr, child_tid);
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

        if (flags & 0x80000) {  // CLONE_SETTLS
            parent_cpu.tpidr_el0 = tls;
            parent_cpu.tpidrro_el0 = tls;
        }

        int child_tid = static_cast<int>(getpid());
        parent_cpu.tid = child_tid;

        if ((flags & 0x1000000) && ctid_ptr) {  // CLONE_CHILD_SETTID
            mem_.store<uint32_t>(ctid_ptr, child_tid);
        }
        if (flags & 0x2000000) {  // CLONE_CHILD_CLEARTID
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
    if ((flags & 0x100000) && ptid_ptr) {
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
