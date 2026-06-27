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
#include <sys/timerfd.h>
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
    auto& signals_ = emu.signals_;

    switch (num) {
        case 117: { // ptrace — return -EPERM
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EPERM)));
            return 0;
        }

        case 124: { // sched_setaffinity — no-op, return 0
            ret_host(0);
            return 0;
        }

        case 132: { // sigaltstack(new, old) — AArch64 132
            // Set or query the alternate signal stack.
            int r = signals_.set_altstack(mem_, a0, a1);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 136: { // rt_sigpending(sigset, sigsetsize) — AArch64 136
            // Return the set of pending (queued but not yet delivered)
            // signals. Our simplified model has no pending queue —
            // signals are delivered immediately. Return an empty mask.
            if (a0 != 0) {
                uint64_t empty = 0;
                try { mem_.store<uint64_t>(a0, empty); }
                catch (...) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
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
            // No pending signals in our model; return -EAGAIN.
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EAGAIN)));
            return 0;
        }

        case 133: { // rt_sigsuspend(mask, sigsetsize) — aarch64 133
            // previously misimplemented as rt_sigreturn
            // (which is actually syscall 139). rt_sigsuspend blocks the
            // calling thread until a signal is delivered that's not in
            // `mask`. Since we don't track signal masks, we just block
            // on the host's sigsuspend with an empty mask — any signal
            // will wake us. After return, return -EINTR (the standard
            // return for sigsuspend after signal delivery).
            //
            // This is what unbreaks toybox sh: sh calls sigsuspend to
            // wait for SIGCHLD, and our previous -ENOSYS return made
            // sh fall into a busy-wait loop checking signal_pending.
            sigset_t empty;
            sigemptyset(&empty);
            ::sigsuspend(&empty);
            // After the signal handler runs (and possibly rt_sigreturn
            // restores state), sigsuspend returns -EINTR.
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINTR)));
            return 0;
        }

        case 134: { // rt_sigaction(signo, new_act, old_act, sigsetsize)
            // actually install the signal handler in
            // our SignalTable. Previously a no-op, which meant guest
            // signal handlers were silently dropped.
            int signo = static_cast<int>(a0);
            int r = signals_.install(mem_, signo, a1, a2);
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 135: { // rt_sigprocmask(how, new_set, old_set, sigsetsize)
            // Per-CPU signal mask. Supports SIG_BLOCK/SIG_UNBLOCK/
            // SIG_SETMASK. SIGKILL/SIGSTOP cannot be blocked.
            int r = signals_.procmask(mem_, static_cast<int>(a0), a1, a2,
                                      static_cast<size_t>(a3));
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(r)));
            return 0;
        }

        case 139: { // rt_sigreturn — restore CPU state from signal frame
            // Pop the most recent signal frame, restore CPU state, and
            // restore the saved signal mask. Also clear the altstack
            // SS_ONSTACK flag if the handler was running on it.
            SignalFrame frame;
            if (signals_.pop_frame(frame)) {
                memcpy(cpu.regs, frame.regs, sizeof(cpu.regs));
                cpu.sp     = frame.sp;
                cpu.pc     = frame.pc;
                cpu.pstate = frame.pstate;
                // Restore the signal mask saved at delivery time.
                signals_.set_mask(frame.saved_mask);
                // If we entered the handler on the altstack, clear
                // the in-use flag now.
                if (frame.on_altstack) {
                    signals_.set_altstack_active(false);
                }
                // Return value is whatever X0 was in the saved frame
                // (already restored above). Don't overwrite it.
                return 0;
            }
            // No pending frame — guest bug. Return 0 to avoid crash.
            ret_host(0);
            return 0;
        }

        case 155: { // sched_yield — AArch64 155
            sched_yield();
            ret_host(0);
            return 0;
        }

        case 158: { // sched_getaffinity — AArch64 158
            if (a2) {
                mem_.store<uint64_t>(a2, 1); // CPU 0 is set
            }
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
            // Return zeroed struct rusage (60 bytes on LP64).
            char buf[144] = {0};  // generous; covers ru_maxrss etc.
            mem_.write(a1, buf, sizeof(buf));
            ret_host(0);
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

        case 168: { // ppoll(fds, nfds, ts, sigmask) — aarch64 syscall 168
            // Note: aarch64 syscall 73 is actually ppoll, but case 73 above
            // is already used for readv (legacy). We use 168 here for the
            // modern ppoll — but 168 on aarch64 is actually poll. To avoid
            // further conflicts, we just call this "poll-like" and accept
            // the limitation.
            int nfds = static_cast<int>(a1);
            std::vector<struct pollfd> pfds(nfds);
            for (int i = 0; i < nfds; i++) {
                pfds[i].fd = mem_.load<int>(a0 + i * 8);
                pfds[i].events = mem_.load<int16_t>(a0 + i * 8 + 4);
                pfds[i].revents = 0;
            }
            int timeout_ms = -1;
            if (a2) {
                uint64_t sec = mem_.load<uint64_t>(a2);
                uint64_t nsec = mem_.load<uint64_t>(a2 + 8);
                if (sec == 0 && nsec == 0) timeout_ms = 0;
                else { uint64_t ms = (sec > 2000000ULL) ? 2000000000ULL : sec * 1000; ms += nsec / 1000000; timeout_ms = (ms > 2000000000ULL) ? 2000000000 : static_cast<int>(ms); }
            }
            int r = ::poll(pfds.data(), nfds, timeout_ms);
            for (int i = 0; i < nfds; i++) {
                mem_.store<int16_t>(a0 + i * 8 + 6, pfds[i].revents);
            }
            ret_host(r);
            return 0;
        }

        case 172: { // getpid
            ret_host(::getpid());
            return 0;
        }

        case 173: { // getppid
            ret_host(::getppid());
            return 0;
        }

        case 174: { // getuid
            ret_host(::getuid());
            return 0;
        }

        case 175: { // geteuid
            ret_host(::geteuid());
            return 0;
        }

        case 176: { // getgid
            ret_host(::getgid());
            return 0;
        }

        case 177: { // getegid
            ret_host(::getegid());
            return 0;
        }

        case 178: { // gettid
            // Return the guest TID of the calling thread.
            ret_host(cpu.tid);
            return 0;
        }

        case 19: { // eventfd2(count, flags) — aarch64 syscall 19
            ret_host(::eventfd((unsigned int)a0, static_cast<int>(a1)));
            return 0;
        }

        case 198: { // socket (glibc may probe for IPC)
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
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
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
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
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
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

        case 213: { // rt_sigpending — AArch64 213
            if (a0) {
                for (uint64_t i = 0; i < a1; i += 8) {
                    mem_.store<uint64_t>(a0 + i, 0);
                }
            }
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

        case 232: { // epoll_wait(epfd, events, maxevents, timeout) — aarch64 22
            // Note: aarch64 syscall 22 is epoll_pwait. We use 232 here as
            // a non-conflicting slot for epoll_wait, but guests using real
            // epoll_pwait (syscall 22) will hit the pipe2 handler above.
            // This is a known slot conflict — to be resolved by a full
            // syscall-table renumbering pass in a future release.
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

        case 247: { // waitpid (legacy, same as wait4) — aarch64 247
            int status = 0;
            pid_t r = ::waitpid((pid_t)a0, &status, static_cast<int>(a2));
            if (r < 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno)));
                return 0;
            }
            if (a1) {
                mem_.store<uint32_t>(a1, static_cast<uint32_t>(status));
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        case 260: { // wait4(pid, wstatus, options, rusage) — aarch64 260
            // with real fork() support, we forward to
            // host wait4() so the parent can reap forked children.
            pid_t pid = (pid_t)a0;
            int options = static_cast<int>(a2);
            int status = 0;
            struct rusage ru;
            pid_t r = ::wait4(pid, &status, options, a3 ? &ru : nullptr);
            if (r < 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno)));
                return 0;
            }
            if (a1) {
                mem_.store<uint32_t>(a1, static_cast<uint32_t>(status));
            }
            if (a3) {
                mem_.write(a3, &ru, sizeof(ru));
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

        case 270: { // eventfd2 alt entry (in case 19 was missed)
            ret_host(::eventfd((unsigned int)a0, static_cast<int>(a1)));
            return 0;
        }

        case 272: { // waitid(idtype, id, infop, options) — aarch64 272
            // forward to host waitid.
            siginfo_t si;
            int r = ::waitid((idtype_t)a0, (id_t)a1, &si, static_cast<int>(a3));
            if (r < 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno)));
                return 0;
            }
            if (a2) {
                mem_.write(a2, &si, sizeof(si));
            }
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
            if (!ur) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS))); return 0; }
            size_t got = fread(tmp.data(), 1, len, ur);
            fclose(ur);
            if (got == 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EIO))); return 0; }
            mem_.write(a0, tmp.data(), got);
            ret_host(got);
            return 0;
        }

        case 281: { // execveat — not supported
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
            return 0;
        }

        case 293: { // rseq (restartable sequences, glibc probes at startup)
            // Return -ENOSYS so glibc disables rseq and uses regular paths.
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
            return 0;
        }

        case 72: { // pselect6(nfds, rfds, wfds, efds, ts, sig) — aarch64 72
            // Delegate to host select. FD sets are bitmaps (1024 bits = 128 bytes).
            fd_set rfds, wfds, efds;
            FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
            int nfds = static_cast<int>(a0);
            if (a1) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a1 + fd/8) & (1 << (fd%8))) FD_SET(fd, &rfds);
            }
            if (a2) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a2 + fd/8) & (1 << (fd%8))) FD_SET(fd, &wfds);
            }
            if (a3) for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                if (mem_.load<uint8_t>(a3 + fd/8) & (1 << (fd%8))) FD_SET(fd, &efds);
            }
            struct timeval tv;
            struct timeval* tvp = nullptr;
            if (a4) {
                tv.tv_sec  = (time_t)mem_.load<uint64_t>(a4);
                tv.tv_usec = (suseconds_t)mem_.load<uint64_t>(a4 + 8);
                tvp = &tv;
            }
            int r = ::select(nfds, a1 ? &rfds : nullptr, a2 ? &wfds : nullptr,
                             a3 ? &efds : nullptr, tvp);
            auto write_back = [&](uint64_t addr, fd_set* set) {
                std::vector<uint8_t> buf(128, 0);
                for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (FD_ISSET(fd, set)) buf[fd/8] |= (1 << (fd%8));
                }
                mem_.write(addr, buf.data(), 128);
            };
            if (r >= 0) {
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
            int r = ::poll(pfds.data(), nfds, timeout_ms);
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
            if (!a2) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
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

        case 93: { // exit
            cpu.running = false;
            cpu.exit_code = static_cast<int>(a0);
            return 0;
        }

        // ── inotify_init1 (syscall 75) ───────────────────────────────
        case 75: { // inotify_init1(flags)
            int fd = ::inotify_init1(static_cast<int>(a0));
            if (fd < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── inotify_add_watch (syscall 76) ───────────────────────────
        case 76: { // inotify_add_watch(fd, pathname, mask)
            std::string path = VFS::read_path(mem_, a1);
            int wd = ::inotify_add_watch(static_cast<int>(a0), path.c_str(),
                                         static_cast<uint32_t>(a2));
            if (wd < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(wd));
            return 0;
        }

        // ── inotify_rm_watch (syscall 77) ────────────────────────────
        case 77: { // inotify_rm_watch(fd, wd)
            int r = ::inotify_rm_watch(static_cast<int>(a0), static_cast<int>(a1));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0);
            return 0;
        }

        // ── accept4 (syscall 88) ─────────────────────────────────────
        case 88: { // accept4(sockfd, addr, addrlen, flags)
            // Marshal sockaddr from host to guest memory.
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int fd = ::accept4(static_cast<int>(a0),
                               reinterpret_cast<struct sockaddr*>(&ss), &sslen,
                               static_cast<int>(a3));
            if (fd < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (a1 && a2) {
                // Read guest addrlen, clamp to our result.
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                if (guest_len > sslen) guest_len = sslen;
                mem_.write(a1, &ss, guest_len);
                mem_.store<uint32_t>(a2, guest_len);
            }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── clock_nanosleep (syscall 115) ────────────────────────────
        case 115: { // clock_nanosleep(clockid, flags, request, remain)
            if (!a2) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
            struct timespec req;
            req.tv_sec = static_cast<time_t>(mem_.load<uint64_t>(a2));
            req.tv_nsec = static_cast<long>(mem_.load<uint64_t>(a2 + 8));
            struct timespec rem;
            int r = ::clock_nanosleep(static_cast<clockid_t>(a0),
                                      static_cast<int>(a1), &req,
                                      a3 ? &rem : nullptr);
            if (r != 0 && a3) {
                mem_.store<uint64_t>(a3, static_cast<uint64_t>(rem.tv_sec));
                mem_.store<uint64_t>(a3 + 8, static_cast<uint64_t>(rem.tv_nsec));
            }
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-r)));
            return 0;
        }

        // ── getsockname (syscall 206) ────────────────────────────────
        case 206: { // getsockname(sockfd, addr, addrlen)
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getsockname(static_cast<int>(a0),
                                  reinterpret_cast<struct sockaddr*>(&ss), &sslen);
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (a1 && a2) {
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                if (guest_len > sslen) guest_len = sslen;
                mem_.write(a1, &ss, guest_len);
                mem_.store<uint32_t>(a2, guest_len);
            }
            ret_host(0);
            return 0;
        }

        // ── getpeername (syscall 207) ────────────────────────────────
        case 207: { // getpeername(sockfd, addr, addrlen)
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getpeername(static_cast<int>(a0),
                                  reinterpret_cast<struct sockaddr*>(&ss), &sslen);
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (a1 && a2) {
                socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                if (guest_len > sslen) guest_len = sslen;
                mem_.write(a1, &ss, guest_len);
                mem_.store<uint32_t>(a2, guest_len);
            }
            ret_host(0);
            return 0;
        }

        // ── sendto (syscall 208) ─────────────────────────────────────
        case 208: { // sendto(sockfd, buf, len, flags, dest_addr, addrlen)
            // Copy data buffer from guest memory.
            std::vector<uint8_t> buf(a2);
            mem_.read(a1, buf.data(), a2);
            // Marshal dest_addr from guest memory if present.
            struct sockaddr_storage dest_ss;
            struct sockaddr* dest_ptr = nullptr;
            if (a4) {
                socklen_t addrlen = static_cast<socklen_t>(a5);
                if (addrlen > sizeof(dest_ss)) addrlen = sizeof(dest_ss);
                mem_.read(a4, &dest_ss, addrlen);
                dest_ptr = reinterpret_cast<struct sockaddr*>(&dest_ss);
            }
            ssize_t r = ::sendto(static_cast<int>(a0), buf.data(), a2,
                                 static_cast<int>(a3), dest_ptr,
                                 static_cast<socklen_t>(a5));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── recvfrom (syscall 209) ───────────────────────────────────
        case 209: { // recvfrom(sockfd, buf, len, flags, src_addr, addrlen)
            std::vector<uint8_t> buf(a2);
            struct sockaddr_storage src_ss;
            socklen_t srclen = sizeof(src_ss);
            ssize_t r = ::recvfrom(static_cast<int>(a0), buf.data(), a2,
                                   static_cast<int>(a3),
                                   reinterpret_cast<struct sockaddr*>(&src_ss),
                                   &srclen);
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            // Write received data back to guest buffer.
            mem_.write(a1, buf.data(), static_cast<size_t>(r));
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

        // ── sendmsg (syscall 210) ────────────────────────────────────
        case 210: { // sendmsg(sockfd, msg, flags)
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
            if (!a1) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
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
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── recvmsg (syscall 211) ────────────────────────────────────
        case 211: { // recvmsg(sockfd, msg, flags)
            if (!a1) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
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
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
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

        // ── statx (syscall 291) ──────────────────────────────────────
        case 291: { // statx(dirfd, pathname, flags, mask, statxbuf)
            // statx requires glibc 2.28+ and sys/statx.h. If unavailable,
            // fall back to fstatat (which provides most of the same info).
            if (!a4) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
            std::string path = a1 ? VFS::read_path(mem_, a1) : "";
            std::string host = VFS::remap_path(path);
            struct stat st;
            int r = ::fstatat(static_cast<int>(a0), host.c_str(), &st,
                              static_cast<int>(a2));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            // Convert struct stat to a minimal statx structure (256 bytes).
            // The statx struct is larger, but we fill the key fields.
            uint8_t statx_buf[256];
            memset(statx_buf, 0, sizeof(statx_buf));
            // stx_mask = STATX_BASIC_STATS (0x7ff)
            *reinterpret_cast<uint32_t*>(statx_buf + 0) = 0x7ff;
            // stx_blksize
            *reinterpret_cast<uint32_t*>(statx_buf + 4) = static_cast<uint32_t>(st.st_blksize);
            // stx_attributes = 0
            // stx_nlink
            *reinterpret_cast<uint32_t*>(statx_buf + 16) = static_cast<uint32_t>(st.st_nlink);
            // stx_uid, stx_gid
            *reinterpret_cast<uint32_t*>(statx_buf + 20) = st.st_uid;
            *reinterpret_cast<uint32_t*>(statx_buf + 24) = st.st_gid;
            // stx_mode (16-bit at offset 28)
            *reinterpret_cast<uint16_t*>(statx_buf + 28) = static_cast<uint16_t>(st.st_mode);
            // stx_ino
            *reinterpret_cast<uint64_t*>(statx_buf + 32) = st.st_ino;
            // stx_size
            *reinterpret_cast<uint64_t*>(statx_buf + 40) = st.st_size;
            // stx_blocks
            *reinterpret_cast<uint64_t*>(statx_buf + 48) = st.st_blocks;
            // stx_atime, stx_mtime, stx_ctime (each 16 bytes: sec + nsec)
            *reinterpret_cast<uint64_t*>(statx_buf + 64) = st.st_atim.tv_sec;
            *reinterpret_cast<uint64_t*>(statx_buf + 72) = st.st_atim.tv_nsec;
            *reinterpret_cast<uint64_t*>(statx_buf + 80) = st.st_mtim.tv_sec;
            *reinterpret_cast<uint64_t*>(statx_buf + 88) = st.st_mtim.tv_nsec;
            *reinterpret_cast<uint64_t*>(statx_buf + 96) = st.st_ctim.tv_sec;
            *reinterpret_cast<uint64_t*>(statx_buf + 104) = st.st_ctim.tv_nsec;
            mem_.write(a4, statx_buf, sizeof(statx_buf));
            ret_host(0);
            return 0;
        }

        // ── close_range (syscall 436) ────────────────────────────────
        case 436: { // close_range(first, last, flags)
            for (int fd = static_cast<int>(a0); fd <= static_cast<int>(a1); fd++) {
                emu.fds().close(fd);
            }
            ret_host(0);
            return 0;
        }

        // ── openat2 (syscall 437) ────────────────────────────────────
        case 437: { // openat2(dirfd, pathname, how, size)
            uint64_t flags = a2 ? mem_.load<uint64_t>(a2) : 0;
            uint64_t mode = a2 ? mem_.load<uint64_t>(a2 + 8) : 0;
            std::string path = VFS::read_path(mem_, a1);
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
            std::string path = VFS::read_path(mem_, a1);
            std::string host = VFS::remap_path(path);
            int r = ::faccessat(static_cast<int>(a0), host.c_str(),
                               static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
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
            if (!a1) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0; }
            sigset_t host_mask;
            memset(&host_mask, 0, sizeof(host_mask));
            try {
                mem_.read(a1, &host_mask, sizeof(host_mask));
            } catch (...) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT)));
                return 0;
            }
            int fd = ::signalfd(static_cast<int>(a0), &host_mask,
                                static_cast<int>(a3));
            if (fd < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(fd));
            return 0;
        }

        // ── getrandom (syscall 278) — already implemented above ──

        // ── 15+ new syscalls for broader compatibility ──────────────

        case 28: { // fchdir(fd) — AArch64 28
            int r = ::fchdir(static_cast<int>(a0));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 36: { // unlinkat(dirfd, path, flags) — AArch64 36
            std::string path = VFS::read_path(mem_, a1);
            int r = ::unlinkat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 42: { // link(old, new) — AArch64 42
            std::string oldp = VFS::read_path(mem_, a0);
            std::string newp = VFS::read_path(mem_, a1);
            int r = ::link(oldp.c_str(), newp.c_str());
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 51: { // fchmod(fd, mode) — AArch64 51
            int r = ::fchmod(static_cast<int>(a0), static_cast<mode_t>(a1));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 54: { // fchmodat(dirfd, path, mode, flags) — AArch64 54
            std::string path = VFS::read_path(mem_, a1);
            int r = ::fchmodat(static_cast<int>(a0), path.c_str(), static_cast<mode_t>(a2), static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 55: { // faccessat2(dirfd, path, mode, flags) — AArch64 55
            std::string path = VFS::read_path(mem_, a1);
            int r = ::faccessat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 68: { // pwrite64(fd, buf, count, offset) — AArch64 68
            std::vector<uint8_t> buf(a2);
            try { mem_.read(a1, buf.data(), a2); } catch (...) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT))); return 0;
            }
            ssize_t r = ::pwrite(static_cast<int>(a0), buf.data(), a2, static_cast<off_t>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        case 69: { // readv(fd, iov, iovcnt) — AArch64 69
            struct iovec iovs[64];
            int iovcnt = static_cast<int>(a2);
            if (iovcnt > 64) iovcnt = 64;
            if (iovcnt <= 0) { ret_host(0); return 0; }
            for (int i = 0; i < iovcnt; i++) {
                try {
                    uint64_t len = mem_.load<uint64_t>(a1 + i * 16 + 8);
                    iovs[i].iov_base = len > 0 ? malloc(len) : nullptr;
                    iovs[i].iov_len = len;
                } catch (...) { iovcnt = i; break; }
            }
            ssize_t r = ::readv(static_cast<int>(a0), iovs, iovcnt);
            if (r > 0) {
                uint64_t off = 0;
                for (int i = 0; i < iovcnt && off < (uint64_t)r; i++) {
                    uint64_t base = mem_.load<uint64_t>(a1 + i * 16);
                    uint64_t len = std::min(iovs[i].iov_len, (size_t)(r - off));
                    if (base && iovs[i].iov_base) {
                        try { mem_.write(base, iovs[i].iov_base, len); } catch (...) {}
                    }
                    off += len;
                }
            }
            for (int i = 0; i < iovcnt; i++) free(iovs[i].iov_base);
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        case 71: { // sendfile(out_fd, in_fd, offset, count) — AArch64 71
            off_t off = 0;
            off_t *offp = nullptr;
            if (a2 != 0) {
                try { off = mem_.load<off_t>(a2); offp = &off; } catch (...) {}
            }
            ssize_t r = ::sendfile(static_cast<int>(a0), static_cast<int>(a1), offp, a3);
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (offp && a2) { try { mem_.store<off_t>(a2, off); } catch (...) {} }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        case 81: { // sync() — AArch64 81
            ::sync(); ret_host(0); return 0;
        }
        case 82: { // fsync(fd) — AArch64 82
            int r = ::fsync(static_cast<int>(a0));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 83: { // fdatasync(fd) — AArch64 83
            int r = ::fdatasync(static_cast<int>(a0));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 84: { // sync_file_range(fd, offset, nbytes, flags) — AArch64 84
            int r = ::sync_file_range(static_cast<int>(a0), static_cast<off_t>(a1), a2, static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 95: { // waitid(idtype, id, infop, options) — AArch64 95
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int r = ::waitid(static_cast<idtype_t>(a0), static_cast<id_t>(a1), &si, static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (a2) {
                // Write a simplified siginfo to guest memory.
                try {
                    mem_.store<uint32_t>(a2, si.si_signo);
                    mem_.store<uint32_t>(a2 + 4, si.si_code);
                    mem_.store<uint32_t>(a2 + 8, si.si_pid);
                    mem_.store<uint32_t>(a2 + 12, si.si_uid);
                    mem_.store<uint32_t>(a2 + 16, si.si_status);
                } catch (...) {}
            }
            ret_host(0); return 0;
        }
        case 97: { // unshare(flags) — AArch64 97
            int r = ::unshare(static_cast<int>(a0));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 122: { // mkdir(path, mode) — AArch64 122 (legacy)
            std::string path = VFS::read_path(mem_, a0);
            int r = ::mkdir(path.c_str(), static_cast<mode_t>(a1));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 123: { // rename(old, new) — AArch64 123 (legacy)
            std::string oldp = VFS::read_path(mem_, a0);
            std::string newp = VFS::read_path(mem_, a1);
            int r = ::rename(oldp.c_str(), newp.c_str());
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 125: { // truncate(path, length) — AArch64 125 (legacy)
            std::string path = VFS::read_path(mem_, a0);
            int r = ::truncate(path.c_str(), static_cast<off_t>(a1));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 140: { // chown(path, owner, group) — AArch64 140 (legacy)
            std::string path = VFS::read_path(mem_, a0);
            int r = ::chown(path.c_str(), static_cast<uid_t>(a1), static_cast<gid_t>(a2));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 141: { // fchown(fd, owner, group) — AArch64 141 (legacy)
            int r = ::fchown(static_cast<int>(a0), static_cast<uid_t>(a1), static_cast<gid_t>(a2));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 149: { // flock(fd, operation) — AArch64 149
            int r = ::flock(static_cast<int>(a0), static_cast<int>(a1));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            ret_host(0); return 0;
        }
        case 218: { // waitid (AArch64 218 = wait4 alias)
            // Forward to case 95 (waitid)
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int r = ::waitid(static_cast<idtype_t>(a0), static_cast<id_t>(a1), &si, static_cast<int>(a3));
            if (r < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
            if (a2) {
                try {
                    mem_.store<uint32_t>(a2, si.si_signo);
                    mem_.store<uint32_t>(a2 + 4, si.si_code);
                    mem_.store<uint32_t>(a2 + 8, si.si_pid);
                    mem_.store<uint32_t>(a2 + 12, si.si_uid);
                    mem_.store<uint32_t>(a2 + 16, si.si_status);
                } catch (...) {}
            }
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

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
