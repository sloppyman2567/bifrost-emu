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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace arm64emu {
// ── Main dispatcher ────────────────────────────────────────────────────
void Emulator::syscall(CPU& cpu) {
    uint64_t num = cpu.regs[8];
    // v1.5.1-alpha: vDSO clock fast-path. The vDSO clock stubs
    // (gettimeofday/clock_gettime/clock_getres) trap here with the SVC's
    // return PC inside the vDSO mapping. Read the host clock directly and
    // skip the full syscall dispatch (drain_host_signals + six subsystem
    // lookups). This mirrors real Linux, where vDSO clock reads never enter
    // the kernel and therefore never process pending signals either. The JIT
    // additionally bypasses the interpreter for these calls via
    // jit_vdso_clock_svc, but this check covers the interpreter and any
    // JIT-fallback path too.
    if (in_vdso_range(cpu.pc) &&
        (num == 113 || num == 114 || num == 169) &&
        syscall_vdso_clock(*this, cpu, num)) {
        return;
    }
    // Drain pending host-forwarded signals at every syscall boundary.
    // This keeps signal-delivery latency low even when the guest is in
    // a tight syscall loop (e.g., ppoll waiting for SIGCHLD). Without
    // this, signals would only be drained every 4096 instructions in
    // the run loop, which can be hundreds of milliseconds under the
    // interpreter.
    drain_host_signals(cpu);
    // (SIGTERM, SIGKILL, etc.) with no handler, cpu.running is now false
    // and cpu.exit_code is set. We must NOT execute the syscall — the
    // guest has been killed. Continuing would execute the syscall (e.g.,
    // close(fd)) and then return to the JIT, which would run the `ret`
    // block. The ret block reads x30, but if the signal delivery
    // corrupted the call chain (e.g., by setting up a signal frame for
    // a different signal), x30 could be wrong, causing a SIGSEGV at
    // pc=0. More importantly, executing syscalls after the guest is
    // dead is wrong — the guest should not observe any side effects.
    if (!cpu.running) return;
    // Optional syscall trace via BIFROST_SYSCALL_TRACE env var.
    // Cache both flags in thread-local statics so we only call getenv
    // once per thread (getenv is not cheap — it scans environ).
    thread_local bool trace_syscalls = (getenv("BIFROST_SYSCALL_TRACE") != nullptr);
    thread_local bool trace_all = (getenv("BIFROST_SYSCALL_TRACE_ALL") != nullptr);
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
            case 179: name = "sysinfo"; break;
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
        } else if (trace_all) {
            fprintf(stderr, "[syscall t%d] %llu (unknown) a0=0x%llx a1=0x%llx a2=0x%llx pc=0x%llx\n",
                    cpu.tid,
                    static_cast<unsigned long long>(num),
                    static_cast<unsigned long long>(cpu.regs[0]),
                    static_cast<unsigned long long>(cpu.regs[1]),
                    static_cast<unsigned long long>(cpu.regs[2]),
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
