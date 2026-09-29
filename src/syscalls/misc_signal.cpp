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
                            static_cast<uint32_t>(cpu.sigpending.load()));
                    } else {
                        mem_.store<uint64_t>(a0, cpu.sigpending.load());
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
            // A caught signal must leave the handler running (and
            // rt_sigreturn restoring) the PRE-sigsuspend mask, not the
            // temporary mask passed to this call. deliver_signal reads
            // these when it builds the frame.
            cpu.sigsuspend_saved_mask = saved_sigmask;
            cpu.sigsuspend_active = true;
            cpu.sigmask = guest_mask & ~((1ULL << (BIFROST_SIGKILL - 1)) |
                                          (1ULL << (BIFROST_SIGSTOP - 1)));
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
            if (cpu.sigpending.load() != 0) {
                if (deliver_pending_signals(emu, cpu, signals_) > 0)
                    return 0;
            }
            if (emu.handle_eintr(cpu)) return 0;
            sigset_t host_mask;
            sigemptyset(&host_mask);
            // Full 1..64: the guest mask is 64-bit (RT signals live at
            // 32..64); the old 1..31 loop dropped timer/cancel wakes.
            for (int signo = 1; signo <= 64; signo++) {
                if ((guest_mask >> (signo - 1)) & 1)
                    sigaddset(&host_mask, signo);
            }
            ::sigsuspend(&host_mask);
            const bool delivered = emu.handle_eintr(cpu);
            if (!delivered) {
                cpu.sigmask = saved_sigmask;
                cpu.sigsuspend_active = false;
            }
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
                        static_cast<unsigned long long>(cpu.sigpending.load()));
            }
            bool signal_delivered = false;
            if (r == 0 && cpu.sigpending.load() != 0) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
                if (deliver_pending_signals(emu, cpu, signals_) > 0)
                    signal_delivered = true;
            }
            if (!signal_delivered)
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }
        case 139: { // rt_sigreturn — SP points at {siginfo, ucontext}
            SignalFrame frame;
            if ((cpu.sp & 15) || cpu.sp > UINT64_MAX - 128 ||
                !restore_ucontext(mem_, cpu, cpu.sp + 128)) {
                // A bad signal frame raises SIGSEGV, rather than returning
                // a syscall error or silently using the private snapshot.
                signals_.pop_frame(cpu, frame);
                cpu.sigmask &= ~(1ULL << (BIFROST_SIGSEGV - 1));
                deliver_signal(emu, cpu, signals_, BIFROST_SIGSEGV,
                               SI_KERNEL_EMU);
                return 0;
            }
            if (signals_.pop_frame(cpu, frame)) {
                // TLS has no slot in the baseline AArch64 ucontext ABI.
                cpu.tpidr_el0 = frame.tpidr_el0;
                cpu.tpidrro_el0 = frame.tpidrro_el0;
            }
            if (cpu.sigpending.load() != 0)
                deliver_pending_signals(emu, cpu, signals_);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
}
} // namespace arm64emu
