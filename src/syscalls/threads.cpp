// syscalls/threads.cpp — thread/signal syscalls: clone/clone3/futex/
// set_tid_address/set_robust_list/get_robust_list/tgkill/tkill/kill.
//
// Extracted verbatim from the original syscalls.cpp. References to
// private Emulator members (signals_, mem_, get_futex, spawn_thread,
// find_cpu_by_tid, etc.) work via the friend declaration.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include "vfs/vfs.h"
#include "jit/frostjit.hpp"

#include <errno.h>
#include <signal.h>
#include <mutex>
#include <cstring>
#include <vector>
#include <thread>

namespace arm64emu {

int64_t syscall_threads(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& signals_ = emu.signals_;

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
            // We support two paths:
            //   1. CLONE_VM (threads): spawn a vCPU on a host thread.
            //   2. No CLONE_VM (fork): host fork() with CoW memory.
            uint64_t flags = a0;
            uint64_t stack = a1;
            uint64_t ptid_ptr = a2;
            uint64_t ctid_ptr = a3;
            uint64_t tls = a4;

            const uint64_t BIFROST_CLONE_VM = 0x100;
            if (!(flags & BIFROST_CLONE_VM)) {
                // ── Fork path (no CLONE_VM) ──
                // Use host fork() for copy-on-write memory. The child
                // process inherits the entire emulator state and runs
                // independently. The parent's wait4() forwards to host
                // wait4().
                int child_pid = emu.fork_guest(cpu, stack, flags,
                                               ptid_ptr, ctid_ptr, tls);
                if (child_pid < 0) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                } else {
                    ret_host(static_cast<uint64_t>(child_pid));
                }
                return 0;
            }

            // ── Thread path (CLONE_VM) ──
            // The new thread's entry point is the parent's link register
            // (X30). This matches the AArch64 convention where clone()
            // returns to the caller in both parent and child — the child
            // then checks x0==0 and calls the thread function.
            uint64_t entry_pc = cpu.regs[30];  // LR
            uint64_t arg = 0;  // x0 will be set to 0 for child

            int child_tid = spawn_thread(cpu, flags, stack, entry_pc, arg, tls);
            if (child_tid < 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }

            // CLONE_PARENT_SETTID: write child TID to *ptid
            if ((flags & 0x100000) && ptid_ptr) {  // CLONE_PARENT_SETTID
                mem_.store<uint32_t>(ptid_ptr, child_tid);
            }

            ret_host(child_tid);
            return 0;
        }

        case 221: { // execve — AArch64 syscall 221
            // execve(path, argv, envp) — replace the guest's memory image
            // with a new ELF binary. This is called by the shell after
            // fork() to run external commands.
            //
            // We implement this by:
            //   1. Reading the path from guest memory
            //   2. Checking if it's a valid AArch64 ELF
            //   3. If yes: clear guest memory, reload the ELF, set up new
            //      stack, jump to entry point
            //   4. If no: return -ENOENT
            //
            // This is called in the child process after fork(). The child
            // has a CoW copy of the parent's memory, so clearing it is
            // safe — the parent is unaffected.
            std::string path = VFS::read_path(mem_, a0);
            if (path.empty()) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT)));
                return 0;
            }

            // Read the ELF file.
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOENT)));
                return 0;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOEXEC)));
                return 0;
            }
            std::vector<uint8_t> elf_data(sz);
            if (fread(elf_data.data(), 1, sz, f) != static_cast<size_t>(sz)) {
                fclose(f);
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EIO)));
                return 0;
            }
            fclose(f);

            // Validate it's an AArch64 ELF.
            if (elf_data.size() < 64 || elf_data[0] != 0x7f || elf_data[1] != 'E') {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOEXEC)));
                return 0;
            }
            uint16_t e_machine;
            memcpy(&e_machine, elf_data.data() + 18, 2);
            if (e_machine != 183) {  // EM_AARCH64
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOEXEC)));
                return 0;
            }

            // Read argv from guest memory.
            std::vector<std::string> new_argv;
            uint64_t argv_ptr = a1;
            while (true) {
                uint64_t str_ptr;
                try {
                    str_ptr = mem_.load<uint64_t>(argv_ptr);
                } catch (...) { break; }
                if (str_ptr == 0) break;
                std::string arg = VFS::read_path(mem_, str_ptr);
                new_argv.push_back(arg);
                argv_ptr += 8;
            }
            if (new_argv.empty()) new_argv.push_back(path);

            // Clear the JIT cache (the old blocks are invalid after exec).
            if (emu.jit()) emu.jit()->flush_cache();

            // Reload the ELF into the existing Memory. The ElfLoader will
            // map new PT_LOAD segments. Old mappings remain but are
            // overwritten by the new binary's segments.
            auto info = ElfLoader::load(mem_, elf_data);

            // Set up a new initial stack.
            const uint64_t STACK_TOP = 0x8000000000ULL;
            const uint64_t STACK_SIZE = 64 * 1024 * 1024;
            uint64_t stack_base = STACK_TOP - STACK_SIZE;
            // The stack is already mapped from the parent; just reset SP.
            uint64_t sp = STACK_TOP;

            // Push argv strings.
            std::vector<uint64_t> argv_addrs;
            for (auto& a : new_argv) {
                sp -= a.size() + 1;
                mem_.write(sp, a.data(), a.size() + 1);
                argv_addrs.push_back(sp);
            }

            // Push envp (just PATH).
            std::vector<uint64_t> envp_addrs;
            const char* env = "PATH=/bin:/usr/bin:/sbin:/usr/sbin";
            sp -= strlen(env) + 1;
            mem_.write(sp, env, strlen(env) + 1);
            envp_addrs.push_back(sp);

            // AT_RANDOM.
            sp -= 16;
            uint8_t rnd[16] = {0};
            mem_.write(sp, rnd, 16);
            uint64_t random_addr = sp;

            // Build auxv.
            std::vector<uint64_t> auxv = {
                6, 4096,           // AT_PAGESZ
                3, info.phdr_addr, // AT_PHDR
                4, info.phent,     // AT_PHENT
                5, info.phnum,     // AT_PHNUM
                9, info.entry,     // AT_ENTRY
                25, random_addr,   // AT_RANDOM
                16, 0x3ff,         // AT_HWCAP (FP+ASIMD+ATOMICS)
                7, 0,              // AT_BASE
                0, 0,              // AT_NULL
            };

            // Compute total table size and align SP to 16.
            uint64_t argc = new_argv.size();
            uint64_t table_size = 8 + 8 * (argc + 1) + 8 * (envp_addrs.size() + 1) + 8 * auxv.size();
            sp -= table_size;
            sp &= ~0xFULL;

            uint64_t p = sp;
            auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
            push(argc);
            for (auto a : argv_addrs) push(a);
            push(0);
            for (auto e : envp_addrs) push(e);
            push(0);
            for (auto v : auxv) push(v);

            // Set CPU state for the new program.
            cpu.pc = info.entry;
            cpu.sp = sp;
            cpu.pstate = 0;
            cpu.running = true;
            cpu.exit_code = 0;
            memset(cpu.regs, 0, sizeof(cpu.regs));
            memset(cpu.v_lo, 0, sizeof(cpu.v_lo));
            memset(cpu.v_hi, 0, sizeof(cpu.v_hi));
            cpu.tid = static_cast<int>(getpid());

            // Set up a fresh TLS scratch area (like load_elf_file does).
            const uint64_t TLS_SCRATCH_SIZE = 65536;
            uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
            cpu.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
            cpu.tpidrro_el0 = cpu.tpidr_el0;

            // Map the zero page (NULL deref returns 0).
            mem_.map_range(0, 4096);

            // Return 0 to indicate execve succeeded (the syscall doesn't
            // actually return on success — we just set PC to the entry
            // point and continue).
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
            uint32_t op = static_cast<uint32_t>(a1);
            uint32_t val = static_cast<uint32_t>(a2);
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
                        ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EAGAIN)));
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
                    int to_wake = static_cast<int>(val);
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
                    int woken = std::min(static_cast<int>(val), slot->waiters);
                    if (woken >= slot->waiters) slot->cv.notify_all();
                    else for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    ret_host(woken);
                    return 0;
                }
                default:
                    // PI futexes and others: not supported
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
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
            // deliver the signal to the target thread.
            // For now we only handle signals directed at the current
            // thread (tid == cpu.tid). Cross-thread delivery is left
            // to a future version.
            (void)a0;  // tgid
            (void)a1;  // tid
            int sig = static_cast<int>(a2);
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
            int sig = static_cast<int>(a1);
            if (sig == 0) { ret_host(0); return 0; }
            if (sig >= 1 && sig <= MAX_SIGNAL) {
                deliver_signal(emu, cpu, signals_, sig);
            }
            ret_host(0);
            return 0;
        }

        case 129: { // kill(pid, sig)
            int sig = static_cast<int>(a1);
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
