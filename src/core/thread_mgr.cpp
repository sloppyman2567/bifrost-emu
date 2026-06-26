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

    uint64_t count = 0;
    try {
        while (cpu.running) {
            emu->step_public(cpu);
            count++;
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
        // Set up the child's CPU state: new stack, return value 0.
        // The child returns from clone() just like the parent — it
        // continues executing the guest from the instruction after SVC.
        // The normal run loop in main() will handle the child's exit.
        parent_cpu.sp = child_stack;
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
        // not thread/process-safe. The interpreter is always safe.
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
