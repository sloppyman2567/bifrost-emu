// syscalls/time.cpp — time syscalls: nanosleep/clock_gettime/gettimeofday/
// clock_nanosleep/clock_getres.
//
// Extracted verbatim from the original syscalls.cpp.
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
    (void)a2; (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto ret_host = [&](int64_t r) { cpu.regs[0] = static_cast<uint64_t>(r); };

    switch (num) {
        case 101: { // nanosleep(req, rem) — AArch64 syscall 101
            uint64_t req = a0;
            uint64_t tv_sec  = mem_.load<uint64_t>(req);
            uint64_t tv_nsec = mem_.load<uint64_t>(req + 8);
            struct timespec ts = { (time_t)tv_sec, static_cast<long>(tv_nsec) };
            ::nanosleep(&ts, nullptr);
            ret_host(0);
            return 0;
        }

        case 113: { // clock_gettime
            uint64_t clk = a0;
            uint64_t tp = a1;
            struct timespec ts;
            ::clock_gettime((clockid_t)clk, &ts);
            mem_.store<uint64_t>(tp,     ts.tv_sec);
            mem_.store<uint64_t>(tp + 8, ts.tv_nsec);
            ret_host(0);
            return 0;
        }

        case 169: { // gettimeofday
            struct timeval tv;
            ::gettimeofday(&tv, nullptr);
            mem_.store<uint64_t>(a0,     tv.tv_sec);
            mem_.store<uint64_t>(a0 + 8, tv.tv_usec);
            ret_host(0);
            return 0;
        }

        case 206: { // clock_nanosleep(clockid, flags, req, rem) — aarch64 206
            if (a2) {
                uint64_t sec = mem_.load<uint64_t>(a2);
                uint64_t nsec = mem_.load<uint64_t>(a2 + 8);
                struct timespec ts = { (time_t)sec, static_cast<long>(nsec) };
                ::nanosleep(&ts, nullptr);
            }
            ret_host(0);
            return 0;
        }

        case 114: { // clock_getres — AArch64 114
            if (a1) {
                mem_.store<uint64_t>(a1, 0);       // tv_sec
                mem_.store<uint64_t>(a1 + 8, 1);   // tv_nsec
            }
            ret_host(0);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
