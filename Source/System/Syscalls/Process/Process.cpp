// Process.cpp — dispatcher for process, identity and resource syscalls.
// Each family owns a disjoint set of syscall numbers:
//
//   - Scheduling.cpp — ptrace, sched_*, setpriority/getpriority, getpgid, getcpu
//   - Identity.cpp   — getuid/getpid/uname/sysinfo/prctl/getrandom/keyring/
//                      unshare/etc
//   - Wait.cpp       — times, waitpid, wait4, waitid
//   - Exit.cpp       — exit, exit_group
//
// Each family returns SYSCALL_NOT_HANDLED if it doesn't recognize `num`;
// otherwise it sets cpu.regs[0] and returns 0. These files use the public
// Emulator accessors (emu.mem(), emu.fds(), ...), not friend access.
#include "System/Core/Emulator.hpp"
#include "System/Core/CPU.hpp"
#include "System/Syscalls/Syscalls.hpp"
namespace arm64emu {
int64_t syscall_process(Emulator& emu, CPU& cpu, uint64_t num) {
    if (syscall_scheduling(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_identity(emu, cpu, num)   != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_wait(emu, cpu, num)       != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_exit(emu, cpu, num)       != SYSCALL_NOT_HANDLED) return 0;
    return SYSCALL_NOT_HANDLED;
}
} // namespace arm64emu
