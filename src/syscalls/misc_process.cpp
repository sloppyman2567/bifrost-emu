// syscalls/misc_process.cpp — process/identity/resource syscalls extracted
// from misc.cpp. (Stub — cases still in misc.cpp. Will be moved incrementally.)
#include "core/emulator.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"

namespace arm64emu {

int64_t syscall_misc_process(Emulator& /*emu*/, CPU& /*cpu*/, uint64_t /*num*/) {
    // TODO: move getpid/getppid/uid/gid/wait/rlimit/prctl cases from misc.cpp.
    return SYSCALL_NOT_HANDLED;
}

} // namespace arm64emu
