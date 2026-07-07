// syscalls/misc_io.cpp — I/O syscalls extracted from misc.cpp.
// (Stub — cases still in misc.cpp. Will be moved incrementally.)
#include "core/emulator.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"

namespace arm64emu {

int64_t syscall_misc_io(Emulator& /*emu*/, CPU& /*cpu*/, uint64_t /*num*/) {
    // TODO: move epoll/eventfd/timerfd/socket/poll cases from misc.cpp.
    return SYSCALL_NOT_HANDLED;
}

} // namespace arm64emu
