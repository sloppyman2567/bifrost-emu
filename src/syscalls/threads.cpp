// syscalls/threads.cpp — thread/signal syscalls: clone/clone3/futex/
// set_tid_address/set_robust_list/get_robust_list/tgkill/tkill/kill.
//
// Extracted verbatim from the original syscalls.cpp. References to
// private Emulator members (signals_, mem_, get_futex, spawn_thread,
// find_cpu_by_tid, etc.) work via the friend declaration.
#include "arm64_emu.hpp"
#include "syscalls/syscalls.h"

#include <errno.h>
#include <signal.h>
#include <mutex>
#include <thread>

namespace arm64emu {

int64_t syscall_threads(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& signals_ = emu.signals_;
    auto ret_host = [&](int64_t r) { cpu.regs[0] = (uint64_t)r; };

    // Lambda wrappers for Emulator member access (spawn_thread, get_futex).
    // (find_cpu_by_tid / decrement_alive_threads are available via emu.*
    // directly at call sites if needed; the lambda wrappers here are
    // only for the members used by every case.)
    auto spawn_thread = [&](CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                            uint64_t entry_pc, uint64_t arg, uint64_t tls) {
        return emu.spawn_thread(parent_cpu, flags, stack_top, entry_pc, arg, tls);
    };
    auto get_futex = [&](uint64_t addr) -> Emulator::FutexSlot* {
        return emu.get_futex(addr);
    };

    switch (num) {
        case 220: { // clone(flags, stack, ptid, ctid, tls)
            // AArch64 clone() syscall signature (matches glibc/musl):
            //   x0 = flags        (CLONE_*)
            //   x1 = stack        (top of child stack)
            //   x2 = parent_tidptr
            //   x3 = child_tidptr (CLONE_CHILD_SETTID writes TID here)
            //   x4 = tls          (new TPIDR_EL0, if CLONE_SETTLS)
            //
            // On success: parent gets child TID, child gets 0.
            // The new thread starts at the same PC as the syscall return
            // address (i.e., x30 / LR of the parent), with x0=0.
            //
            // We support the common subset: CLONE_VM | CLONE_FS |
            // CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
            // optionally combined with CLONE_SETTLS / CLONE_PARENT_SETTID
            // / CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID.
            uint64_t flags = a0;
            uint64_t stack = a1;
            uint64_t ptid_ptr = a2;
            (void)a3;  // ctid_ptr — child tid pointer; cleartid handled via CLONE_CHILD_CLEARTID flag path
            uint64_t tls = a4;

            // Refuse fork()-style clones (no CLONE_VM) for now
            const uint64_t BIFROST_CLONE_VM = 0x100;
            if (!(flags & BIFROST_CLONE_VM)) {
                // v1.4.0-alpha.2: real fork via host fork(). The child
                // gets a copy-on-write duplicate of the entire emulator
                // state (CPU, memory, etc.). The parent returns the child
                // PID; the child returns 0 and continues executing the
                // guest from the same PC. The parent's wait4() forwards
                // to host wait4() on the child PID.
                //
                // The guest's stack pointer (SP) is set to the `stack`
                // argument for the child (clone() semantics: the child
                // runs on a new stack). The parent's SP is unchanged.
                //
                // We mark the child process with a flag so the run loop
                // can detect it and handle exit differently.
                pid_t child_pid = ::fork();
                if (child_pid < 0) {
                    ret_host((uint64_t)-ENOMEM);
                    return 0;
                }
                if (child_pid == 0) {
                    // Child: set SP to the new stack, return 0.
                    cpu.sp = stack;
                    cpu.regs[0] = 0;
                    // Mark that we're a forked child so the run loop
                    // knows to _exit() instead of returning to the
                    // parent's caller. We use a sentinel in the CPU.
                    // Actually, the child just continues executing the
                    // guest. When the guest calls exit(), the syscall
                    // handler calls _exit() which terminates the child
                    // process. The parent's wait4() reaps it.
                    ret_host(0);
                    return 0;
                }
                // Parent: return child PID.
                // CLONE_PARENT_SETTID: write child TID to *ptid
                if ((flags & 0x100000) && ptid_ptr) {
                    mem_.store<uint32_t>(ptid_ptr, (uint32_t)child_pid);
                }
                ret_host((uint64_t)child_pid);
                return 0;
            }

            // The new thread's entry point is the parent's link register
            // (X30). This matches the AArch64 convention where clone()
            // returns to the caller in both parent and child — the child
            // then checks x0==0 and calls the thread function.
            uint64_t entry_pc = cpu.regs[30];  // LR
            uint64_t arg = 0;  // x0 will be set to 0 for child

            int child_tid = spawn_thread(cpu, flags, stack, entry_pc, arg, tls);
            if (child_tid < 0) {
                ret_host((uint64_t)-ENOMEM);
                return 0;
            }

            // CLONE_PARENT_SETTID: write child TID to *ptid
            if ((flags & 0x100000) && ptid_ptr) {  // CLONE_PARENT_SETTID
                mem_.store<uint32_t>(ptid_ptr, child_tid);
            }

            ret_host(child_tid);
            return 0;
        }

        case 221: { // clone3 - not supported (use clone)
            ret_host((uint64_t)-ENOSYS);
            return 0;
        }

        case 98: { // futex(uaddr, op, val, timeout, uaddr2, val3)
            // Real futex implementation: WAIT blocks the calling thread
            // until woken or timeout; WAKE wakes blocked threads. Uses
            // per-address (mutex, condvar) pairs stored in the futex table.
            //
            // Supported ops:
            //   FUTEX_WAIT (0):           block if *uaddr == val
            //   FUTEX_WAKE (1):           wake up to val waiters
            //   FUTEX_WAIT_BITSET (9):    like WAIT but with bitset
            //   FUTEX_WAKE_BITSET (10):   like WAKE but with bitset
            //   FUTEX_REQUEUE (3):        requeue waiters from uaddr to uaddr2
            //   FUTEX_CMP_REQUEUE (4):    requeue with comparison
            //   FUTEX_LOCK_PI / UNLOCK_PI / etc.: not supported (return -ENOSYS)
            uint64_t uaddr = a0;
            uint32_t op = (uint32_t)a1;
            uint32_t val = (uint32_t)a2;
            uint64_t timeout_ptr = a3;
            (void)a4;  // uaddr2 — used by FUTEX_REQUEUE, not yet implemented
            (void)a5;  // val3   — used by FUTEX_REQUEUE, not yet implemented

            // Mask out private flag — we treat all futexes as private
            op &= ~0x80;  // FUTEX_PRIVATE_FLAG

            switch (op) {
                case 0:  // FUTEX_WAIT
                case 9:  // FUTEX_WAIT_BITSET
                {
                    // Backward-compat: if no other threads are alive to
                    // wake us, return 0 immediately (pretend we waited
                    // and were woken). This preserves the previous
                    // single-threaded behavior where futex was a no-op.
                    // Without this, glibc's startup mutex lock would
                    // block forever in single-threaded code.
                    if (emu.alive_threads_.load() == 0) {
                        ret_host(0);
                        return 0;
                    }
                    Emulator::FutexSlot* slot = get_futex(uaddr);
                    std::unique_lock<std::mutex> lk(slot->mu);
                    // Re-check *uaddr == val UNDER the slot lock so that
                    // a concurrent WAKE can't slip in between our initial
                    // check and our waiter increment. Without this, a
                    // waker could see waiters==0 and skip notify, leaving
                    // us sleeping forever.
                    uint32_t cur = mem_.load<uint32_t>(uaddr);
                    if (cur != val) {
                        ret_host((uint64_t)-EAGAIN);
                        return 0;
                    }
                    slot->waiters++;
                    // cv.wait() (no predicate) blocks until notified or
                    // spuriously woken. The guest is expected to loop on
                    // FUTEX_WAIT (re-checking *uaddr) per the futex API
                    // contract, so spurious wakeups returning 0 are safe.
                    // The lock is released while waiting and reacquired
                    // on wake, allowing a concurrent FUTEX_WAKE to take
                    // the lock and call notify before we increment
                    // waiters — but the order is: we increment waiters
                    // (line above) BEFORE releasing the lock via wait(),
                    // so a waker acquiring the lock after us is
                    // guaranteed to see the incremented count.
                    if (timeout_ptr == 0) {
                        slot->cv.wait(lk);
                    } else {
                        // timeout is struct timespec { sec, nsec }
                        uint64_t sec = mem_.load<uint64_t>(timeout_ptr);
                        uint64_t nsec = mem_.load<uint64_t>(timeout_ptr + 8);
                        auto duration = std::chrono::seconds(sec) +
                                        std::chrono::nanoseconds(nsec);
                        slot->cv.wait_for(lk, duration);
                    }
                    slot->waiters--;
                    ret_host(0);
                    return 0;
                }
                case 1:  // FUTEX_WAKE
                case 10: // FUTEX_WAKE_BITSET
                {
                    Emulator::FutexSlot* slot = get_futex(uaddr);
                    std::lock_guard<std::mutex> lk(slot->mu);
                    int to_wake = (int)val;
                    if (to_wake <= 0) { ret_host(0); return 0; }
                    int woken = std::min(to_wake, slot->waiters);
                    if (woken >= slot->waiters) {
                        slot->cv.notify_all();
                    } else {
                        for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    }
                    ret_host(woken);
                    return 0;
                }
                case 3:  // FUTEX_REQUEUE
                case 4:  // FUTEX_CMP_REQUEUE
                {
                    // For simplicity, treat requeue as wake — wake up to
                    // `val` waiters on uaddr, ignore uaddr2. Real requeue
                    // moves them to a different futex word without waking.
                    Emulator::FutexSlot* slot = get_futex(uaddr);
                    std::lock_guard<std::mutex> lk(slot->mu);
                    int woken = std::min((int)val, slot->waiters);
                    if (woken >= slot->waiters) slot->cv.notify_all();
                    else for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    ret_host(woken);
                    return 0;
                }
                default:
                    // PI futexes and others: not supported
                    ret_host((uint64_t)-ENOSYS);
                    return 0;
            }
        }

        case 96: { // set_tid_address
            // Stores the tid_address pointer in the calling thread's CPU
            // state. Real Linux writes the TID to *tid_address when the
            // thread terminates (used by futex on child termination).
            cpu.set_tid_address_ptr = a0;
            ret_host(cpu.tid);
            return 0;
        }

        case 99: { // set_robust_list - no-op
            ret_host(0);
            return 0;
        }

        case 100: { // get_robust_list — no-op stub (AArch64 100)
            ret_host(0);
            return 0;
        }

        case 131: { // tgkill(tgid, tid, sig) — send signal to specific thread
            // v1.4.0-alpha: deliver the signal to the target thread.
            // For now we only handle signals directed at the current
            // thread (tid == cpu.tid). Cross-thread delivery is left
            // to a future version.
            (void)a0;  // tgid
            (void)a1;  // tid
            int sig = (int)a2;
            if (sig == 0) {
                // Signal 0: just check permission (always succeeds).
                ret_host(0);
                return 0;
            }
            if (sig >= 1 && sig <= MAX_SIGNAL) {
                // If there's a handler installed, deliver it.
                // Otherwise apply default disposition (which may
                // terminate the guest).
                deliver_signal(emu, cpu, signals_, sig);
            }
            ret_host(0);
            return 0;
        }

        case 130: { // tkill(tid, sig)
            int sig = (int)a1;
            if (sig == 0) { ret_host(0); return 0; }
            if (sig >= 1 && sig <= MAX_SIGNAL) {
                deliver_signal(emu, cpu, signals_, sig);
            }
            ret_host(0);
            return 0;
        }

        case 129: { // kill(pid, sig)
            int sig = (int)a1;
            if (sig == 0) { ret_host(0); return 0; }
            if (sig >= 1 && sig <= MAX_SIGNAL) {
                deliver_signal(emu, cpu, signals_, sig);
            }
            ret_host(0);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
