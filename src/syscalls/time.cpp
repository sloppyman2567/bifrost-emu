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

int64_t syscall_time(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;  // a3 is used by clock_nanosleep
    auto& mem_ = emu.mem_;

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
            if (getenv("BIFROST_SIGNAL_TRACE")) {
                fprintf(stderr, "[signal] nanosleep(%lds, %ldns) called\n",
                        (long)ts.tv_sec, (long)ts.tv_nsec);
            }
            int r = ::nanosleep(&ts, &rem);
            if (getenv("BIFROST_SIGNAL_TRACE")) {
                fprintf(stderr, "[signal] nanosleep returned %d (errno=%d)\n",
                        r, r < 0 ? errno : 0);
            }
            if (r < 0) {
                // EINTR: write remaining time to `rem` if provided.
                // BUGFIX (Turn 42): when nanosleep is interrupted by a
                // host signal (e.g., SIGINT from Ctrl-C), we need to
                // deliver the signal to the guest. The host signal
                // handler already queued it in host_signal_queue_. But
                // we need to return -EINTR so the guest's libc can
                // deliver the pending signal (musl checks for pending
                // signals after EINTR). We also need to call
                // drain_host_signals here so the signal is delivered
                // immediately rather than waiting for the next 4K-
                // instruction check.
                if (errno == EINTR) {
                    emu.drain_host_signals(cpu);
                }
                if (errno == EINTR && a1) {
                    try {
                        mem_.store<uint64_t>(a1,     static_cast<uint64_t>(rem.tv_sec));
                        mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(rem.tv_nsec));
                    } catch (...) {
                        // Bad rem pointer — still return EINTR.
                    }
                }
                ret_errno();
                return 0;
            }
            ret_ok();
            return 0;
        }

        case 113: { // clock_gettime(clkid, tp) — AArch64 113
            if (!a1) { ret_err(EFAULT); return 0; }
            struct timespec ts;
            int r = ::clock_gettime(static_cast<clockid_t>(a0), &ts);
            if (r < 0) { ret_errno(); return 0; }
            try {
                mem_.store<uint64_t>(a1,     static_cast<uint64_t>(ts.tv_sec));
                mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(ts.tv_nsec));
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            ret_ok();
            return 0;
        }

        case 114: { // clock_getres(clkid, res) — AArch64 114
            // musl defaults CLOCK_REALTIME resolution to 1ns.
            if (a1) {
                try {
                    mem_.store<uint64_t>(a1,     0);  // tv_sec
                    mem_.store<uint64_t>(a1 + 8, 1);  // tv_nsec
                } catch (...) {
                    ret_err(EFAULT);
                    return 0;
                }
            }
            ret_ok();
            return 0;
        }

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
            // (e.g. EINTR=4) — never negative, never -1+errno. The old
            // `if (r < 0)` check meant EINTR/EINVAL were never reported;
            // every sleep appeared to succeed. Fix: check `r != 0`.
            if (r != 0) {
                if (r == EINTR && a3) {
                    try {
                        mem_.store<uint64_t>(a3,     static_cast<uint64_t>(rem.tv_sec));
                        mem_.store<uint64_t>(a3 + 8, static_cast<uint64_t>(rem.tv_nsec));
                    } catch (...) {
                        // Bad rem pointer — still return EINTR.
                    }
                }
                ret_host(static_cast<int64_t>(-r));
                return 0;
            }
            ret_ok();
            return 0;
        }

        case 169: { // gettimeofday(tv, tz) — AArch64 169
            if (!a0) { ret_err(EFAULT); return 0; }
            struct timeval tv;
            int r = ::gettimeofday(&tv, nullptr);
            if (r < 0) { ret_errno(); return 0; }
            try {
                mem_.store<uint64_t>(a0,     static_cast<uint64_t>(tv.tv_sec));
                mem_.store<uint64_t>(a0 + 8, static_cast<uint64_t>(tv.tv_usec));
            } catch (...) {
                ret_err(EFAULT);
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
