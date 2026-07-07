// syscalls/misc_io.cpp — I/O syscalls extracted from misc.cpp.
// Handles:
//   - eventfd2 (case 19)
//   - epoll_create1 / epoll_ctl / epoll_pwait (case 20, 21, 22)
//   - pselect6 / ppoll (case 72, 73)
//   - timerfd_create / timerfd_settime / timerfd_gettime (case 85, 86, 87)
//   - socket / socketpair / bind / listen / accept / connect (case 198-203)
//
// All case bodies are extracted verbatim from the original misc.cpp.
//
// NOTE: this file is NOT a friend of Emulator (unlike misc.cpp). It accesses
// private state via the public accessors emu.mem(), emu.fds(), etc.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"

#include <errno.h>
#include <poll.h>
#include <vector>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

namespace arm64emu {

int64_t syscall_misc_io(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();

    switch (num) {
        case 19: { // eventfd2(count, flags) — aarch64 syscall 19
            ret_host(::eventfd((unsigned int)a0, static_cast<int>(a1)));
            return 0;
        }

        case 20: { // epoll_create1(flags) — aarch64 syscall 20
            ret_host(::epoll_create1(static_cast<int>(a0)));
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

        default:
            return SYSCALL_NOT_HANDLED;
    }
}

} // namespace arm64emu
