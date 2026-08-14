// syscalls/time.cpp — time syscalls: nanosleep/clock_gettime/gettimeofday/
// clock_getres/clock_nanosleep.
//
// Extracted from the original syscalls.cpp. All handlers now:
//   - Check return values of libc calls (EINTR/EINVAL propagated as -errno).
//   - Wrap guest-pointer reads in try/catch to return EFAULT on bad pointers
//     instead of crashing the emulator with UnmappedMemory.
//   - Use the ret_errno()/ret_err()/ret_ok() macros from syscalls.h for
//     consistent error-return formatting.
//
// AArch64 syscall numbers (per asm-generic/unistd.h):
//   101 = nanosleep         (NOT 35 — that's unlinkat)
//   113 = clock_gettime
//   114 = clock_getres
//   115 = clock_nanosleep   (NOT 206 — that's getsockname, handled in misc.cpp)
//   169 = gettimeofday
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
namespace arm64emu {
// ── vDSO clock fast-path (1.5.2-alpha) ─────────────────────────────────
// Shared by three paths so the clock logic lives in one place:
//   1. Emulator::syscall() — when a SVC's return PC is inside the vDSO
//      range (interpreter + JIT-fallback call it through the dispatcher).
//   2. The JIT native fast path (jit_vdso_clock_svc) — emitted for SVC
//      instructions translated from the vDSO clock stubs, which bypasses
//      the interpreter/step()/syscall dispatch entirely.
// Reads the host clock directly (::clock_gettime/::gettimeofday) and
// writes the result into guest memory, exactly matching syscall_time()'s
// semantics for clock_gettime(113)/clock_getres(114)/gettimeofday(169).
// Returns true if the call was handled (result set in cpu.regs[0]).
bool syscall_vdso_clock(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1];
    auto& mem_ = emu.mem_;
    switch (num) {
        case 113: { // clock_gettime(clkid, tp)
            if (!a1) { ret_err(EFAULT); return true; }
            struct timespec ts;
            memset(&ts, 0, sizeof(ts));
            int r = ::clock_gettime(static_cast<clockid_t>(a0), &ts);
            if (r < 0) {
                try {
                    mem_.store<uint64_t>(a1, 0);
                    mem_.store<uint64_t>(a1 + 8, 0);
                } catch (...) { ret_err(EFAULT); return true; }
                ret_errno();
                return true;
            }
            try {
                mem_.store<uint64_t>(a1,     static_cast<uint64_t>(ts.tv_sec));
                mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(ts.tv_nsec));
            } catch (...) {
                ret_err(EFAULT);
                return true;
            }
            ret_ok();
            return true;
        }
        case 114: { // clock_getres(clkid, res) — 1ns resolution
            if (a1) {
                try {
                    mem_.store<uint64_t>(a1,     0);  // tv_sec
                    mem_.store<uint64_t>(a1 + 8, 1);  // tv_nsec
                } catch (...) {
                    ret_err(EFAULT);
                    return true;
                }
            }
            ret_ok();
            return true;
        }
        case 169: { // gettimeofday(tv, tz)
            if (!a0) { ret_err(EFAULT); return true; }
            struct timeval tv;
            memset(&tv, 0, sizeof(tv));
            int r = ::gettimeofday(&tv, nullptr);
            if (r < 0) {
                try {
                    mem_.store<uint64_t>(a0, 0);
                    mem_.store<uint64_t>(a0 + 8, 0);
                } catch (...) { ret_err(EFAULT); return true; }
                ret_errno();
                return true;
            }
            try {
                mem_.store<uint64_t>(a0,     static_cast<uint64_t>(tv.tv_sec));
                mem_.store<uint64_t>(a0 + 8, static_cast<uint64_t>(tv.tv_usec));
            } catch (...) {
                ret_err(EFAULT);
                return true;
            }
            ret_ok();
            return true;
        }
        default:
            return false;
    }
}
int64_t syscall_time(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;  // a3 is used by clock_nanosleep
    auto& mem_ = emu.mem_;
    static const bool trace = (getenv("BIFROST_SIGNAL_TRACE") != nullptr);
    switch (num) {
        case 101: { // nanosleep(req, rem) — AArch64 101
            if (!a0) { ret_err(EFAULT); return 0; }
            struct timespec ts;
            struct timespec rem;
            try {
                ts.tv_sec  = static_cast<time_t>(mem_.load<uint64_t>(a0));
                ts.tv_nsec = static_cast<long>(mem_.load<uint64_t>(a0 + 8));
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            if (trace) {
                fprintf(stderr, "[signal] nanosleep(%lds, %ldns) called\n",
                        (long)ts.tv_sec, (long)ts.tv_nsec);
            }
            int r = ::nanosleep(&ts, &rem);
            if (r < 0 && errno == EINTR) {
                // Write remaining time to `rem` if provided.
                if (a1) {
                    try {
                        mem_.store<uint64_t>(a1,     static_cast<uint64_t>(rem.tv_sec));
                        mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(rem.tv_nsec));
                    } catch (...) {
                        // Bad rem pointer — still return EINTR.
                    }
                }
                // Pre-set cpu.regs[0] = -EINTR so the signal frame
                // captures it. After sigreturn, cpu.regs[0] = -EINTR.
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                // No signal delivered (SIG_IGN). cpu.regs[0] is -EINTR.
                if (trace) {
                    fprintf(stderr, "[signal] nanosleep returned -EINTR (SIG_IGN)\n");
                }
                return 0;
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_ok();
            return 0;
        }
        case 113: // clock_gettime(clkid, tp) — AArch64 113
        case 114: // clock_getres(clkid, res) — AArch64 114
        case 169: // gettimeofday(tv, tz) — AArch64 169
            // Delegated to the shared vDSO fast-path handler so the
            // direct (non-vDSO) syscall path and the vDSO stubs behave
            // identically.
            if (syscall_vdso_clock(emu, cpu, num)) return 0;
            return SYSCALL_NOT_HANDLED;
        case 115: { // clock_nanosleep(clkid, flags, req, rem) — AArch64 115
            if (!a2) { ret_err(EFAULT); return 0; }
            struct timespec ts;
            struct timespec rem;
            try {
                ts.tv_sec  = static_cast<time_t>(mem_.load<uint64_t>(a2));
                ts.tv_nsec = static_cast<long>(mem_.load<uint64_t>(a2 + 8));
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            int r = ::clock_nanosleep(static_cast<clockid_t>(a0),
                                      static_cast<int>(a1), &ts, &rem);
            // clock_nanosleep returns 0 on success or a *positive* errno
            // (e.g. EINTR=4) — never negative, never -1+errno.
            if (r == EINTR) {
                if (a3) {
                    try {
                        mem_.store<uint64_t>(a3,     static_cast<uint64_t>(rem.tv_sec));
                        mem_.store<uint64_t>(a3 + 8, static_cast<uint64_t>(rem.tv_nsec));
                    } catch (...) {
                        // Bad rem pointer — still return EINTR.
                    }
                }
                // Pre-set cpu.regs[0] = -EINTR, then drain signals.
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                // No signal delivered (SIG_IGN). Return -EINTR.
                return 0;  // cpu.regs[0] is already -EINTR
            }
            if (r != 0) {
                ret_host(static_cast<int64_t>(-r));
                return 0;
            }
            ret_ok();
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
}
} // namespace arm64emu
