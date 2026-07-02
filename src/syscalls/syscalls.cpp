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

    // Drain pending host-forwarded signals at every syscall boundary.
    // This keeps signal-delivery latency low even when the guest is in
    // a tight syscall loop (e.g., ppoll waiting for SIGCHLD). Without
    // this, signals would only be drained every 4096 instructions in
    // the run loop, which can be hundreds of milliseconds under the
    // interpreter.
    drain_host_signals(cpu);

    // Optional syscall trace via BIFROST_SYSCALL_TRACE env var.
    static bool trace_syscalls = (getenv("BIFROST_SYSCALL_TRACE") != nullptr);
    if (trace_syscalls) {
        // Read path string for path-based syscalls (56=openat, 79=fstatat, etc.)
        const char* name = nullptr;
        switch (num) {
            case 56: name = "openat"; break;
            case 57: name = "close"; break;
            case 63: name = "read"; break;
            case 64: name = "write"; break;
            case 79: name = "fstatat"; break;
            case 61: name = "getdents64"; break;
            case 291: name = "statx"; break;
            case 78: name = "readlinkat"; break;
            case 48: name = "faccessat"; break;
            case 49: name = "chdir"; break;
            case 50: name = "fchdir"; break;
            case 80: name = "fstat"; break;
            case 221: name = "execve"; break;
            case 220: name = "clone"; break;
            case 435: name = "clone3"; break;
            case 93: name = "exit"; break;
            case 94: name = "exit_group"; break;
            case 96: name = "set_tid_address"; break;
            case 98: name = "futex"; break;
            case 99: name = "set_robust_list"; break;
            case 100: name = "get_robust_list"; break;
            case 129: name = "kill"; break;
            case 130: name = "tkill"; break;
            case 131: name = "tgkill"; break;
            default: break;
        }
        if (name) {
            fprintf(stderr, "[syscall t%d] %llu %s a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx a4=0x%llx a5=0x%llx pc=0x%llx\n",
                    cpu.tid,
                    static_cast<unsigned long long>(num), name,
                    static_cast<unsigned long long>(cpu.regs[0]),
                    static_cast<unsigned long long>(cpu.regs[1]),
                    static_cast<unsigned long long>(cpu.regs[2]),
                    static_cast<unsigned long long>(cpu.regs[3]),
                    static_cast<unsigned long long>(cpu.regs[4]),
                    static_cast<unsigned long long>(cpu.regs[5]),
                    static_cast<unsigned long long>(cpu.pc));
        }
    }

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
