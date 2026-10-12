// Exit.cpp — thread and process termination:
//   - exit (93): ends only the calling thread (musl/glibc pthread_join
//     waits on the thread's tid word, which is cleared and woken here)
//   - exit_group (94): ends every guest thread via Emulator::group_exit
//
// Dispatched from syscall_process() (Process.cpp).
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include <mutex>
namespace arm64emu {
int64_t syscall_exit(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0];
    switch (num) {
        case 93: { // exit — exit current thread (not whole process)
            // On AArch64 Linux, exit(2) (syscall 93) exits only the
            // calling thread. The kernel's do_exit() handles:
            //   1. clear_child_tid zeroing + futex wake (CLONE_CHILD_CLEARTID)
            //   2. thread->tid zeroing (so pthread_join sees tid==0)
            //
            // musl's pthread_join polls thread->tid (at TPIDR_EL0 - 0xa0)
            // via futex WAIT. The kernel zeros this field on thread exit.
            // Our cleanup in thread_entry zeros clear_child_tid but NOT
            // thread->tid. We zero it here so pthread_join can proceed.
            //
            // thread->tid address = TPIDR_EL0 - 0xc8 + 0x28 = TPIDR_EL0 - 0xa0
            // (musl's struct pthread: base = TPIDR_EL0 - 0xc8, tid at +0x28)
            // Leave the glibc DTV attached here: NPTL may cache this TCB and
            // reads its DTV before calling _dl_allocate_tls_init on reuse.
            // That allocator releases emulator-owned dynamic TLS then.
            if (cpu.tpidr_el0 != 0) {
                uint64_t tid_addr = cpu.tpidr_el0 - 0xa0;
                try {
                    emu.mem().store<uint32_t>(tid_addr, 0);
                    // Futex wake on the tid field so pthread_join unblocks.
                    auto* slot = emu.get_futex(tid_addr);
                    {
                        std::lock_guard<std::mutex> lk(slot->mu);
                        slot->cv.notify_all();
                    }
                } catch (...) {
                    // tid_addr unmapped — nothing we can do
                }
            }
            cpu.running = false;
            cpu.exit_code = static_cast<int>(a0);
            return 0;
        }
        case 94: { // exit_group
            // Linux exit_group terminates EVERY thread in the process, not
            // just the caller — including the main thread when a worker
            // calls it. From main, stop + reap the siblings here; from a
            // worker, group_exit stops main and run() reaps the rest.
            if (&cpu == &emu.main_cpu()) emu.kill_other_threads(cpu);
            emu.group_exit(cpu, static_cast<int>(a0));
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
