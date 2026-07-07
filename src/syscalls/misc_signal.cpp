// syscalls/misc_signal.cpp — signal-related syscalls extracted from misc.cpp.
// Handles: sigaltstack(132), rt_sigsuspend(133), rt_sigaction(134),
// rt_sigprocmask(135), rt_sigpending(136), rt_sigtimedwait(137),
// rt_sigqueueinfo(138), rt_sigreturn(139).
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"

#include <errno.h>
#include <signal.h>
#include <cstdio>
#include <cstring>

namespace arm64emu {

int64_t syscall_misc_signal(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& signals_ = emu.signals();

    switch (num) {
        case 132: { // sigaltstack(new, old) — AArch64 132
            int r = SignalTable::set_altstack(mem_, cpu, a0, a1);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 136: { // rt_sigpending(sigset, sigsetsize) — AArch64 136
            if (a0 != 0) {
                try {
                    if (a3 == 4) {
                        mem_.store<uint32_t>(a0,
                            static_cast<uint32_t>(cpu.sigpending));
                    } else {
                        mem_.store<uint64_t>(a0, cpu.sigpending);
                    }
                }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }

        case 138: { // rt_sigqueueinfo(tgid, signo, siginfo) — AArch64 138
            ret_host(0);
            return 0;
        }

        case 137: { // rt_sigtimedwait — AArch64 137
            ret_err(EAGAIN);
            return 0;
        }

        case 133: { // rt_sigsuspend(mask, sigsetsize) — AArch64 133
            uint64_t guest_mask = 0;
            if (a0 != 0) {
                try { guest_mask = mem_.load<uint64_t>(a0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            const uint64_t saved_sigmask = cpu.sigmask;
            cpu.sigmask = guest_mask & ~((1ULL << (BIFROST_SIGKILL - 1)) |
                                          (1ULL << (BIFROST_SIGSTOP - 1)));
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
            if (cpu.sigpending != 0) {
                if (deliver_pending_signals(emu, cpu, signals_) > 0)
                    return 0;
            }
            if (emu.handle_eintr(cpu)) return 0;
            sigset_t host_mask;
            sigemptyset(&host_mask);
            for (int signo = 1; signo <= 31; signo++) {
                if ((guest_mask >> (signo - 1)) & 1)
                    sigaddset(&host_mask, signo);
            }
            ::sigsuspend(&host_mask);
            const bool delivered = emu.handle_eintr(cpu);
            if (!delivered) cpu.sigmask = saved_sigmask;
            return 0;
        }

        case 134: { // rt_sigaction(signo, new_act, old_act, sigsetsize)
            int signo = static_cast<int>(a0);
            int r = signals_.install(mem_, signo, a1, a2);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 135: { // rt_sigprocmask(how, new_set, old_set, sigsetsize)
            static const bool trace = (getenv("BIFROST_SIGNAL_TRACE") != nullptr);
            int r = SignalTable::procmask(mem_, cpu, static_cast<int>(a0),
                                          a1, a2, static_cast<size_t>(a3));
            if (trace) {
                uint64_t new_mask_val = 0;
                if (a1 != 0) {
                    try { new_mask_val = mem_.load<uint64_t>(a1); } catch (...) {}
                }
                fprintf(stderr, "[signal] rt_sigprocmask how=%lld "
                        "newmask=0x%llx r=%d cpu.sigmask=0x%llx "
                        "cpu.sigpending=0x%llx\n",
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(new_mask_val),
                        r,
                        static_cast<unsigned long long>(cpu.sigmask),
                        static_cast<unsigned long long>(cpu.sigpending));
            }
            bool signal_delivered = false;
            if (r == 0 && cpu.sigpending != 0) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
                if (deliver_pending_signals(emu, cpu, signals_) > 0)
                    signal_delivered = true;
            }
            if (!signal_delivered)
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 139: { // rt_sigreturn — restore CPU state from signal frame
            SignalFrame frame;
            if (signals_.pop_frame(frame)) {
                memcpy(cpu.regs, frame.regs, sizeof(frame.regs));
                cpu.regs[31] = 0;
                cpu.sp     = frame.sp;
                cpu.pc     = frame.pc;
                cpu.pstate = frame.pstate;
                cpu.sigmask = frame.saved_mask;
                memcpy(cpu.v_lo, frame.v_lo, sizeof(cpu.v_lo));
                memcpy(cpu.v_hi, frame.v_hi, sizeof(cpu.v_hi));
                cpu.fpcr = frame.fpcr;
                cpu.fpsr = frame.fpsr;
                if (frame.on_altstack)
                    SignalTable::set_altstack_active(cpu, false);
                if (cpu.sigpending != 0)
                    deliver_pending_signals(emu, cpu, signals_);
                return 0;
            }
            ret_host(0);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
}

} // namespace arm64emu
