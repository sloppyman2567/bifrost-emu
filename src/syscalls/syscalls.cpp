// syscalls/syscalls.cpp — Linux AArch64 syscall dispatcher.
//
// Implements Emulator::syscall(), which is called when the guest executes
// SVC #0. The dispatcher reads the syscall number from cpu.regs[8] and
// delegates to one of the subsystem handlers:
//
//   syscall_fs      (fs.cpp)        — file ops: openat/close/read/write/dup/...
//   syscall_mem     (mem.cpp)       — mmap/munmap/mremap/mprotect/brk
//   syscall_threads (threads.cpp)   — clone/clone3/futex/set_tid_address
//   syscall_time    (time.cpp)      — clock_gettime/gettimeofday/nanosleep
//   syscall_ioctls  (ioctls.cpp)    — ioctl (fb, termios, etc.)
//   syscall_misc    (this file)     — everything else (uname/getpid/prctl/...)
//
// Each handler returns SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
// The dispatcher tries them in order; the first one that handles the call
// wins. Unhandled syscalls fall through to a -ENOSYS return.
//
// When adding a new syscall:
//   1. Pick the right subsystem file (fs/mem/threads/time/ioctls/misc).
//   2. Add a case in that file's switch.
//   3. Document the syscall number (AArch64 numbering).
//   4. Add a test if possible.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include "syscalls/syscalls.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syscall.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace arm64emu {

// ── Main dispatcher ────────────────────────────────────────────────────
void Emulator::syscall(CPU& cpu) {
    uint64_t num = cpu.regs[8];

    // Try each subsystem handler in order. The first one that handles
    // the call returns 0 (with the result already in cpu.regs[0]).
    if (syscall_fs(*this, cpu, num)      != SYSCALL_NOT_HANDLED) return;
    if (syscall_mem(*this, cpu, num)     != SYSCALL_NOT_HANDLED) return;
    if (syscall_threads(*this, cpu, num) != SYSCALL_NOT_HANDLED) return;
    if (syscall_time(*this, cpu, num)    != SYSCALL_NOT_HANDLED) return;
    if (syscall_ioctls(*this, cpu, num)  != SYSCALL_NOT_HANDLED) return;
    if (syscall_misc(*this, cpu, num)    != SYSCALL_NOT_HANDLED) return;

    // Completely unknown syscall — return -ENOSYS.
    cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS));
}

} // namespace arm64emu
