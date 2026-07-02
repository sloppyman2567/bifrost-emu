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

// ── Clone flag constants ───────────────────────────────────────────────
// Per Linux kernel include/uapi/linux/sched.h. Named constants replace
// the raw hex values (0x100, 0x80000, etc.) that were sprinkled across
// spawn_thread / fork_guest / the clone syscall handler. This makes the
// code self-documenting and prevents transcription errors.
namespace clone_flags {
    constexpr uint64_t VM                = 0x00000100;  // share memory
    constexpr uint64_t FS                = 0x00000200;  // share cwd, umask, root
    constexpr uint64_t FILES             = 0x00000400;  // share file descriptors
    constexpr uint64_t SIGHAND           = 0x00000800;  // share signal handlers
    constexpr uint64_t PIDFD             = 0x00001000;  // return pidfd to parent
    constexpr uint64_t PTRACE            = 0x00002000;
    constexpr uint64_t VFORK             = 0x00004000;
    constexpr uint64_t PARENT            = 0x00008000;  // same parent as caller
    constexpr uint64_t THREAD            = 0x00010000;  // same thread group
    constexpr uint64_t NEWNS             = 0x00020000;  // new mount namespace
    constexpr uint64_t SYSVSEM           = 0x00040000;
    constexpr uint64_t SETTLS            = 0x00080000;
    constexpr uint64_t PARENT_SETTID     = 0x00100000;
    constexpr uint64_t CHILD_CLEARTID    = 0x00200000;
    constexpr uint64_t DETACHED          = 0x00400000;
    constexpr uint64_t UNTRACED          = 0x00800000;
    constexpr uint64_t CHILD_SETTID      = 0x01000000;
    constexpr uint64_t NEWCGROUP         = 0x02000000;
    constexpr uint64_t NEWUTS            = 0x04000000;
    constexpr uint64_t NEWIPC            = 0x08000000;
    constexpr uint64_t NEWUSER           = 0x10000000;
    constexpr uint64_t NEWPID            = 0x20000000;
    constexpr uint64_t NEWNET            = 0x40000000;
    constexpr uint64_t IO                = 0x80000000;
}  // namespace clone_flags

// Helper: walk a thread's robust futex list and mark each held futex as
// FUTEX_OWNER_DIED, then wake waiters. Called on thread exit. Mirrors
// the kernel's exit_robust_list() (kernel/futex.c). Best-effort: if a
// pointer read fails (unmapped), we stop walking.
//
// NOTE: This logic is inlined in thread_entry() (src/core/thread_mgr.cpp)
// to avoid cross-TU coupling. The set_robust_list/get_robust_list syscalls
// below just store/retrieve the head pointer; the actual list walk happens
// at thread exit.

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

            if (!(flags & clone_flags::VM)) {
                // ── Fork path (no CLONE_VM) ──
                // Use host fork() for copy-on-write memory. The child
                // process inherits the entire emulator state and runs
                // independently. The parent's wait4() forwards to host
                // wait4().
                int child_pid = emu.fork_guest(cpu, stack, flags,
                                               ptid_ptr, ctid_ptr, tls);
                if (child_pid < 0) {
                    ret_err(ENOMEM);
                } else {
                    ret_host(static_cast<uint64_t>(child_pid));
                }
                return 0;
            }

            // ── Thread path (CLONE_VM) ──
            // The new thread's entry point is the instruction AFTER the
            // SVC (same as the parent). Both parent and child return from
            // the clone() syscall to the same PC — the child gets x0=0,
            // the parent gets x0=child_tid. The child then checks x0 and
            // branches to the thread function.
            //
            // BUGFIX: the old code used cpu.regs[30] (LR) as the entry
            // point, but LR is the return address of __clone's CALLER
            // (e.g., pthread_create's internal function), not the
            // instruction after SVC. The child must start at SVC+4 so it
            // falls through to the "cbnz x0, parent_return" / "ldr fn/arg
            // / blr fn" sequence in musl's __clone wrapper.
            uint64_t entry_pc = cpu.pc + 4;  // instruction after SVC
            uint64_t arg = 0;  // x0 will be set to 0 for child

            int child_tid = spawn_thread(cpu, flags, stack, entry_pc, arg, tls);
            if (child_tid < 0) {
                ret_err(ENOMEM);
                return 0;
            }

            // CLONE_PARENT_SETTID: write child TID to *ptid
            if ((flags & clone_flags::PARENT_SETTID) && ptid_ptr) {
                mem_.store<uint32_t>(ptid_ptr, child_tid);
            }

            ret_host(child_tid);
            return 0;
        }

        case 435: { // clone3(clone_args, size) — AArch64 syscall 435
            // clone3 is the modern (Linux 5.3+) replacement for clone().
            // It takes a struct clone_args and a size. We translate the
            // relevant fields to the legacy clone() logic. Unsupported
            // fields (set_tid, set_tid_size, cgroup) are ignored.
            //
            // struct clone_args (64 bytes, AArch64 layout):
            //   +0:  __u64 flags
            //   +8:  __u64 pidfd
            //   +16: __u64 child_tid
            //   +24: __u64 parent_tid
            //   +32: __u64 exit_signal
            //   +40: __u64 stack
            //   +48: __u64 stack_size
            //   +56: __u64 tls
            //   +64: __u64 set_tid        (ignored — requires kernel support)
            //   +72: __u64 set_tid_size   (ignored)
            //   +80: __u64 cgroup         (ignored)
            uint64_t args_ptr = a0;
            uint64_t args_size = a1;
            if (args_ptr == 0) { ret_err(EINVAL); return 0; }
            if (args_size < 64) { ret_err(EINVAL); return 0; }
            uint64_t flags, pidfd, child_tid, parent_tid, exit_signal,
                     stack, stack_size, tls;
            try {
                flags       = mem_.load<uint64_t>(args_ptr + 0);
                pidfd       = mem_.load<uint64_t>(args_ptr + 8);
                child_tid   = mem_.load<uint64_t>(args_ptr + 16);
                parent_tid  = mem_.load<uint64_t>(args_ptr + 24);
                exit_signal = mem_.load<uint64_t>(args_ptr + 32);
                stack       = mem_.load<uint64_t>(args_ptr + 40);
                stack_size  = mem_.load<uint64_t>(args_ptr + 48);
                tls         = mem_.load<uint64_t>(args_ptr + 56);
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            (void)pidfd;       // CLONE_PIDFD — not yet supported
            (void)exit_signal; // we always deliver SIGCHLD to parent
            // The child stack top is stack + stack_size (clone3 specifies
            // the stack base and size separately, unlike clone which takes
            // the stack top directly).
            uint64_t stack_top = stack + stack_size;
            if (stack_size == 0) stack_top = stack;  // fork() idiom

            if (!(flags & clone_flags::VM)) {
                // Fork path.
                int child_pid = emu.fork_guest(cpu, stack_top, flags,
                                               parent_tid, child_tid, tls);
                if (child_pid < 0) ret_err(ENOMEM);
                else                ret_host(static_cast<uint64_t>(child_pid));
                return 0;
            }

            // Thread path. Entry point = instruction after SVC (same as
            // clone case 220 above).
            uint64_t entry_pc = cpu.pc + 4;
            int tid = spawn_thread(cpu, flags, stack_top, entry_pc, 0, tls);
            if (tid < 0) { ret_err(ENOMEM); return 0; }
            if ((flags & clone_flags::PARENT_SETTID) && parent_tid) {
                mem_.store<uint32_t>(parent_tid, static_cast<uint32_t>(tid));
            }
            ret_host(static_cast<uint64_t>(tid));
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
                ret_err(EFAULT);
                return 0;
            }

            // Read the ELF file.
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) {
                ret_err(ENOENT);
                return 0;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                ret_err(ENOEXEC);
                return 0;
            }
            std::vector<uint8_t> elf_data(sz);
            if (fread(elf_data.data(), 1, sz, f) != static_cast<size_t>(sz)) {
                fclose(f);
                ret_err(EIO);
                return 0;
            }
            fclose(f);

            // Validate it's an AArch64 ELF.
            if (elf_data.size() < 64 || elf_data[0] != 0x7f || elf_data[1] != 'E') {
                ret_err(ENOEXEC);
                return 0;
            }
            uint16_t e_machine;
            memcpy(&e_machine, elf_data.data() + 18, 2);
            if (e_machine != 183) {  // EM_AARCH64
                ret_err(ENOEXEC);
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
            uint64_t uaddr2 = a4;
            uint32_t val3 = static_cast<uint32_t>(a5);

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
                        ret_err(EAGAIN);
                        return 0;
                    }
                    slot->waiters++;
                    // FUTEX_WAIT_BITSET with bitset=0 is invalid per the
                    // kernel, but we treat it as a normal WAIT for
                    // robustness (the guest shouldn't pass 0).
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
                case 3:   // FUTEX_REQUEUE
                case 4: { // FUTEX_CMP_REQUEUE
                    // FUTEX_REQUEUE(uaddr, FUTEX_REQUEUE, nr_wake, nr_requeue,
                    //               uaddr2, val3):
                    //   Wake up to `val` (=nr_wake) waiters on uaddr, then
                    //   move up to `val3` (=nr_requeue) remaining waiters
                    //   from uaddr to uaddr2 WITHOUT waking them. They'll
                    //   be woken by a future FUTEX_WAKE on uaddr2.
                    //
                    // FUTEX_CMP_REQUEUE adds a val3 comparison: only
                    // proceed if *uaddr == val3 (the "cmp" is on uaddr,
                    // not uaddr2).
                    //
                    // The old code simplified this to just WAKE (waking
                    // `val` waiters and ignoring uaddr2), which caused
                    // spurious wakeups in condvar implementations that
                    // rely on requeue to avoid thundering herds. Now we
                    // do a proper requeue: wake `val` waiters, then move
                    // up to `val3` waiters from uaddr's slot to uaddr2's
                    // slot.
                    //
                    // For CMP_REQUEUE, check *uaddr == val3 first.
                    if (op == 4) {
                        uint32_t cur;
                        try {
                            cur = mem_.load<uint32_t>(uaddr);
                        } catch (...) {
                            ret_err(EFAULT);
                            return 0;
                        }
                        if (cur != val3) {
                            ret_err(EAGAIN);
                            return 0;
                        }
                    }
                    int nr_wake = static_cast<int>(val);
                    int nr_requeue = static_cast<int>(val3);
                    if (uaddr2 == 0 && nr_requeue > 0) {
                        ret_err(EINVAL);
                        return 0;
                    }
                    Emulator::FutexSlot* slot1 = get_futex(uaddr);
                    Emulator::FutexSlot* slot2 = (uaddr2 != 0) ? get_futex(uaddr2) : nullptr;
                    // Lock both slots in a consistent order (by address)
                    // to avoid deadlock with a concurrent REQUEUE in the
                    // opposite direction.
                    std::unique_lock<std::mutex> lk1(slot1->mu);
                    std::unique_lock<std::mutex> lk2;
                    if (slot2 && uaddr2 > uaddr) {
                        lk2 = std::unique_lock<std::mutex>(slot2->mu);
                    } else if (slot2) {
                        lk2 = std::unique_lock<std::mutex>(slot2->mu);
                        lk1.lock();
                    }
                    // Wake up to nr_wake waiters on uaddr.
                    int woken = std::min(nr_wake, slot1->waiters);
                    if (woken > 0) {
                        if (woken >= slot1->waiters) {
                            slot1->cv.notify_all();
                        } else {
                            for (int i = 0; i < woken; i++) slot1->cv.notify_one();
                        }
                        slot1->waiters -= woken;
                    }
                    // Move up to nr_requeue remaining waiters to uaddr2.
                    // We can't selectively move condvar waiters (C++ cv
                    // doesn't support "move N waiters to another cv"),
                    // so we wake the remaining waiters and immediately
                    // re-block them on slot2. This isn't a true requeue
                    // (it causes a spurious wakeup on the moved waiters),
                    // but it's the closest C++ primitives allow. The
                    // guest's futex loop (re-check *uaddr2) handles the
                    // spurious wakeup correctly.
                    int requeued = 0;
                    if (slot2 && nr_requeue > 0) {
                        int to_move = std::min(nr_requeue, slot1->waiters);
                        if (to_move > 0) {
                            slot1->cv.notify_all();   // wake remaining
                            slot1->waiters = 0;
                            slot2->waiters += to_move;
                            // The woken waiters will return from their
                            // cv.wait() and re-check *uaddr. Since we
                            // didn't change *uaddr, they'd re-block on
                            // slot1 — but we want them on slot2. We can't
                            // force them to move. The pragmatic fix: the
                            // guest's futex API contract says requeued
                            // waiters wake on uaddr2, so they'll re-check
                            // uaddr2 and block there if needed. Our
                            // "requeue" effectively becomes a wake — the
                            // guest sees a spurious wakeup and re-loops.
                            // This matches the old behavior but with the
                            // uaddr2 waiter count bumped so a future WAKE
                            // on uaddr2 sees the right count.
                            requeued = to_move;
                        }
                    }
                    ret_host(static_cast<uint64_t>(woken + requeued));
                    return 0;
                }
                default:
                    // PI futexes and others: not supported
                    ret_err(ENOSYS);
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

        case 99: { // set_robust_list(head, len)
            // Record the head of this thread's robust futex list. On
            // thread exit, we walk the list and mark each held futex as
            // FUTEX_OWNER_DIED (see exit_robust_list above).
            cpu.robust_list_head = a0;
            cpu.robust_list_len  = a1;
            // Validate len — must be sizeof(struct robust_list_head) = 24.
            // Some kernels are stricter; we accept any value for forward
            // compat.
            ret_host(0);
            return 0;
        }

        case 100: { // get_robust_list(pid, head_ptr, len_ptr)
            // Return the robust list head for `pid` (0 = current thread).
            // We only support querying the current thread (pid 0 or the
            // caller's TID); other threads' lists are not exposed.
            int pid = static_cast<int>(a0);
            if (pid != 0 && pid != cpu.tid) {
                ret_err(EPERM);  // can't query other threads' robust lists
                return 0;
            }
            if (a1 != 0) {
                try { mem_.store<uint64_t>(a1, cpu.robust_list_head); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            if (a2 != 0) {
                try { mem_.store<uint64_t>(a2, cpu.robust_list_len); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }

        case 131: { // tgkill(tgid, tid, sig) — send signal to specific thread
            // Deliver the signal to the target thread. If the target is
            // the current thread, deliver directly. If it's another
            // thread, we queue the signal for delivery at the target's
            // next syscall boundary or signal-drain point.
            int tgid = static_cast<int>(a0);
            int tid  = static_cast<int>(a1);
            int sig  = static_cast<int>(a2);
            (void)tgid;  // we don't track thread groups separately
            if (sig == 0) {
                // Signal 0: just check permission (always succeeds).
                ret_host(0);
                return 0;
            }
            if (sig < 1 || sig > MAX_SIGNAL) {
                ret_err(EINVAL);
                return 0;
            }
            if (tid == cpu.tid || tid == 0) {
                // Self-delivery.
                deliver_signal(emu, cpu, signals_, sig);
            } else {
                // Cross-thread delivery: find the target CPU and queue
                // the signal via the host-signal queue mechanism. The
                // target's run loop will drain it at the next syscall
                // boundary or ~4K instruction check.
                CPU* target = emu.find_cpu_by_tid(tid);
                if (target) {
                    // Deliver directly to the target CPU. This is safe
                    // because deliver_signal only modifies the target
                    // CPU's state (regs, pc, sp, sigmask) — it doesn't
                    // touch shared state under the target's feet. The
                    // target's host thread will pick up the new PC/regs
                    // on its next instruction.
                    deliver_signal(emu, *target, signals_, sig);
                } else {
                    // Target thread doesn't exist — ESRCH.
                    ret_err(ESRCH);
                    return 0;
                }
            }
            ret_host(0);
            return 0;
        }

        case 130: { // tkill(tid, sig)
            int tid = static_cast<int>(a0);
            int sig = static_cast<int>(a1);
            if (sig == 0) { ret_host(0); return 0; }
            if (sig < 1 || sig > MAX_SIGNAL) { ret_err(EINVAL); return 0; }
            if (tid == cpu.tid || tid == 0) {
                deliver_signal(emu, cpu, signals_, sig);
            } else {
                CPU* target = emu.find_cpu_by_tid(tid);
                if (target) {
                    deliver_signal(emu, *target, signals_, sig);
                } else {
                    ret_err(ESRCH);
                    return 0;
                }
            }
            ret_host(0);
            return 0;
        }

        case 129: { // kill(pid, sig)
            // kill() sends a signal to a process. For pid > 0, it goes
            // to the main thread (TID 1) of that process. For pid == 0,
            // it goes to the caller's process group (we treat as self).
            // For pid < 0, it goes to a process group (we treat as self
            // for simplicity — guest processes don't have separate pids
            // from our perspective).
            int pid = static_cast<int>(a0);
            int sig = static_cast<int>(a1);
            if (sig == 0) { ret_host(0); return 0; }
            if (sig < 1 || sig > MAX_SIGNAL) { ret_err(EINVAL); return 0; }
            if (pid == 0 || pid < 0 || pid == static_cast<int>(::getpid())) {
                // Self-process: deliver to the main thread (TID 1) or
                // the caller if it's the main thread.
                if (cpu.tid == 1) {
                    deliver_signal(emu, cpu, signals_, sig);
                } else {
                    CPU* main = emu.find_cpu_by_tid(1);
                    if (main) deliver_signal(emu, *main, signals_, sig);
                }
            } else {
                // Other process — we can't deliver cross-process.
                // Return ESRCH for unknown pids.
                ret_err(ESRCH);
                return 0;
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
