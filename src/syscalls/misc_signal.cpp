// syscalls/misc_signal.cpp — signal-related syscalls extracted from misc.cpp.
// (Stub — cases still in misc.cpp. Will be moved incrementally.)
#include "core/emulator.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"

namespace arm64emu {

int64_t syscall_misc_signal(Emulator& /*emu*/, CPU& /*cpu*/, uint64_t /*num*/) {
    // TODO: move signal cases (132-139) from misc.cpp to here.
    return SYSCALL_NOT_HANDLED;
}

} // namespace arm64emu
