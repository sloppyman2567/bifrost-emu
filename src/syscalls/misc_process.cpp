// syscalls/misc_process.cpp — dispatcher for process/identity/resource
// syscalls extracted from misc.cpp (Turn 68 refactor).
//
// Turn 69 refactor: the 40 individual cases that used to live here have
// been split into 3 sub-handlers by topic. syscall_misc_process() now
// only dispatches to the sub-handlers in order:
//
//   - misc_sched.cpp  (10 cases) — ptrace, sched_*, setpriority/getpriority,
//                                   getpgid, getcpu
//   - misc_id.cpp     (27 cases) — getuid/getpid/uname/sysinfo/prctl/
//                                   getrandom/keyring/etc
//   - misc_wait.cpp   (3 cases)  — times, waitpid, wait4
//
// Each sub-handler returns SYSCALL_NOT_HANDLED if it doesn't recognize
// `num`; otherwise it sets cpu.regs[0] and returns 0. This file just
// chains them together and returns SYSCALL_NOT_HANDLED if none match.
//
// NOTE: this file is NOT a friend of Emulator (unlike misc.cpp). It accesses
// private state via the public accessors emu.mem(), emu.fds(), etc.
#include "core/emulator.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"

namespace arm64emu {

int64_t syscall_misc_process(Emulator& emu, CPU& cpu, uint64_t num) {
    // Dispatch to the 3 sub-handlers in order. Each returns
    // SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
    if (syscall_misc_sched(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_misc_id(emu, cpu, num)    != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_misc_wait(emu, cpu, num)  != SYSCALL_NOT_HANDLED) return 0;
    return SYSCALL_NOT_HANDLED;
}

} // namespace arm64emu
