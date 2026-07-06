// syscalls/misc.cpp — catch-all syscall handler for syscalls that don't
// fit into fs/mem/threads/time/ioctls. Includes:
//   - ppoll / pselect6 (case 73, 72, 168)
//   - exit / exit_group (case 93, 94)
//   - getrusage (case 165)
//   - prctl (case 167)
//   - rt_sigaction / rt_sigprocmask / rt_sigsuspend / rt_sigreturn / rt_sigpending
//     (case 134, 135, 133, 139, 213)
//   - uname (case 160)
//   - epoll_create1 / epoll_ctl / epoll_pwait / epoll_wait (case 20, 21, 22, 232)
//   - eventfd2 (case 19, 270)
//   - timerfd_create / timerfd_settime / timerfd_gettime (case 85, 86, 87)
//   - getrandom (case 278)
//   - getrlimit / prlimit64 (case 163, 261)
//   - getpid / getppid / getuid / geteuid / getgid / getegid / gettid
//     (case 172, 173, 174, 175, 176, 177, 178)
//   - rseq (case 293)
//   - socket / socketpair / bind / listen / accept / connect (case 198-203)
//   - umask (case 166)
//   - wait4 / waitid / waitpid (case 260, 272, 247)
//   - sched_yield / sched_getaffinity / sched_setaffinity (case 155, 158, 124)
//   - sethostname / ptrace / munlock / execveat (case 159, 117, 217, 281)
//
// All case bodies are extracted verbatim from the original syscalls.cpp.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "syscalls/syscalls.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <syscall.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/sendfile.h>

namespace arm64emu {

int64_t syscall_misc(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& fds_ = emu.fds_;
    auto& signals_ = emu.signals_;

    switch (num) {
        case 117: { // ptrace — return -EPERM
            ret_err(EPERM);
            return 0;
        }

        case 124: { // sched_yield — AArch64 124
            // BUGFIX: was previously labeled "sched_setaffinity" but
            // sched_setaffinity is 122, not 124. The actual syscall at
            // 124 is sched_yield. The old code returned 0 without
            // yielding, causing concurrent threads that rely on
            // sched_yield() to spin-lock.
            int r = ::sched_yield();
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 122: { // sched_setaffinity — no-op, return 0
            // BUGFIX: this is the real sched_setaffinity (was at 124).
            // Return 0 to indicate the affinity "call" succeeded.
            ret_host(0);
            return 0;
        }
        case 123: { // sched_getaffinity — return full mask
            // AArch64 123. Write an 8-byte affinity mask (all CPUs = 1).
            if (a1 >= 8 && a2 != 0) {
                try { mem_.store<uint64_t>(a2, 1ULL); }
                catch (...) { ret_err(EFAULT); return 0; }
                ret_host(8);
                return 0;
            }
            ret_host(0);
            return 0;
        }
        case 125: { // sched_get_priority_max(policy) — AArch64 125
            // BUGFIX: previously labeled "truncate (legacy)" but AArch64
            // has no legacy truncate (it's truncate at 45). The real
            // syscall at 125 is sched_get_priority_max.
            ret_host(99);  // SCHED_FIFO max priority
            return 0;
        }
        case 126: { // sched_get_priority_min(policy) — AArch64 126
            ret_host(1);
            return 0;
        }
        case 140: { // setpriority — AArch64 140
            // BUGFIX: previously labeled "chown (legacy)" but AArch64
            // has no legacy chown. Real syscall at 140 is setpriority.
            // We accept but ignore priority changes.
            ret_host(0);
            return 0;
        }
        case 141: { // getpriority — AArch64 141
            // BUGFIX: previously labeled "fchown (legacy)". Real syscall
            // is getpriority. Return priority 20 (default nice value).
            // Note: getpriority returns the value in [0, 40] (priority+20),
            // or -1 on error; we return 20 (priority 0).
            ret_host(20);
            return 0;
        }

        case 132: { // sigaltstack(new, old) — AArch64 132
            // Set or query the per-CPU alternate signal stack.
            int r = SignalTable::set_altstack(mem_, cpu, a0, a1);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 136: { // rt_sigpending(sigset, sigsetsize) — AArch64 136
            // Return the calling thread's pending-signal mask.
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
            // We don't support queueing signals with payloads. Pretend
            // success so callers (e.g., raise()) proceed.
            ret_host(0);
            return 0;
        }

        case 137: { // rt_sigtimedwait(sigset, info, timeout, sigsetsize)
            // We don't support sigtimedwait (no signal queue). Return
            // -EAGAIN so callers fall back to a polling loop.
            ret_err(EAGAIN);
            return 0;
        }

        case 133: { // rt_sigsuspend(mask, sigsetsize) — AArch64 133
            // Atomically replace the signal mask with `mask` and block
            // until a signal is delivered that's not in `mask`. After
            // the handler runs (and rt_sigreturn restores state), return
            // -EINTR (the standard sigsuspend return).
            //
            // We translate the guest mask to a host sigset for the actual
            // blocking wait, AND we update cpu.sigmask so that signals
            // not in `mask` are deliverable (not re-queued as pending).
            //
            // Mask restoration: on real Linux, the kernel restores the
            // original mask AFTER the handler returns. We approximate
            // this by restoring the original mask only if NO signal was
            // delivered. If a signal WAS delivered, the mask after
            // rt_sigreturn will be the sigsuspend mask (a slight semantic
            // difference that doesn't affect typical programs — they
            // re-set the mask explicitly after sigsuspend returns).
            uint64_t guest_mask = 0;
            if (a0 != 0) {
                try {
                    guest_mask = mem_.load<uint64_t>(a0);
                } catch (...) {
                    ret_err(EFAULT);
                    return 0;
                }
            }
            // Save the current cpu.sigmask and replace it with guest_mask
            // so drain_host_signals → deliver_signal sees the sigsuspend
            // mask (signals not in the mask are unblocked and deliverable).
            const uint64_t saved_sigmask = cpu.sigmask;
            cpu.sigmask = guest_mask & ~((1ULL << (BIFROST_SIGKILL - 1)) |
                                          (1ULL << (BIFROST_SIGSTOP - 1)));

            // Pre-set the return value so the signal frame captures it.
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));

            // First, drain any pending guest signals that are now
            // unblocked by the sigsuspend mask. If a handler runs, we
            // return immediately (the handler is set up to run; after
            // rt_sigreturn, cpu.regs[0] = -EINTR).
            if (cpu.sigpending != 0) {
                if (deliver_pending_signals(emu, cpu, signals_) > 0) {
                    return 0;  // handler will run
                }
            }
            // Also drain any queued host signals (from earlier kill/raise
            // that arrived via the host signal handler).
            if (emu.handle_eintr(cpu)) {
                return 0;  // handler will run
            }

            // No pending signals — block on the host sigsuspend until a
            // new signal arrives. Build the host sigset from guest_mask.
            sigset_t host_mask;
            sigemptyset(&host_mask);
            for (int signo = 1; signo <= 31; signo++) {
                if ((guest_mask >> (signo - 1)) & 1) {
                    sigaddset(&host_mask, signo);
                }
            }
            ::sigsuspend(&host_mask);
            // sigsuspend returned — a host signal arrived. Drain the SPSC
            // queue to deliver the guest handler.
            const bool delivered = emu.handle_eintr(cpu);
            if (!delivered) {
                // No signal delivered (SIG_IGN or empty queue). Restore
                // the original mask and return -EINTR.
                cpu.sigmask = saved_sigmask;
            }
            // If delivered: the handler is set up to run. After rt_sigreturn,
            // cpu.sigmask will be the sigsuspend mask and cpu.regs[0] = -EINTR.
            return 0;
        }

        case 134: { // rt_sigaction(signo, new_act, old_act, sigsetsize)
            // Install/query the guest's signal handler. See SignalTable::install
            // for semantics (SIG_DFL/SIG_IGN/handler, query-only via new_act=0,
            // EINVAL for SIGKILL/SIGSTOP).
            int signo = static_cast<int>(a0);
            int r = signals_.install(mem_, signo, a1, a2);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 135: { // rt_sigprocmask(how, new_set, old_set, sigsetsize)
            // Per-CPU signal mask. Supports SIG_BLOCK/SIG_UNBLOCK/
            // SIG_SETMASK. SIGKILL/SIGSTOP cannot be blocked.
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
            // After changing the mask, check for newly-unblocked pending
            // signals and deliver them. This is what makes raise() of a
            // blocked signal work: musl blocks all, calls tkill (queues
            // the signal), then restores the mask. The restore triggers
            // delivery here.
            //
            // We must set cpu.regs[0] = r BEFORE delivery so the signal
            // frame captures the syscall's return value. After the handler
            // runs and rt_sigreturn restores the frame, cpu.regs[0] will
            // be `r` — which is what the libc wrapper expects to see.
            bool signal_delivered = false;
            if (r == 0 && cpu.sigpending != 0) {
                if (trace) {
                    fprintf(stderr, "[signal] rt_sigprocmask: pending=0x%llx, "
                            "delivering...\n",
                            static_cast<unsigned long long>(cpu.sigpending));
                }
                // Pre-set the syscall return value so the signal frame
                // captures it. deliver_signal will overwrite cpu.regs[0]
                // with `signo` for the handler, but the SAVED frame.regs[0]
                // will be `r`. After rt_sigreturn, cpu.regs[0] = r.
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
                if (deliver_pending_signals(emu, cpu, signals_) > 0) {
                    // A signal was delivered — the handler is now set up
                    // (cpu.pc = handler, cpu.regs[0] = signo). DON'T
                    // overwrite x0 — the handler expects x0=signo. After
                    // rt_sigreturn, cpu.regs[0] will be restored to `r`
                    // (the value we pre-set above and the frame saved).
                    signal_delivered = true;
                }
            }
            if (!signal_delivered) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            }
            return 0;
        }

        case 139: { // rt_sigreturn — restore CPU state from signal frame
            // Pop the most recent signal frame, restore all CPU state
            // (X0..X30, SP, PC, PSTATE, V0..V31, FPCR, FPSR), restore the
            // saved signal mask, and clear the altstack in-use flag if the
            // handler ran on the altstack. Returns whatever X0 was in the
            // saved frame (already restored) — do NOT overwrite it.
            SignalFrame frame;
            if (signals_.pop_frame(frame)) {
                // BUGFIX (Turn 55 — root cause of interpreter stack corruption):
                // The old code used `sizeof(cpu.regs)` (32 entries, 256 bytes)
                // but `frame.regs` only has 31 entries (248 bytes). The memcpy
                // read 8 bytes PAST frame.regs, getting frame.sp and writing
                // it into cpu.regs[31]. Since cpu.regs[31] is supposed to be
                // XZR (always 0), any instruction that reads Rn=31 (like
                // `mov w0, wzr` or `orr w0, wzr, w19`) would get the SP value
                // instead of 0, corrupting the destination register.
                //
                // This was the root cause of the "interpreter rt_sigreturn
                // stack-corruption bug" documented as a known issue since
                // Turn 46. The JIT was unaffected because it doesn't use
                // cpu.regs[31] for XZR — it emits a literal 0 in the codegen.
                //
                // Fix: copy exactly sizeof(frame.regs) bytes (31 entries),
                // then explicitly zero cpu.regs[31] to maintain XZR semantics.
                memcpy(cpu.regs, frame.regs, sizeof(frame.regs));
                cpu.regs[31] = 0;  // XZR — must always be 0
                cpu.sp     = frame.sp;
                cpu.pc     = frame.pc;
                cpu.pstate = frame.pstate;
                cpu.sigmask = frame.saved_mask;
                memcpy(cpu.v_lo, frame.v_lo, sizeof(cpu.v_lo));
                memcpy(cpu.v_hi, frame.v_hi, sizeof(cpu.v_hi));
                cpu.fpcr = frame.fpcr;
                cpu.fpsr = frame.fpsr;
                if (frame.on_altstack) {
                    SignalTable::set_altstack_active(cpu, false);
                }
                // BUGFIX (production hardening): after restoring the saved
                // mask, check for newly-unblocked pending signals. Without
                // this, a signal that was queued while the handler was
                // running (e.g., SIGUSR2 raised inside the SIGUSR1 handler
                // with SIGUSR2 in sa_mask) would never be delivered — the
                // run loop only drains EXTERNAL (host) signals, not guest
                // pending signals.
                //
                // We return 0 to the run loop with cpu.pc = the restored
                // PC. The run loop will then call drain_host_signals, but
                // that doesn't touch cpu.sigpending. So we MUST do it here.
                if (cpu.sigpending != 0) {
                    deliver_pending_signals(emu, cpu, signals_);
                    // Note: if a signal was delivered, cpu.pc is now the
                    // handler's PC (NOT the restored PC). The restored PC
                    // is saved in the new signal frame, and will be
                    // restored when this handler returns via rt_sigreturn.
                    // If no signal was delivered (all pending still
                    // blocked), cpu.pc is unchanged.
                }
                return 0;
            }
            // No pending frame — guest bug. Return 0 to avoid crash.
            ret_host(0);
            return 0;
        }

        case 155: { // getpgid(pid) — AArch64 155
            // BUGFIX: previously labeled "sched_yield" but sched_yield is
            // at 124 (now correctly handled). The real syscall at 155 is
            // getpgid. Return 1 (we're a single-process guest with PGID=1).
            ret_host(1);
            return 0;
        }

        case 153: { // times(struct tms *buf) — AArch64 153
            // times() returns the number of clock ticks since an arbitrary
            // point in the past, and fills struct tms.
            // BUGFIX (Turn 62 rev 2): ALWAYS write to the guest buffer,
            // even on error. The old code returned ret_errno() without
            // writing, leaving the guest's struct tms uninitialized with
            // deterministic stack garbage. Now we zero-fill first, then
            // try host times(). If it fails, the guest gets zeros.
            struct tms t;
            memset(&t, 0, sizeof(t));
            clock_t r = ::times(a0 ? &t : nullptr);
            if (r == static_cast<clock_t>(-1)) {
                // Host times() failed — still write zeros to guest.
                if (a0) {
                    try { mem_.write(a0, &t, sizeof(t)); }
                    catch (...) { ret_err(EFAULT); return 0; }
                }
                ret_errno();
                return 0;
            }
            if (a0) {
                try {
                    mem_.write(a0, &t, sizeof(t));
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        case 158: { // sched_setaffinity — no-op (alias of 122, some guests use 158)
            // Note: AArch64 158 is actually rseqg, but some musl versions
            // probe sched_setaffinity here on legacy builds. Treat as no-op.
            ret_host(0);
            return 0;
        }

        case 159: { // sethostname — no-op for emulation
            ret_host(0);
            return 0;
        }

        case 160: { // uname
            // struct utsname (Linux): 6 fields of 65 bytes each
            //   sysname, nodename, release, version, machine, domainname
            // glibc checks the release string to decide which features
            // (VDSO, futex flags, etc.) are available. We advertise a
            // reasonably modern kernel so glibc takes the fast paths.
            const char* fields[] = {
                "Linux",                       // sysname
                "arm64-emu",                   // nodename
                "6.5.0",                       // release (glibc wants >= 3.2 for most things)
                "#1 SMP PREEMPT Dynamic arm64-emu", // version
                "aarch64",                     // machine
                "(none)",                      // domainname
            };
            uint64_t off = a0;
            for (auto s : fields) {
                char buf[65] = {0};
                strncpy(buf, s, 64);
                mem_.write(off, buf, 65);
                off += 65;
            }
            ret_host(0);
            return 0;
        }

        case 163: { // getrlimit(resource, rlim) — AArch64 syscall 163
            // Return generous infinite limits so libc doesn't choke.
            // struct rlimit { uint64_t rlim_cur; uint64_t rlim_max; }
            uint64_t rlim[2] = { static_cast<uint64_t>(-1ULL), static_cast<uint64_t>(-1ULL) };
            mem_.write(a1, rlim, sizeof(rlim));
            ret_host(0);
            return 0;
        }

        case 165: { // getrusage(who, usage) — AArch64 syscall 165
            // BUGFIX (Turn 62): forward to host ::getrusage(). The host's
            // rusage includes emulator overhead, but for RUSAGE_CHILDREN
            // (used by `toybox time` after wait4) it's the forked emulator
            // child's CPU time — the closest we can get to guest CPU time.
            // struct rusage is 144 bytes on LP64, identical layout on
            // x86-64 host and AArch64 guest (both use 64-bit time_t).
            //
            // BUGFIX (Turn 62 rev 2): ALWAYS write to the guest buffer,
            // even on error. The old code returned ret_errno() without
            // writing, leaving the guest's stack buffer uninitialized
            // with deterministic garbage (e.g. "user 549755811552.42").
            // Now we zero-fill first, then try host getrusage. If it
            // fails, the guest gets zeros (not garbage).
            int who = static_cast<int>(a0);
            if (a1 == 0) { ret_err(EFAULT); return 0; }
            // Zero-fill first so the guest NEVER sees uninitialized data.
            struct rusage ru;
            memset(&ru, 0, sizeof(ru));
            int r = ::getrusage(who, &ru);
            if (r < 0) {
                // Host getrusage failed — write zeros (not garbage).
                if (getenv("BIFROST_TRACE_RUSAGE")) {
                    fprintf(stderr, "[getrusage] who=%d FAILED errno=%d — "
                            "writing zeros\n", who, errno);
                }
            } else {
                if (getenv("BIFROST_TRACE_RUSAGE")) {
                    fprintf(stderr, "[getrusage] who=%d OK: utime=%ld.%06ld "
                            "stime=%ld.%06ld\n", who,
                            (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
                            (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec);
                }
            }
            try { mem_.write(a1, &ru, sizeof(ru)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_host(r < 0 ? static_cast<uint64_t>(static_cast<int64_t>(-errno)) : 0);
            return 0;
        }

        case 166: { // umask(new_mask) — aarch64 166
            // toybox sh calls umask(0) during init and
            // umask(prev) at shutdown. Just pass through to the host.
            mode_t old = ::umask((mode_t)a0);
            ret_host(static_cast<uint64_t>(old));
            return 0;
        }

        case 167: { // prctl - handle PR_SET_NAME etc as no-op
            ret_host(0);
            return 0;
        }

        case 168: { // getcpu(cpu, node, tcache) — AArch64 168
            // BUGFIX: previously implemented as ppoll, but AArch64 168 is
            // getcpu (ppoll is at 73). The old code dereferenced `cache`
            // (a3) as a `timespec*` and called ::poll with `node` (a1, a
            // pointer) as nfds — corrupting memory and crashing. We now
            // implement getcpu: write CPU=0 and NUMA node=0 (we have one
            // of each in the guest).
            if (a0 != 0) {
                try { mem_.store<uint32_t>(a0, 0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            if (a1 != 0) {
                try { mem_.store<uint32_t>(a1, 0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }

        case 172: { // getpid
            // CLONE_THREAD semantics: all threads in the same thread
            // group see the same PID (= the main thread's TID, which is
            // 1 for the guest process). Forked children (clone without
            // CLONE_VM) are separate processes — they must report a
            // DIFFERENT PID so that getpid() != parent_pid, which lets
            // the child detect it's a forked process and reset signal
            // handlers to SIG_DFL (e.g., toybox sh line 2711:
            // "if (getpid() != TT.pid) signal(SIGINT, SIG_DFL)").
            // Without this, the forked child inherits SIG_IGN from the
            // parent and Ctrl+C can't interrupt foreground commands.
            if (cpu.is_fork_process) {
                ret_host(static_cast<uint64_t>(cpu.tid));
            } else {
                ret_host(1);
            }
            return 0;
        }

        case 173: { // getppid
            // BUGFIX: was returning the emulator's host parent PID for ALL
            // guests. For the main process, the host ppid is the launching
            // shell, but the guest should see init (PID 1) as its parent.
            // For a forked child (fork_guest uses host fork), the host
            // ppid IS the parent emulator's PID, which is the correct
            // guest ppid — and critically, the child uses getppid() to
            // send signals back to its parent (e.g. test_sigint_handler
            // does `kill(getppid(), SIGINT)`), so we MUST return the
            // real host ppid for forked children.
            if (cpu.is_fork_process) {
                ret_host(::getppid());
            } else {
                ret_host(1);  // main process: parent is init
            }
            return 0;
        }

        case 174: { // getuid — return 0 (root) so setuid programs work
            // BUGFIX: was returning the host's real UID. If the emulator
            // runs as a normal user (uid 1000), the guest saw uid 1000
            // instead of 0 (root), breaking setuid programs, file
            // ownership checks, and any program expecting to run as root
            // in the rootfs. The guest is a single-user sandbox → root.
            ret_host(0);
            return 0;
        }

        case 175: { // geteuid — return 0 (root)
            ret_host(0);
            return 0;
        }

        case 176: { // getgid — return 0 (root)
            ret_host(0);
            return 0;
        }

        case 177: { // getegid — return 0 (root)
            ret_host(0);
            return 0;
        }

        case 178: { // gettid
            // Return the guest TID of the calling thread.
            ret_host(cpu.tid);
            return 0;
        }

        case 180: { // sysinfo(struct sysinfo *info) — AArch64 syscall 180
            // Fills a struct sysinfo (112 bytes on 64-bit) with system
            // memory/load info. Used by `free`, `top`, and other tools.
            // We return reasonable fake values so these tools don't
            // crash or show garbage.
            //
            // struct sysinfo layout (64-bit, 112 bytes):
            //   offset  0: uptime (8 bytes)
            //   offset  8: loads[3] (24 bytes)
            //   offset 32: totalram (8 bytes)
            //   offset 40: freeram (8 bytes)
            //   offset 48: sharedram (8 bytes)
            //   offset 56: bufferram (8 bytes)
            //   offset 64: totalswap (8 bytes)
            //   offset 72: freeswap (8 bytes)
            //   offset 80: procs (2 bytes)
            //   offset 82: pad (2 bytes)
            //   offset 88: totalhigh (8 bytes)
            //   offset 96: freehigh (8 bytes)
            //   offset 104: mem_unit (4 bytes)
            //   offset 108: padding (4 bytes)
            if (a0 == 0) { ret_err(EFAULT); return 0; }
            // BUGFIX (Turn 62 rev 3): forward to host ::sysinfo() for real
            // uptime and load averages. The old code returned fake uptime=100
            // and zero loads, which broke `toybox uptime` (showed "230961:11:19").
            struct sysinfo si;
            memset(&si, 0, sizeof(si));
            ::sysinfo(&si);
            try {
                // uptime: real host uptime (seconds since boot)
                mem_.store<uint64_t>(a0 + 0, si.uptime);
                // loads: 1/5/15 min load averages (scaled by 65536)
                mem_.store<uint64_t>(a0 + 8,  si.loads[0]);
                mem_.store<uint64_t>(a0 + 16, si.loads[1]);
                mem_.store<uint64_t>(a0 + 24, si.loads[2]);
                // Memory: use host's real values
                mem_.store<uint64_t>(a0 + 32, si.totalram);
                mem_.store<uint64_t>(a0 + 40, si.freeram);
                mem_.store<uint64_t>(a0 + 48, si.sharedram);
                mem_.store<uint64_t>(a0 + 56, si.bufferram);
                mem_.store<uint64_t>(a0 + 64, si.totalswap);
                mem_.store<uint64_t>(a0 + 72, si.freeswap);
                // procs: use host's real proc count
                mem_.store<uint16_t>(a0 + 80, si.procs);
                mem_.store<uint16_t>(a0 + 82, 0);  // pad
                mem_.store<uint64_t>(a0 + 88, si.totalhigh);
                mem_.store<uint64_t>(a0 + 96, si.freehigh);
                mem_.store<uint32_t>(a0 + 104, si.mem_unit);
                mem_.store<uint32_t>(a0 + 108, 0); // padding
                ret_host(0);
            } catch (...) {
                ret_err(EFAULT);
            }
            return 0;
        }

        case 19: { // eventfd2(count, flags) — aarch64 syscall 19
            ret_host(::eventfd((unsigned int)a0, static_cast<int>(a1)));
            return 0;
        }

        case 198: { // socket (glibc may probe for IPC)
            ret_err(ENOSYS);
            return 0;
        }

        case 199: { // socketpair(domain, type, protocol, sv) — aarch64 199
            int fds[2];
            int r = ::socketpair(static_cast<int>(a0), static_cast<int>(a1), static_cast<int>(a2), fds);
            if (r == 0) {
                mem_.store<int>(a3, fds[0]);
                mem_.store<int>(a3 + 4, fds[1]);
            }
            ret_host(r);
            return 0;
        }

        case 20: { // epoll_create1(flags) — aarch64 syscall 20
            ret_host(::epoll_create1(static_cast<int>(a0)));
            return 0;
        }

        case 200: { // bind(sockfd, addr, addrlen) — aarch64 200
            // We can't marshal sockaddr from guest memory safely without
            // knowing the family, so return -ENOSYS for now.
            ret_err(ENOSYS);
            return 0;
        }

        case 201: { // listen(sockfd, backlog) — aarch64 201
            ret_host(::listen(static_cast<int>(a0), static_cast<int>(a1)));
            return 0;
        }

        case 202: { // accept(sockfd, addr, addrlen) — aarch64 202
            ret_host(::accept(static_cast<int>(a0), nullptr, nullptr));
            return 0;
        }

        case 203: { // connect(sockfd, addr, addrlen) — aarch64 203
            ret_err(ENOSYS);
            return 0;
        }

        case 21: { // epoll_ctl(epfd, op, fd, event) — aarch64 syscall 21
            // struct epoll_event: { uint32_t events; epoll_data_t data; }
            // epoll_data_t is a union with uint64_t as the largest member.
            // On aarch64 Linux this is packed to 12 bytes total.
            struct epoll_event ev;
            ev.events = mem_.load<uint32_t>(a3);
            ev.data.u64 = mem_.load<uint64_t>(a3 + 4);
            ret_host(::epoll_ctl(static_cast<int>(a0), static_cast<int>(a1), static_cast<int>(a2), &ev));
            return 0;
        }

        case 213: { // readahead(fd, offset, count) — AArch64 213
            // BUGFIX: was previously labeled "rt_sigpending" but
            // rt_sigpending is at 136 (already correctly handled). The
            // real syscall at 213 is readahead. Forward to host readahead;
            // for non-host-fds, return 0 (pretend we read ahead).
            int fd = static_cast<int>(a0);
            (void)fd; (void)a1; (void)a2;
            // Bypass FdTable (readahead is best-effort and harmless to skip).
            ::readahead(fd, (off_t)a1, a2);
            ret_host(0);
            return 0;
        }

        case 217: { // munlock — no-op
            ret_host(0);
            return 0;
        }

        case 22: { // epoll_pwait(epfd, events, maxevents, timeout, sigmask)
            // Real AArch64 syscall 22. We forward to epoll_wait and ignore
            // the sigmask (guest signal delivery isn't supported anyway).
            struct epoll_event evs[256];
            int maxev = static_cast<int>(a2);
            if (maxev > 256) maxev = 256;
            int n = ::epoll_wait(static_cast<int>(a0), evs, maxev, static_cast<int>(a3));
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    uint64_t p = a1 + static_cast<uint64_t>(i) * 12;
                    mem_.store<uint32_t>(p, evs[i].events);
                    mem_.store<uint64_t>(p + 4, evs[i].data.u64);
                }
            }
            ret_host(n);
            return 0;
        }

        case 232: { // mincore(addr, length, vec) — AArch64 232
            // BUGFIX: previously labeled "epoll_wait" but 232 is mincore.
            // epoll_wait does not exist on AArch64 (epoll_pwait at 22 is
            // already correctly handled). The old code called ::epoll_wait
            // with garbage args when the guest invoked mincore. We now
            // return 0 (success) and write 1s to the vec bitmap, indicating
            // all pages are in memory (which is true for our sparse memory).
            if (a2 != 0 && a1 > 0) {
                uint64_t pages = (a1 + 4095) / 4096;
                try {
                    for (uint64_t i = 0; i < pages; i++) {
                        mem_.store<uint8_t>(a2 + i, 1);
                    }
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }

        case 247: { // waitpid (legacy, same as wait4) — aarch64 247
            // BUGFIX (Turn 62 rev 3): ALWAYS write status to guest.
            int status = 0;
            pid_t r;
            while (true) {
                r = ::waitpid((pid_t)a0, &status, static_cast<int>(a2));
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            if (a1) {
                try { mem_.store<uint32_t>(a1, static_cast<uint32_t>(status)); }
                catch (...) {}
            }
            if (r < 0) {
                ret_errno();
                return 0;
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        case 260: { // wait4(pid, wstatus, options, rusage) — aarch64 260
            // BUGFIX (Turn 62 rev 3): ALWAYS write to guest buffers (a1
            // wstatus, a3 rusage) even on error. The old code returned
            // ret_errno() without writing, leaving the guest's struct
            // rusage uninitialized with deterministic stack garbage.
            // This was the ACTUAL root cause of `toybox time` showing
            // "user 549755811552.42" — wait4 failed (ECHILD or EINTR),
            // the rusage buffer was never written, and the guest read
            // its uninitialized stack.
            pid_t pid = (pid_t)a0;
            int options = static_cast<int>(a2);
            int status = 0;
            struct rusage ru;
            memset(&ru, 0, sizeof(ru));
            pid_t r;
            while (true) {
                r = ::wait4(pid, &status, options, a3 ? &ru : nullptr);
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            // ALWAYS write wstatus and rusage to guest memory, even on
            // error, so the guest never sees uninitialized stack data.
            if (a1) {
                try { mem_.store<uint32_t>(a1, static_cast<uint32_t>(status)); }
                catch (...) {}
            }
            if (a3) {
                try { mem_.write(a3, &ru, sizeof(ru)); }
                catch (...) {}
            }
            if (r < 0) {
                ret_errno();
                return 0;
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        case 261: { // prlimit64 (glibc probes resource limits)
            // prlimit64(pid, resource, new_rlim, old_rlim)
            // Return 0 with zeroed rlim if old_rlim is non-NULL.
            if (a3 != 0) {
                uint8_t buf[16] = {0};
                // rlim_cur = RLIM_INFINITY = ~0
                uint64_t inf = ~0ULL;
                memcpy(buf, &inf, 8);
                memcpy(buf + 8, &inf, 8);
                mem_.write(a3, buf, 16);
            }
            ret_host(0);
            return 0;
        }

        case 270: { // process_vm_readv(pid, lvec, liovcnt, rvec, riovcnt, flags)
            // BUGFIX: was previously labeled "eventfd2 alt" but eventfd2 is
            // at 19 (already handled). The real syscall at 270 is
            // process_vm_readv. We don't support cross-process VM reads;
            // return -ENOSYS.
            ret_err(ENOSYS);
            return 0;
        }

        case 272: { // kcmp(pid1, pid2, type, idx1, idx2) — AArch64 272
            // BUGFIX: was previously labeled "waitid" but waitid is at 95
            // (already handled). The real syscall at 272 is kcmp. We don't
            // support kernel comparison; return 0 (same file) for safety.
            ret_host(0);
            return 0;
        }

        case 278: { // getrandom(buf, buflen, flags) — AArch64 278
            // Provide real random bytes from /dev/urandom. With TLS
            // properly set up, glibc's per-thread getrandom state is
            // zero-initialized (state->buf == NULL), so it tries to
            // initialize via this syscall; returning the requested
            // bytes sets state->cap > 0.
            if (a1 == 0 || a0 == 0) { ret_host(0); return 0; }
            // Cap at 256 bytes to prevent huge allocations — the kernel
            // itself caps getrandom at 256 per call for GRND_NONBLOCK.
            size_t len = a1;
            if (len > 256) len = 256;
            std::vector<uint8_t> tmp(len);
            FILE* ur = fopen("/dev/urandom", "rb");
            if (!ur) { ret_err(ENOSYS); return 0; }
            size_t got = fread(tmp.data(), 1, len, ur);
            fclose(ur);
            if (got == 0) { ret_err(EIO); return 0; }
            mem_.write(a0, tmp.data(), got);
            ret_host(got);
            return 0;
        }

        case 281: { // execveat — not supported
            ret_err(ENOSYS);
            return 0;
        }

        case 293: { // rseq (restartable sequences, glibc probes at startup)
            // Return -ENOSYS so glibc disables rseq and uses regular paths.
            ret_err(ENOSYS);
            return 0;
        }

        case 72: { // pselect6(nfds, rfds, wfds, efds, ts, sig) — aarch64 72
            // Delegate to host select. FD sets are bitmaps (1024 bits = 128 bytes).
            int nfds = static_cast<int>(a0);
            struct timeval tv;
            struct timeval* tvp = nullptr;
            if (a4) {
                tv.tv_sec  = (time_t)mem_.load<uint64_t>(a4);
                tv.tv_usec = (suseconds_t)mem_.load<uint64_t>(a4 + 8);
                tvp = &tv;
            }
            int r;
            fd_set rfds, wfds, efds;
            while (true) {
                // Rebuild fd_sets each iteration — select modifies them
                // in place, so on EINTR retry we need fresh copies.
                FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
                if (a1) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (mem_.load<uint8_t>(a1 + fd/8) & (1 << (fd%8))) FD_SET(fd, &rfds);
                }
                if (a2) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (mem_.load<uint8_t>(a2 + fd/8) & (1 << (fd%8))) FD_SET(fd, &wfds);
                }
                if (a3) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (mem_.load<uint8_t>(a3 + fd/8) & (1 << (fd%8))) FD_SET(fd, &efds);
                }
                r = ::select(nfds, a1 ? &rfds : nullptr, a2 ? &wfds : nullptr,
                             a3 ? &efds : nullptr, tvp);
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            if (r >= 0) {
                auto write_back = [&](uint64_t addr, fd_set* set) {
                    std::vector<uint8_t> buf(128, 0);
                    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                        if (FD_ISSET(fd, set)) buf[fd/8] |= (1 << (fd%8));
                    }
                    mem_.write(addr, buf.data(), 128);
                };
                if (a1) write_back(a1, &rfds);
                if (a2) write_back(a2, &wfds);
                if (a3) write_back(a3, &efds);
            }
            ret_host(r);
            return 0;
        }

        case 73: { // ppoll(fds, nfds, ts, sigmask) — aarch64 syscall 73
            // previously missing — toybox's `sh` calls
            // ppoll() to wait for input on stdin, and the -ENOSYS fallback
            // sent it into a busy-wait loop. Forward to host poll(2) with
            // a millisecond timeout derived from the timespec.
            int nfds = static_cast<int>(a1);
            std::vector<struct pollfd> pfds(nfds);
            for (int i = 0; i < nfds; i++) {
                pfds[i].fd      = mem_.load<int>(a0 + static_cast<uint64_t>(i) * 8);
                pfds[i].events  = mem_.load<int16_t>(a0 + static_cast<uint64_t>(i) * 8 + 4);
                pfds[i].revents = 0;
            }
            int timeout_ms = -1;
            if (a2) {
                uint64_t sec  = mem_.load<uint64_t>(a2);
                uint64_t nsec = mem_.load<uint64_t>(a2 + 8);
                if (sec == 0 && nsec == 0) timeout_ms = 0;
                else { uint64_t ms = (sec > 2000000ULL) ? 2000000000ULL : sec * 1000; ms += nsec / 1000000; timeout_ms = (ms > 2000000000ULL) ? 2000000000 : static_cast<int>(ms); }
            }
            int r;
            while (true) {
                r = ::poll(pfds.data(), nfds, timeout_ms);
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            for (int i = 0; i < nfds; i++) {
                mem_.store<int16_t>(a0 + static_cast<uint64_t>(i) * 8 + 6, pfds[i].revents);
            }
            ret_host(r);
            return 0;
        }

        case 85: { // timerfd_create(clockid, flags) — aarch64 syscall 85
            ret_host(::timerfd_create(static_cast<int>(a0), static_cast<int>(a1)));
            return 0;
        }

        case 86: { // timerfd_settime(fd, flags, new, old) — aarch64 syscall 86
            if (!a2) { ret_err(EFAULT); return 0; }
            struct itimerspec newv;
            struct itimerspec oldv;
            newv.it_interval.tv_sec  = static_cast<time_t>(mem_.load<uint64_t>(a2));
            newv.it_interval.tv_nsec = static_cast<long>(mem_.load<uint64_t>(a2 + 8));
            newv.it_value.tv_sec     = static_cast<time_t>(mem_.load<uint64_t>(a2 + 16));
            newv.it_value.tv_nsec    = static_cast<long>(mem_.load<uint64_t>(a2 + 24));
            int r = ::timerfd_settime(static_cast<int>(a0), static_cast<int>(a1), &newv, a3 ? &oldv : nullptr);
            if (r == 0 && a3) {
                mem_.store<uint64_t>(a3, static_cast<uint64_t>(oldv.it_interval.tv_sec));
                mem_.store<uint64_t>(a3 + 8, static_cast<uint64_t>(oldv.it_interval.tv_nsec));
                mem_.store<uint64_t>(a3 + 16, static_cast<uint64_t>(oldv.it_value.tv_sec));
                mem_.store<uint64_t>(a3 + 24, static_cast<uint64_t>(oldv.it_value.tv_nsec));
            }
            ret_host(r);
            return 0;
        }

        case 87: { // timerfd_gettime(fd, curr) — aarch64 syscall 87
            struct itimerspec cur;
            int r = ::timerfd_gettime(static_cast<int>(a0), &cur);
            if (r == 0 && a1) {
                mem_.store<uint64_t>(a1, static_cast<uint64_t>(cur.it_interval.tv_sec));
                mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(cur.it_interval.tv_nsec));
                mem_.store<uint64_t>(a1 + 16, static_cast<uint64_t>(cur.it_value.tv_sec));
                mem_.store<uint64_t>(a1 + 24, static_cast<uint64_t>(cur.it_value.tv_nsec));
            }
            ret_host(r);
            return 0;
        }

        case 93: { // exit — exit current thread (not whole process)
            // On AArch64 Linux, exit(2) (syscall 93) exits only the
            // calling thread. The kernel's do_exit() handles:
            //   1. clear_child_tid zeroing + futex wake (CLONE_CHILD_CLEARTID)
            //   2. thread->tid zeroing (so pthread_join sees tid==0)
            //
            // musl's pthread_join polls thread->tid (at TPIDR_EL0 - 0xa0)
            // via futex WAIT. The kernel zeros this field on thread exit.
            // Our cleanup in thread_entry zeros clear_child_tid but NOT
            // thread->tid. We zero it here so pthread_join can proceed.
            //
            // thread->tid address = TPIDR_EL0 - 0xc8 + 0x28 = TPIDR_EL0 - 0xa0
            // (musl's struct pthread: base = TPIDR_EL0 - 0xc8, tid at +0x28)
            if (cpu.tpidr_el0 != 0) {
                uint64_t tid_addr = cpu.tpidr_el0 - 0xa0;
                try {
                    emu.mem_.store<uint32_t>(tid_addr, 0);
                    // Futex wake on the tid field so pthread_join unblocks.
                    auto* slot = emu.get_futex(tid_addr);
                    {
                        std::lock_guard<std::mutex> lk(slot->mu);
                        slot->cv.notify_all();
                    }
                } catch (...) {
                    // tid_addr unmapped — nothing we can do
                }
            }
            cpu.running = false;
            cpu.exit_code = static_cast<int>(a0);
            return 0;
        }

        // ── inotify_init1 (syscall 26) ───────────────────────────────
        // BUGFIX: AArch64 syscall numbers 75/76/77 are vmsplice/splice/tee,
        // NOT inotify. The real inotify numbers per asm-generic/unistd.h are:
        //   26 = inotify_init1
        //   27 = inotify_add_watch
        //   28 = inotify_rm_watch
        // The old code misrouted any guest vmsplice/splice/tee call into
        // inotify handlers (which would call host inotify with garbage
        // args and fail). Real guest inotify_init1 (syscall 26) returned
        // -ENOSYS. Fixed by renumbering to the correct AArch64 slots.
        case 26: { // inotify_init1(flags)
            int fd = ::inotify_init1(static_cast<int>(a0));
            if (fd < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── inotify_add_watch (syscall 27) ───────────────────────────
        case 27: { // inotify_add_watch(fd, pathname, mask)
            std::string path = Yggdrasil::read_path(mem_, a1);
            int wd = ::inotify_add_watch(static_cast<int>(a0), path.c_str(),
                                         static_cast<uint32_t>(a2));
            if (wd < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(wd));
            return 0;
        }

        // ── inotify_rm_watch (syscall 28) ────────────────────────────
        case 28: { // inotify_rm_watch(fd, wd)
            int r = ::inotify_rm_watch(static_cast<int>(a0), static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        // ── vmsplice / splice / tee (syscalls 75/76/77) ──────────────
        // These are real AArch64 syscalls but we don't implement them.
        // Return -ENOSYS so callers can fall back to read/write loops.
        case 75: { ret_err(ENOSYS); return 0; }  // vmsplice
        case 76: { ret_err(ENOSYS); return 0; }  // splice
        case 77: { ret_err(ENOSYS); return 0; }  // tee

        // ── accept4 (syscall 242) ────────────────────────────────────
        // NOTE: AArch64 syscall 88 is utimensat (handled in fs.cpp), NOT
        // accept4. Real accept4 is syscall 242. The old code at case 88
        // was dead — fs.cpp's utimensat handler always won the dispatch
        // order, so this case never ran. Moved to the correct number.
        case 242: { // accept4(sockfd, addr, addrlen, flags) — AArch64 242
            // Marshal sockaddr from host to guest memory.
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int fd = ::accept4(static_cast<int>(a0),
                               reinterpret_cast<struct sockaddr*>(&ss), &sslen,
                               static_cast<int>(a3));
            if (fd < 0) { ret_errno(); return 0; }
            if (a1 && a2) {
                // Read guest addrlen, clamp to our result.
                try {
                    socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                    if (guest_len > sslen) guest_len = sslen;
                    mem_.write(a1, &ss, guest_len);
                    mem_.store<uint32_t>(a2, guest_len);
                } catch (...) {
                    // Bad addr/addrlen pointer — close fd, return EFAULT.
                    ::close(fd);
                    ret_err(EFAULT);
                    return 0;
                }
            }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── clock_nanosleep (syscall 115) ────────────────────────────
        // NOTE: clock_nanosleep is now handled in time.cpp (which runs
        // before misc.cpp in the dispatcher). The duplicate case here
        // was dead code — removed during the rc.1 syscall cleanup to
        // avoid confusion. AArch64 syscall 115 is clock_nanosleep per
        // asm-generic/unistd.h.

        // ── getsockname (syscall 204) ────────────────────────────────
        // BUGFIX: AArch64 syscall numbers per asm-generic/unistd.h:
        //   198=socket 199=socketpair 200=bind 201=listen 202=accept
        //   203=connect 204=getsockname 205=getpeername
        //   206=sendto 207=recvfrom 208=setsockopt 209=getsockopt
        //   210=shutdown 211=sendmsg 212=recvmsg
        // The previous code had all six cases 206–211 mislabeled — each
        // function did what its comment said, but at the wrong syscall
        // number. The renumbering below fixes that: each handler moves to
        // its correct number, and the three previously-missing handlers
        // (setsockopt, getsockopt, shutdown) are added.
        case 204: { // getsockname(sockfd, addr, addrlen)
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getsockname(static_cast<int>(a0),
                                  reinterpret_cast<struct sockaddr*>(&ss), &sslen);
            if (r < 0) { ret_errno(); return 0; }
            if (a1 && a2) {
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                if (guest_len > sslen) guest_len = sslen;
                mem_.write(a1, &ss, guest_len);
                mem_.store<uint32_t>(a2, guest_len);
            }
            ret_host(0);
            return 0;
        }

        // ── getpeername (syscall 205) ────────────────────────────────
        case 205: { // getpeername(sockfd, addr, addrlen)
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getpeername(static_cast<int>(a0),
                                  reinterpret_cast<struct sockaddr*>(&ss), &sslen);
            if (r < 0) { ret_errno(); return 0; }
            if (a1 && a2) {
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                if (guest_len > sslen) guest_len = sslen;
                mem_.write(a1, &ss, guest_len);
                mem_.store<uint32_t>(a2, guest_len);
            }
            ret_host(0);
            return 0;
        }

        // ── sendto (syscall 206) ─────────────────────────────────────
        case 206: { // sendto(sockfd, buf, len, flags, dest_addr, addrlen)
            // Cap buffer size to prevent bad_alloc on absurd lengths.
            size_t len = a2;
            if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
            std::vector<uint8_t> buf(len);
            if (len) mem_.read(a1, buf.data(), len);
            // Marshal dest_addr from guest memory if present.
            struct sockaddr_storage dest_ss;
            struct sockaddr* dest_ptr = nullptr;
            if (a4) {
                socklen_t addrlen = static_cast<socklen_t>(a5);
                if (addrlen > sizeof(dest_ss)) addrlen = sizeof(dest_ss);
                mem_.read(a4, &dest_ss, addrlen);
                dest_ptr = reinterpret_cast<struct sockaddr*>(&dest_ss);
            }
            ssize_t r = ::sendto(static_cast<int>(a0), buf.data(), len,
                                 static_cast<int>(a3), dest_ptr,
                                 static_cast<socklen_t>(a5));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── recvfrom (syscall 207) ───────────────────────────────────
        case 207: { // recvfrom(sockfd, buf, len, flags, src_addr, addrlen)
            size_t len = a2;
            if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
            std::vector<uint8_t> buf(len);
            struct sockaddr_storage src_ss;
            socklen_t srclen = sizeof(src_ss);
            ssize_t r = ::recvfrom(static_cast<int>(a0), buf.data(), len,
                                   static_cast<int>(a3),
                                   reinterpret_cast<struct sockaddr*>(&src_ss),
                                   &srclen);
            if (r < 0) { ret_errno(); return 0; }
            // Write received data back to guest buffer.
            if (r > 0) mem_.write(a1, buf.data(), static_cast<size_t>(r));
            // Write source address back to guest memory if requested.
            if (a4 && a5) {
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a5));
                if (guest_len > srclen) guest_len = srclen;
                mem_.write(a4, &src_ss, guest_len);
                mem_.store<uint32_t>(a5, guest_len);
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── setsockopt (syscall 208) ─────────────────────────────────
        case 208: { // setsockopt(sockfd, level, optname, optval, optlen)
            if (a3 == 0 || a4 == 0) { ret_err(EFAULT); return 0; }
            size_t optlen = a4;
            if (optlen > 4096) optlen = 4096;  // sanity cap
            std::vector<uint8_t> optval(optlen);
            mem_.read(a3, optval.data(), optlen);
            int r = ::setsockopt(static_cast<int>(a0), static_cast<int>(a1),
                                 static_cast<int>(a2), optval.data(),
                                 static_cast<socklen_t>(optlen));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        // ── getsockopt (syscall 209) ─────────────────────────────────
        case 209: { // getsockopt(sockfd, level, optname, optval, optlen*)
            if (a4 == 0) { ret_err(EFAULT); return 0; }
            socklen_t host_optlen = static_cast<socklen_t>(mem_.load<uint32_t>(a4));
            if (host_optlen > 4096) host_optlen = 4096;
            std::vector<uint8_t> optval(host_optlen);
            int r = ::getsockopt(static_cast<int>(a0), static_cast<int>(a1),
                                 static_cast<int>(a2), optval.data(), &host_optlen);
            if (r < 0) { ret_errno(); return 0; }
            if (a3) mem_.write(a3, optval.data(), host_optlen);
            mem_.store<uint32_t>(a4, host_optlen);
            ret_host(0);
            return 0;
        }

        // ── shutdown (syscall 210) ───────────────────────────────────
        case 210: { // shutdown(sockfd, how)
            int r = ::shutdown(static_cast<int>(a0), static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        // ── sendmsg (syscall 211) ────────────────────────────────────
        case 211: { // sendmsg(sockfd, msg, flags)
            // Marshal msghdr + iovec from guest memory.
            // Guest msghdr layout (AArch64):
            //   +0:  void*     msg_name      (8 bytes)
            //   +8:  socklen_t msg_namelen   (4 bytes)
            //   +12: padding                  (4 bytes)
            //   +16: struct iovec* msg_iov    (8 bytes)
            //   +24: size_t    msg_iovlen    (8 bytes)
            //   +32: void*     msg_control   (8 bytes)
            //   +40: socklen_t msg_controllen (4 bytes)
            //   +44: int       msg_flags     (4 bytes)
            if (!a1) { ret_err(EFAULT); return 0; }
            uint64_t msg_name = mem_.load<uint64_t>(a1);
            uint32_t msg_namelen = mem_.load<uint32_t>(a1 + 8);
            uint64_t msg_iov = mem_.load<uint64_t>(a1 + 16);
            uint64_t msg_iovlen = mem_.load<uint64_t>(a1 + 24);

            // Marshal iovec array: each entry is (void* base, size_t len).
            if (msg_iovlen > 1024) msg_iovlen = 1024;  // sanity cap
            std::vector<iovec> iovs(msg_iovlen);
            std::vector<std::vector<uint8_t>> iov_bufs(msg_iovlen);
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t base = mem_.load<uint64_t>(msg_iov + i * 16);
                uint64_t len = mem_.load<uint64_t>(msg_iov + i * 16 + 8);
                iov_bufs[i].resize(len);
                mem_.read(base, iov_bufs[i].data(), len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            // Marshal msg_name.
            std::vector<uint8_t> name_buf;
            if (msg_name && msg_namelen) {
                name_buf.resize(msg_namelen);
                mem_.read(msg_name, name_buf.data(), msg_namelen);
            }
            struct msghdr host_msg;
            memset(&host_msg, 0, sizeof(host_msg));
            host_msg.msg_name = name_buf.empty() ? nullptr : name_buf.data();
            host_msg.msg_namelen = msg_namelen;
            host_msg.msg_iov = iovs.data();
            host_msg.msg_iovlen = msg_iovlen;
            ssize_t r = ::sendmsg(static_cast<int>(a0), &host_msg, static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── recvmsg (syscall 212) ────────────────────────────────────
        // BUGFIX: was at case 211 (wrong — 211 is sendmsg). Moved to 212
        // which is the correct AArch64 syscall number per asm-generic/unistd.h.
        case 212: { // recvmsg(sockfd, msg, flags)
            if (!a1) { ret_err(EFAULT); return 0; }
            uint64_t msg_name = mem_.load<uint64_t>(a1);
            uint32_t msg_namelen = mem_.load<uint32_t>(a1 + 8);
            uint64_t msg_iov = mem_.load<uint64_t>(a1 + 16);
            uint64_t msg_iovlen = mem_.load<uint64_t>(a1 + 24);

            if (msg_iovlen > 1024) msg_iovlen = 1024;
            std::vector<iovec> iovs(msg_iovlen);
            std::vector<std::vector<uint8_t>> iov_bufs(msg_iovlen);
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t len = mem_.load<uint64_t>(msg_iov + i * 16 + 8);
                iov_bufs[i].resize(len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            std::vector<uint8_t> name_buf;
            if (msg_name && msg_namelen) name_buf.resize(msg_namelen);
            struct msghdr host_msg;
            memset(&host_msg, 0, sizeof(host_msg));
            host_msg.msg_name = name_buf.empty() ? nullptr : name_buf.data();
            host_msg.msg_namelen = msg_namelen;
            host_msg.msg_iov = iovs.data();
            host_msg.msg_iovlen = msg_iovlen;
            ssize_t r = ::recvmsg(static_cast<int>(a0), &host_msg, static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            // Write received data back to guest iovec buffers.
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t base = mem_.load<uint64_t>(msg_iov + i * 16);
                mem_.write(base, iov_bufs[i].data(), iov_bufs[i].size());
            }
            // Write source address back.
            if (msg_name && msg_namelen) {
                socklen_t actual = static_cast<socklen_t>(host_msg.msg_namelen);
                if (actual > msg_namelen) actual = msg_namelen;
                mem_.write(msg_name, name_buf.data(), actual);
                mem_.store<uint32_t>(a1 + 8, actual);
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── fadvise64 (syscall 223) ──────────────────────────────────
        case 223: { // fadvise64(fd, offset, len, advice)
            int r = ::posix_fadvise(static_cast<int>(a0),
                                    static_cast<off_t>(a1),
                                    static_cast<off_t>(a2),
                                    static_cast<int>(a3));
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-r)));
            return 0;
        }

        // ── close_range (syscall 436) ────────────────────────────────
        // BUGFIX: cap iteration to the actually-open fd range. The previous
        // loop iterated from a0 to a1 inclusive; if a1 was INT_MAX, this
        // looped 2 billion times calling fds_.close() on every integer.
        // We now iterate only over the FdTable's open entries.
        case 436: { // close_range(first, last, flags)
            int first = static_cast<int>(a0);
            int last  = static_cast<int>(a1);
            emu.fds().close_range(first, last);
            ret_host(0);
            return 0;
        }

        // ── openat2 (syscall 437) ────────────────────────────────────
        case 437: { // openat2(dirfd, pathname, how, size)
            uint64_t flags = a2 ? mem_.load<uint64_t>(a2) : 0;
            uint64_t mode = a2 ? mem_.load<uint64_t>(a2 + 8) : 0;
            std::string path = Yggdrasil::read_path(mem_, a1);
            int err = 0;
            auto node = emu.vfs().open(path, static_cast<int>(flags),
                                       static_cast<mode_t>(mode), &err);
            if (!node) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(err ? err : -ENOENT)));
                return 0;
            }
            ret_host(static_cast<uint64_t>(emu.fds().allocate(std::move(node))));
            return 0;
        }

        // ── faccessat2 (syscall 439) ─────────────────────────────────
        case 439: { // faccessat2(dirfd, pathname, mode, flags)
            std::string path = Yggdrasil::read_path(mem_, a1);
            std::string host = Yggdrasil::remap_path(path);
            int r = ::faccessat(static_cast<int>(a0), host.c_str(),
                               static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        case 94: { // exit_group
            cpu.running = false;
            cpu.exit_code = static_cast<int>(a0);
            return 0;
        }

        // ── signalfd4 (syscall 74) ────────────────────────────────────
        case 74: { // signalfd4(fd, mask, sizemask, flags)
            // Copy sigset_t from guest memory — a1 is a guest address,
            // NOT a host pointer. Using it directly would read garbage
            // from the host process's memory.
            if (!a1) { ret_err(EFAULT); return 0; }
            sigset_t host_mask;
            memset(&host_mask, 0, sizeof(host_mask));
            try {
                mem_.read(a1, &host_mask, sizeof(host_mask));
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            int fd = ::signalfd(static_cast<int>(a0), &host_mask,
                                static_cast<int>(a3));
            if (fd < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── getrandom (syscall 278) — already implemented above ──

        // ── 15+ new syscalls for broader compatibility ──────────────

        // NOTE: case 28 (formerly mislabeled "fchdir") removed — the real
        // AArch64 syscall 28 is inotify_rm_watch (handled correctly at
        // line 663 above). Real fchdir is syscall 50, handled in fs.cpp.

        case 36: { // symlinkat(old, newdirfd, new) — AArch64 36
            // AArch64 syscall 36 is symlinkat, NOT unlinkat (which is 35).
            // The old code dispatched 36 to unlinkat, which broke `ln -s`
            // (toybox calls symlinkat() via musl). unlinkat is correctly
            // handled at syscall 35 in fs.cpp.
            std::string oldp = Yggdrasil::read_path(mem_, a0);
            std::string newp = Yggdrasil::read_path(mem_, a2);
            int r = ::symlinkat(oldp.c_str(), static_cast<int>(a1), newp.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 42: { // nfsservctl — AArch64 42 (unimplemented, returns ENOSYS)
            // AArch64 syscall 42 is nfsservctl, NOT link. The old code
            // dispatched 42 to link(), but musl's link() wrapper calls
            // syscall 37 (linkat). linkat is now correctly handled at
            // syscall 37 in fs.cpp.
            ret_err(ENOSYS);
            return 0;
        }
        case 51: { // chroot(path) — AArch64 51
            // BUGFIX: was previously labeled "fchmod" but fchmod is at 52
            // (handled in fs.cpp). The real syscall at 51 is chroot. We
            // don't support chroot; return -EPERM (requires CAP_SYS_CHROOT).
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
            return 0;
        }
        case 54: { // fchownat(dirfd, path, owner, group, flags) — AArch64 54
            // BUGFIX: was previously labeled "fchmodat" but fchmodat is
            // at 53 (handled in fs.cpp). The real syscall at 54 is fchownat.
            std::string path = Yggdrasil::remap_path(Yggdrasil::read_path(mem_, a1));
            int r = ::fchownat(static_cast<int>(a0), path.c_str(),
                               static_cast<uid_t>(a2), static_cast<gid_t>(a3),
                               static_cast<int>(a4));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 55: { // fchown(fd, owner, group) — AArch64 55
            // BUGFIX: was previously labeled "faccessat2" but faccessat2
            // is at 439 (handled below). The real syscall at 55 is fchown.
            // Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::fchown(hfd, static_cast<uid_t>(a1), static_cast<gid_t>(a2));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 68: { // pwrite64(fd, buf, count, offset) — AArch64 68
            // BUGFIX: previously called ::pwrite(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            if (a2 == 0) { ret_host(0); return 0; }
            std::vector<uint8_t> buf(a2);
            try { mem_.read(a1, buf.data(), a2); } catch (...) {
                ret_err(EFAULT); return 0;
            }
            ssize_t r = node->write(static_cast<uint64_t>(a3), buf.data(), a2);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(r)); return 0; }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        case 69: { // preadv2(fd, iov, iovcnt, offset, flags) — AArch64 69
            // AArch64 syscall 69 is preadv2 (NOT readv — that's syscall 65,
            // already handled in fs.cpp). The old code here dispatched 69
            // to readv(), which silently mis-handled any preadv2 call.
            // preadv2 is rare in user-space; return ENOSYS for now. If a
            // guest program needs it, implement it by mirroring fs.cpp's
            // readv handler with the offset argument.
            ret_err(ENOSYS);
            return 0;
        }
        case 71: { // sendfile(out_fd, in_fd, offset, count) — AArch64 71
            // BUGFIX: previously called ::sendfile(guest_fd, guest_fd, ...)
            // directly, bypassing FdTable. Resolve both fds via FdTable so
            // virtual fds (memfd-backed /proc/*, /dev/fb0) work.
            auto out_node = fds_.get(static_cast<int>(a0));
            auto in_node  = fds_.get(static_cast<int>(a1));
            if (!out_node || !in_node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF));
                return 0;
            }
            int out_hfd = out_node->host_fd();
            int in_hfd  = in_node->host_fd();
            if (out_hfd < 0 || in_hfd < 0) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF));
                return 0;
            }
            off_t off = 0;
            off_t *offp = nullptr;
            if (a2 != 0) {
                try { off = mem_.load<off_t>(a2); offp = &off; } catch (...) {}
            }
            ssize_t r = ::sendfile(out_hfd, in_hfd, offp, a3);
            if (r < 0) { ret_errno(); return 0; }
            if (offp && a2) { try { mem_.store<off_t>(a2, off); } catch (...) {} }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        case 81: { // sync() — AArch64 81
            ::sync(); ret_host(0); return 0;
        }
        case 82: { // fsync(fd) — AArch64 82
            // BUGFIX: previously called ::fsync(guest_fd) directly. Resolve
            // via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::fsync(hfd);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 83: { // fdatasync(fd) — AArch64 83
            // BUGFIX: previously called ::fdatasync(guest_fd) directly.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::fdatasync(hfd);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 84: { // sync_file_range(fd, offset, nbytes, flags) — AArch64 84
            int r = ::sync_file_range(static_cast<int>(a0), static_cast<off_t>(a1), a2, static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 95: { // waitid(idtype, id, infop, options) — AArch64 95
            // BUGFIX (Turn 62 rev 3): ALWAYS write siginfo to guest.
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int r;
            while (true) {
                r = ::waitid(static_cast<idtype_t>(a0), static_cast<id_t>(a1), &si, static_cast<int>(a3));
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            // ALWAYS write siginfo to guest, even on error.
            if (a2) {
                try {
                    mem_.store<uint32_t>(a2, si.si_signo);
                    mem_.store<uint32_t>(a2 + 4, si.si_code);
                    mem_.store<uint32_t>(a2 + 8, si.si_pid);
                    mem_.store<uint32_t>(a2 + 12, si.si_uid);
                    mem_.store<uint32_t>(a2 + 16, si.si_status);
                } catch (...) {}
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 97: { // unshare(flags) — AArch64 97
            int r = ::unshare(static_cast<int>(a0));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        // BUGFIX: cases 122/123/125/140/141/149 were previously labeled
        // "legacy mkdir/rename/truncate/chown/fchown/flock" but AArch64 has
        // NO legacy syscalls at those numbers — the real syscalls are
        // sched_setaffinity (122), sched_getaffinity (123),
        // sched_get_priority_max (125), setpriority (140), getpriority (141),
        // and setresgid (149). All are now handled at the top of this file
        // (or for 149, removed since we don't implement setresgid). The
        // duplicate legacy handlers below have been removed.
        case 149: { // setresgid(rgid, egid, sgid) — AArch64 149
            // BUGFIX: previously labeled "flock" but flock is at 32. The
            // real syscall at 149 is setresgid. We're a single-user guest,
            // so accept and return 0.
            ret_host(0); return 0;
        }
        case 32: { // flock(fd, operation) — AArch64 32
            // BUGFIX: was previously at case 149 (wrong number). Real
            // AArch64 flock is at 32. Forward to host flock on the
            // underlying host fd (resolve via FdTable).
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::flock(hfd, static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 218: { // waitid (AArch64 218 = wait4 alias)
            // BUGFIX (Turn 62 rev 3): ALWAYS write siginfo to guest.
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int r = ::waitid(static_cast<idtype_t>(a0), static_cast<id_t>(a1), &si, static_cast<int>(a3));
            if (a2) {
                try {
                    mem_.store<uint32_t>(a2, si.si_signo);
                    mem_.store<uint32_t>(a2 + 4, si.si_code);
                    mem_.store<uint32_t>(a2 + 8, si.si_pid);
                    mem_.store<uint32_t>(a2 + 12, si.si_uid);
                    mem_.store<uint32_t>(a2 + 16, si.si_status);
                } catch (...) {}
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 219: { // set_robust_list(head, len) — AArch64 219
            // No-op — we don't implement robust futex lists.
            ret_host(0); return 0;
        }
        case 224: { // mremap(old_addr, old_size, new_size, flags, new_addr)
            // Forward to mem.cpp handler.
            return SYSCALL_NOT_HANDLED;  // handled by syscall_mem
        }

        // ── Bifrost-emu internal thunk syscall (Turn 37) ────────────
        // Trampolines generated by GraphicThunk use this syscall number
        // to trap into the host. x9 holds the symbol_id; the thunk looks
        // up the host function, reads args from x0..x7, calls it, and
        // writes the return value to x0.
        //
        // The number (0x1000 = 4096) is high enough to never collide
        // with real Linux AArch64 syscalls (which go up to ~451 as of
        // kernel 6.x). The thunk is opt-in (BIFROST_THUNK_GRAPHICS=1);
        // when disabled, this case is unreachable (no trampolines are
        // ever written to guest memory).
        case GraphicThunk::SYSCALL_NUMBER: {
            auto* thunk = emu.graphics_.thunk();
            if (!thunk || !thunk->enabled()) {
                ret_err(ENOSYS);
                return 0;
            }
            uint32_t sym_id = static_cast<uint32_t>(cpu.regs[9]);
            int64_t r = thunk->dispatch(cpu, sym_id);
            if (r < 0) {
                ret_host(r);  // negative = -errno
            }
            // On success, dispatch() already wrote the return value to
            // cpu.regs[0]; we just need to return 0 (handled).
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
