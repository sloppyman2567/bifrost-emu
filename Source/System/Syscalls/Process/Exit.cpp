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
#include "FrostJIT/Runtime/PersistentCache.hpp"
#include <mutex>
#include <unistd.h>
namespace arm64emu {
int64_t syscall_exit(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0];
    switch (num) {
        case 93: { // exit — exit current thread (not whole process)
            emu.debugger().exited(cpu, static_cast<int>(a0));
            // exit(2) ends only the calling thread. As in the kernel's mm_release, thread_entry then clears
            // clear_child_tid and wakes it, and that is the one word a joiner may wait on. Write nothing to the TLS
            // block here: TPIDR_EL0 - 0xa0 is musl's detach_state, so clearing it woke pthread_join early, which
            // freed this stack and TLS for a new thread to reuse while this one still had words to write to them.
            cpu.running = false;
            cpu.exit_code = static_cast<int>(a0);
            return 0;
        }
        case 94: { // exit_group
            // A host-fork child owns a copied runtime with vanished host
            // threads and borrowed graphics/audio connections. C++ teardown
            // can wait on copied condition variables or send destruction
            // requests over the parent's display connection. Let the kernel
            // reclaim the child directly, as guest _exit requires.
            if (emu.host_fork_child()) {
                emu.debugger().exited(cpu, static_cast<int>(a0), 0, true);
                PersistentCache::flush_all();  // _exit skips the atexit flush
                ::_exit(static_cast<int>(a0));
            }
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
