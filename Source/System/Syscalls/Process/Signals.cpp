// syscalls/misc_signal.cpp — signal-related syscalls extracted from Miscellaneous.cpp.
// Handles: sigaltstack(132), rt_sigsuspend(133), rt_sigaction(134),
// rt_sigprocmask(135), rt_sigpending(136), rt_sigtimedwait(137),
// rt_sigqueueinfo(138), rt_sigreturn(139).
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/Signals.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Core/DebugFlags.hpp"
#include <errno.h>
#include <signal.h>
#include <time.h>
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
        case 137: { // rt_sigtimedwait(set, info, timeout, sigsetsize)
            // A pending signal from `set` is dequeued and returned without
            // running its handler; otherwise wait for one until `timeout`
            // (NULL = forever). Bionic's sigwait() and ART's signal catcher
            // depend on this blocking: the old EAGAIN stub made them spin
            // forever at ~1M calls/s. Signals outside `set` that are not
            // blocked still run their handlers and end the wait with EINTR.
            if (a3 != 8) { ret_err(EINVAL); return 0; }
            uint64_t wait_set = 0;
            try { wait_set = mem_.load<uint64_t>(a0); }
            catch (...) { ret_err(EFAULT); return 0; }
            wait_set &= ~((1ULL << (BIFROST_SIGKILL - 1)) |
                          (1ULL << (BIFROST_SIGSTOP - 1)));
            bool has_deadline = false;
            struct timespec deadline = {};
            if (a2 != 0) {
                int64_t sec = 0, nsec = 0;
                try {
                    sec = mem_.load<int64_t>(a2);
                    nsec = mem_.load<int64_t>(a2 + 8);
                } catch (...) { ret_err(EFAULT); return 0; }
                if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
                    ret_err(EINVAL);
                    return 0;
                }
                clock_gettime(CLOCK_MONOTONIC, &deadline);
                deadline.tv_sec += sec;
                deadline.tv_nsec += nsec;
                if (deadline.tv_nsec >= 1000000000) {
                    deadline.tv_sec++;
                    deadline.tv_nsec -= 1000000000;
                }
                has_deadline = true;
            }
            // While waiting, `set` counts as blocked, so arriving members
            // are recorded in sigpending instead of being delivered. A
            // handler that interrupts the wait must save the caller's mask
            // (the rt_sigsuspend mechanism), not this temporary one.
            const uint64_t saved_sigmask = cpu.sigmask;
            cpu.sigmask |= wait_set;
            cpu.sigsuspend_saved_mask = saved_sigmask;
            cpu.sigsuspend_active = true;
            auto finish = [&]() {
                cpu.sigmask = saved_sigmask;
                cpu.sigsuspend_active = false;
            };
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
            for (;;) {
                // Cross-thread kill/tgkill entries carry only SI_USER, so
                // folding them into sigpending keeps every pending signal
                // in one place without reordering the producer's queue.
                CPU::PendingSig queued;
                while (cpu.pop_pending(queued, wait_set)) {
                    if (queued.signo >= 1 && queued.signo <= 64)
                        cpu.sigpending.fetch_or(1ULL << (queued.signo - 1));
                }
                const uint64_t ready = cpu.sigpending.load() & wait_set;
                if (ready) {
                    const int signo = __builtin_ctzll(ready) + 1;
                    cpu.sigpending.fetch_and(~(1ULL << (signo - 1)));
                    finish();
                    try { build_siginfo(mem_, a1, signo, SI_USER_EMU, 0); }
                    catch (...) { ret_err(EFAULT); return 0; }
                    ret_host(static_cast<uint64_t>(signo));
                    return 0;
                }
                // Deliver anything else that is unblocked; its frame keeps
                // -EINTR in x0 and the pre-wait mask.
                if (deliver_pending_signals(emu, cpu, signals_) > 0 ||
                    emu.drain_host_signals(cpu))
                    return 0;
                if (cpu.sigpending.load() & wait_set) continue;
                if (!cpu.running) { finish(); return 0; }
                struct timespec slice = {0, 10 * 1000 * 1000};
                if (has_deadline) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    int64_t left = (deadline.tv_sec - now.tv_sec) * 1000000000LL +
                                   (deadline.tv_nsec - now.tv_nsec);
                    if (left <= 0) {
                        finish();
                        ret_err(EAGAIN);
                        return 0;
                    }
                    if (left < slice.tv_nsec) slice.tv_nsec = left;
                }
                // Host signals and shutdown kicks end the sleep early;
                // cross-thread signals are seen within one slice.
                ::nanosleep(&slice, nullptr);
            }
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
            // The host kick is an emulator wakeup, not the guest's signal
            // with the same number. A collector's all-but-restart mask must
            // not prevent queued restart signals or shutdown from waking us.
            Emulator::unblock_host_wakeup(host_mask);
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
                if (dbg().fault_trace) fault_trace_sigreturn(cpu, frame);
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
