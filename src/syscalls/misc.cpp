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

namespace arm64emu {

int64_t syscall_misc(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& signals_ = emu.signals_;
    auto ret_host = [&](int64_t r) { cpu.regs[0] = static_cast<uint64_t>(r); };

    switch (num) {
        case 117: { // ptrace — return -EPERM
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EPERM)));
            return 0;
        }

        case 124: { // sched_setaffinity — no-op, return 0
            ret_host(0);
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
            ret_host(r);
            return 0;
        }

        case 135: { // rt_sigprocmask(how, new_set, old_set, sigsetsize)
            // Still a no-op — we don't track signal masks. musl's
            // libc startup calls this; returning 0 lets it proceed.
            // (If we later track masks, we'd store them per-CPU and
            // check them in deliver_signal().)
            ret_host(0);
            return 0;
        }

        case 139: { // rt_sigreturn — restore CPU state from signal frame
            // corrected syscall number (was wrongly at
            // case 133, which is actually rt_sigsuspend). Pop the most
            // recent signal frame and restore the saved CPU state. The
            // "return value" of this syscall is irrelevant — we restore
            // PC, so the dispatcher will continue at the saved PC, not
            // at the instruction after the SVC.
            SignalFrame frame;
            if (signals_.pop_frame(frame)) {
                memcpy(cpu.regs, frame.regs, sizeof(cpu.regs));
                cpu.sp     = frame.sp;
                cpu.pc     = frame.pc;
                cpu.pstate = frame.pstate;
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

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
