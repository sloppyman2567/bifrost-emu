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
// ── Syscall histogram (BIFROST_STATS_PERIOD printout) ──────────────────
// Every syscall increments an atomic counter. The counts are summed and
// printed (top-N with per-second rate) from Emulator::dump_periodic_stats
// so a "where does the time go" run can attribute the SIGPROF "other"
// bucket to specific syscalls without a clean guest exit.
namespace {
constexpr size_t SYSCALL_HIST_MAX = 4096 + 1;  // includes GraphicThunk::SYSCALL_NUMBER (0x1000)
std::atomic<uint64_t> g_syscall_hist[SYSCALL_HIST_MAX]{};
const char* syscall_name(uint64_t num) {
    switch (num) {
        case 23: return "select";      case 40: return "sendto";
        case 41: return "sendmsg";      case 43: return "recvmsg";
        case 45: return "recvfrom";     case 48: return "faccessat";
        case 49: return "chdir";        case 50: return "fchdir";
        case 56: return "openat";       case 57: return "close";
        case 61: return "getdents64";   case 62: return "lseek";
        case 63: return "read";         case 64: return "write";
        case 72: return "pselect6";     case 73: return "ppoll";
        case 78: return "readlinkat";   case 79: return "fstatat";
        case 80: return "fstat";        case 93: return "exit";
        case 94: return "exit_group";   case 96: return "set_tid_address";
        case 98: return "futex";        case 99: return "set_robust_list";
        case 100: return "get_robust_list"; case 129: return "kill";
        case 130: return "tkill";       case 131: return "tgkill";
        case 169: return "gettimeofday"; case 174: return "rt_sigaction";
        case 178: return "gettimeofday_alt"; case 179: return "sysinfo";
        case 198: return "socket";      case 203: return "connect";
        case 214: return "brk";         case 215: return "munmap";
        case 220: return "clone";       case 221: return "execve";
        case 222: return "mmap";        case 435: return "clone3";
        case 226: return "mprotect";    case 229: return "mremap";
        case 232: return "mincore";     case 233: return "madvise";
        case 113: return "clock_gettime"; case 114: return "clock_getres";
        case 172: return "getpid";      case 173: return "gettid";
        case 278: return "prctl";
        default: return nullptr;
    }
}
}  // namespace
void dump_syscall_histogram(double dt) {
    // Copy out then sort a top-N by count.
    struct Entry { uint64_t num; uint64_t count; };
    Entry top[16];
    size_t ntop = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < SYSCALL_HIST_MAX; i++) {
        uint64_t c = g_syscall_hist[i].load(std::memory_order_relaxed);
        if (!c) continue;
        total += c;
        // Insertion-sort into the (small) top list, descending.
        size_t pos = ntop;
        for (size_t j = 0; j < ntop; j++) {
            if (c > top[j].count) { pos = j; break; }
        }
        if (pos < 16) {
            if (ntop < 16) ntop++;
            for (size_t k = ntop - 1; k > pos; k--) top[k] = top[k - 1];
            top[pos] = Entry{i, c};
        }
    }
    if (!ntop) return;
    fprintf(stderr, "[%s] syscalls: %llu total (%.1f/s)\n", CODENAME,
            static_cast<unsigned long long>(total),
            dt > 0 ? total / dt : 0.0);
    for (size_t j = 0; j < ntop; j++) {
        const char* name = syscall_name(top[j].num);
        fprintf(stderr, "[%s]   %3llu %-16s %10llu (%.1f%%, %.1f/s)\n", CODENAME,
                static_cast<unsigned long long>(top[j].num),
                name ? name : "?",
                static_cast<unsigned long long>(top[j].count),
                100.0 * top[j].count / total,
                dt > 0 ? top[j].count / dt : 0.0);
    }
}
// ── Main dispatcher ────────────────────────────────────────────────────
void Emulator::syscall(CPU& cpu) {
    uint64_t num = cpu.regs[8];
    if (num < SYSCALL_HIST_MAX)
        g_syscall_hist[num].fetch_add(1, std::memory_order_relaxed);
    // 1.5.2-alpha: vDSO clock fast-path. The vDSO clock stubs
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
            case 62: name = "lseek"; break;
            case 222: name = "mmap"; break;
            case 215: name = "munmap"; break;
            case 198: name = "socket"; break;
            case 203: name = "connect"; break;
            case 40: name = "sendto"; break;
            case 45: name = "recvfrom"; break;
            case 221: name = "execve"; break;
            case 220: name = "clone"; break;
            case 435: name = "clone3"; break;
            case 93: name = "exit"; break;
            case 94: name = "exit_group"; break;
            case 96: name = "set_tid_address"; break;
            case 179: name = "sysinfo"; break;
            case 98: name = "futex"; break;
            case 23: name = "select"; break;
            case 72: name = "pselect6"; break;
            case 73: name = "ppoll"; break;
            case 41: name = "sendmsg"; break;
            case 43: name = "recvmsg"; break;
            case 99: name = "set_robust_list"; break;
            case 100: name = "get_robust_list"; break;
            case 129: name = "kill"; break;
            case 130: name = "tkill"; break;
            case 131: name = "tgkill"; break;
            default: break;
        }
        if (name) {
            fprintf(stderr, "[syscall t%d] %llu %s a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx a4=0x%llx a5=0x%llx pc=0x%llx",
                    cpu.tid,
                    static_cast<unsigned long long>(num), name,
                    static_cast<unsigned long long>(cpu.regs[0]),
                    static_cast<unsigned long long>(cpu.regs[1]),
                    static_cast<unsigned long long>(cpu.regs[2]),
                    static_cast<unsigned long long>(cpu.regs[3]),
                    static_cast<unsigned long long>(cpu.regs[4]),
                    static_cast<unsigned long long>(cpu.regs[5]),
                    static_cast<unsigned long long>(cpu.pc));
            if (num == 56 || num == 79 || num == 291 || num == 48 || num == 78 || num == 221) {
                uint64_t p = cpu.regs[1];
                char buf[256];
                size_t n = 0;
                while (n < sizeof(buf) - 1) {
                    char c;
                    if (p + n == 0) break;
                    mem().read(p + n, &c, 1);
                    buf[n] = c;
                    n++;
                    if (c == 0) break;
                }
                buf[n] = 0;
                fprintf(stderr, " path=\"%s\"", buf);
            }
            fprintf(stderr, "\n");
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
    if (trace_syscalls) {
        uint64_t prev = cpu.regs[0];
        if (syscall_fs(*this, cpu, num) != SYSCALL_NOT_HANDLED) {
            fprintf(stderr, "  => ret=%lld (0x%llx)\n",
                    (long long)(int64_t)cpu.regs[0], (unsigned long long)cpu.regs[0]);
            return;
        }
        cpu.regs[0] = prev;
    }
    // Hot-path pre-dispatch: jump straight to the owning subsystem for the
    // most frequent calls instead of walking the six-handler chain. The
    // game's per-frame mesh churn hammers mmap/munmap/brk (mem), and GL/SDL
    // thunk calls ride misc on GraphicThunk::SYSCALL_NUMBER (0x1000).
    // Everything else falls through to the generic chain unchanged, so the
    // ownership is provably identical to before.
    switch (num) {
        case 222:   // mmap
        case 215:   // munmap
        case 216:   // mremap
        case 214:   // brk
        case 226:   // mprotect
        case 233:   // madvise
            if (syscall_mem(*this, cpu, num) != SYSCALL_NOT_HANDLED) return;
            break;
        case 98:    // futex
        case 220:   // clone
        case 435:   // clone3
            if (syscall_threads(*this, cpu, num) != SYSCALL_NOT_HANDLED) return;
            break;
        case 0x1000:  // GraphicThunk::SYSCALL_NUMBER (GL/SDL/EGL/Vulkan thunks)
            if (syscall_misc(*this, cpu, num) != SYSCALL_NOT_HANDLED) return;
            break;
        default:
            break;
    }
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
