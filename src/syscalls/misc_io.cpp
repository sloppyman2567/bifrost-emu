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
//
// 1.5.3-alpha: network handling refinement.
//   - All socket/pipe/eventfd/timerfd/epoll host fds are now wrapped in
//     HostNode and registered in the FdTable. This fixes a long-standing
//     bug where close() on a socket fd returned -EBADF (because the fd
//     wasn't in FdTable), leaking the host fd.
//   - Network syscalls (bind/connect/listen/accept/...) now resolve the
//     guest fd via FdTable to get the host fd, so guest fds and host fds
//     are properly decoupled.
//   - accept() now properly returns the peer address (was passing NULL).
//   - epoll_pwait now honors the sigmask (instead of ignoring it).
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "debug_flags.h"
#include "syscalls/syscalls.h"
#include "yggdrasil/host_node.hpp"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <vector>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
namespace arm64emu {
// ── Guest-fd → host-fd resolution for socket-style fds ──────────────────
// Returns the host fd backing a guest fd, or -1 if the fd is invalid or
// not backed by a host fd. This is the network-syscall analogue of
// fs.cpp's resolve_dirfd — we look up the guest fd in the FdTable and
// return the underlying host fd. For fds that were never registered in
// the FdTable (e.g. stdin/stdout/stderr which are HostNode-wrapped but
// owned by StdioNode, or fds created before this refactor that
// bypassed the FdTable), we fall back to using the guest fd as the host
// fd. This preserves backward compatibility for any code path that
// somehow obtained a raw host fd.
static inline int resolve_sock_fd(Emulator& emu, uint64_t guest_fd) {
    int gfd = static_cast<int>(static_cast<int64_t>(guest_fd));
    if (gfd < 0) return -1;
    auto node = emu.fds().get(gfd);
    if (node) {
        int hfd = node->host_fd();
        return hfd;  // may be -1 for virtual nodes
    }
    // Not in FdTable — fall back to using the guest fd as the host fd.
    // This preserves backward compatibility for pre-Turn-102 callers that
    // somehow obtained a raw host fd (e.g. via dup of stdin/stdout).
    return gfd;
}
// Helper: register a freshly-created host fd in the FdTable and return
// the guest fd. Uses O_RDWR as the default flags (sockets don't have a
// meaningful "flags" field at creation time — they're full-duplex).
static inline int register_host_fd(Emulator& emu, int hfd, int flags = O_RDWR) {
    if (hfd < 0) return hfd;
    return emu.fds().allocate(std::make_shared<yggdrasil::HostNode>(hfd, flags));
}
// Helper: marshal a sockaddr from guest memory into a stack buffer.
// Returns the actual length to pass to the host syscall (clamped to
// sizeof(sockaddr_storage)) or 0 if no addr was provided.
static inline socklen_t marshal_sockaddr_in(Memory& mem, uint64_t guest_addr,
                                            uint64_t guest_len,
                                            struct sockaddr_storage* ss) {
    if (!guest_addr || !guest_len) return 0;
    socklen_t len = static_cast<socklen_t>(guest_len);
    if (len > sizeof(*ss)) len = sizeof(*ss);
    mem.read(guest_addr, ss, len);
    return len;
}
// Helper: write a host sockaddr back into guest memory.
// Reads the guest's addrlen pointer, clamps it to the actual length,
// writes the sockaddr, and writes back the (possibly clamped) length.
static inline void marshal_sockaddr_out(Memory& mem, uint64_t guest_addr,
                                        uint64_t guest_len_ptr,
                                        const struct sockaddr_storage* ss,
                                        socklen_t actual_len) {
    if (!guest_addr || !guest_len_ptr) return;
    socklen_t guest_len = static_cast<socklen_t>(mem.load<uint32_t>(guest_len_ptr));
    if (guest_len > actual_len) guest_len = actual_len;
    mem.write(guest_addr, ss, guest_len);
    mem.store<uint32_t>(guest_len_ptr, guest_len);
}
int64_t syscall_misc_io(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    switch (num) {
        case 19: { // eventfd2(count, flags) — aarch64 syscall 19
            int hfd = ::eventfd(static_cast<unsigned int>(a0), static_cast<int>(a1));
            if (hfd < 0) { ret_errno(); return 0; }
            int gfd = register_host_fd(emu, hfd, O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d eventfd2 hfd=%d gfd=%d\n", cpu.tid, hfd, gfd);
            ret_host(gfd);
            return 0;
        }
        case 20: { // epoll_create1(flags) — aarch64 syscall 20
            int hfd = ::epoll_create1(static_cast<int>(a0));
            if (hfd < 0) { ret_errno(); return 0; }
            int gfd = register_host_fd(emu, hfd, O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d epoll_create1 hfd=%d gfd=%d\n", cpu.tid, hfd, gfd);
            ret_host(gfd);
            return 0;
        }
        case 21: { // epoll_ctl(epfd, op, fd, event) — aarch64 syscall 21
            // struct epoll_event: { uint32_t events; epoll_data_t data; }
            // epoll_data_t is a union with uint64_t as the largest member.
            // On aarch64 Linux this is packed to 12 bytes total.
            int epfd = resolve_sock_fd(emu, a0);
            int fd   = resolve_sock_fd(emu, a2);
            struct epoll_event ev;
            ev.events = mem_.load<uint32_t>(a3);
            ev.data.u64 = mem_.load<uint64_t>(a3 + 4);
            int r = ::epoll_ctl(epfd, static_cast<int>(a1), fd, &ev);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 22: { // epoll_pwait(epfd, events, maxevents, timeout, sigmask)
            // Real AArch64 syscall 22. We forward to epoll_wait and honor
            // the sigmask by temporarily masking the guest's signals
            // around the host epoll_wait (so a guest signal arrives only
            // after epoll_wait returns, matching real kernel semantics).
            int epfd = resolve_sock_fd(emu, a0);
            struct epoll_event evs[256];
            int maxev = static_cast<int>(a2);
            if (maxev > 256) maxev = 256;
            if (maxev < 0) maxev = 0;
            // Save current sigmask, apply guest sigmask if provided.
            sigset_t guestmask;
            bool have_mask = false;
            if (a4) {
                memset(&guestmask, 0, sizeof(guestmask));
                try {
                    mem_.read(a4, &guestmask, sizeof(uint64_t) * 2);
                    have_mask = true;
                } catch (...) { /* ignore — proceed without mask */ }
            }
            int n;
            if (have_mask) {
                n = ::epoll_pwait(epfd, evs, maxev, static_cast<int>(a3),
                                  &guestmask);
            } else {
                n = ::epoll_wait(epfd, evs, maxev, static_cast<int>(a3));
            }
            if (n < 0) { ret_errno(); return 0; }
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    uint64_t p = a1 + static_cast<uint64_t>(i) * 12;
                    try {
                        mem_.store<uint32_t>(p, evs[i].events);
                        mem_.store<uint64_t>(p + 4, evs[i].data.u64);
                    } catch (...) { ret_err(EFAULT); return 0; }
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
            int max_hfd = 0;
            auto fill_set = [&](uint64_t addr, fd_set* set, int* maxfd_out) {
                if (!addr) return;
                for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
                    if (mem_.load<uint8_t>(addr + fd/8) & (1 << (fd%8))) {
                        int hfd = resolve_sock_fd(emu, fd);
                        if (hfd >= 0 && hfd < FD_SETSIZE) {
                            FD_SET(hfd, set);
                            if (hfd > *maxfd_out) *maxfd_out = hfd;
                        }
                    }
                }
            };
            while (true) {
                // Rebuild fd_sets each iteration — select modifies them
                // in place, so on EINTR retry we need fresh copies.
                FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
                max_hfd = 0;
                fill_set(a1, &rfds, &max_hfd);
                fill_set(a2, &wfds, &max_hfd);
                fill_set(a3, &efds, &max_hfd);
                r = ::select(max_hfd + 1, a1 ? &rfds : nullptr, a2 ? &wfds : nullptr,
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
                        int hfd = resolve_sock_fd(emu, fd);
                        if (hfd >= 0 && hfd < FD_SETSIZE && FD_ISSET(hfd, set))
                            buf[fd/8] |= (1 << (fd%8));
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
                pfds[i].fd      = resolve_sock_fd(emu, mem_.load<int>(a0 + static_cast<uint64_t>(i) * 8));
                pfds[i].events  = mem_.load<int16_t>(a0 + static_cast<uint64_t>(i) * 8 + 4);
                pfds[i].revents = 0;
            }
            if (dbg().ppoll) {
                fprintf(stderr, "[PPOLL t%d] nfds=%d to=%lds%dms pc=0x%llx x30=0x%llx", cpu.tid, nfds,
                        a2 ? mem_.load<uint64_t>(a2) : -1,
                        a2 ? (int)(mem_.load<uint64_t>(a2 + 8)) : -1,
                        static_cast<unsigned long long>(cpu.pc),
                        static_cast<unsigned long long>(cpu.regs[30]));
                for (int i = 0; i < nfds && i < 16; i++)
                    fprintf(stderr, " f%d:%d/0x%x", i, pfds[i].fd, pfds[i].events);
                fprintf(stderr, "\n");
                if (nfds > 0 && dbg().ppoll_peek) {
                    for (int i = 0; i < nfds && i < 4; i++) {
                        int hfd = pfds[i].fd;
                        if (hfd < 0) continue;
                        uint8_t pb[64] = {0};
                        ssize_t pn = ::recv(hfd, pb, sizeof(pb), MSG_PEEK | MSG_DONTWAIT);
                        fprintf(stderr, "[PPOLLPEEK t%d pre %d (hfd=%d) pending=%zd first=%02x%02x%02x%02x\n",
                                cpu.tid, i, hfd, pn, pb[0], pb[1], pb[2], pb[3]);
                    }
                }
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
            if (dbg().ppoll) {
                fprintf(stderr, "[PPOLL t%d] ret=%d", cpu.tid, r);
                for (int i = 0; i < nfds && i < 16; i++)
                    if (pfds[i].revents)
                        fprintf(stderr, " f%d:%d/0x%x", i, pfds[i].fd, pfds[i].revents);
                fprintf(stderr, "\n");
                if (nfds > 0 && dbg().ppoll_peek) {
                    for (int i = 0; i < nfds && i < 4; i++) {
                        int hfd = pfds[i].fd;
                        if (hfd < 0) continue;
                        uint8_t pb[64] = {0};
                        ssize_t pn = ::recv(hfd, pb, sizeof(pb), MSG_PEEK | MSG_DONTWAIT);
                        fprintf(stderr, "[PPOLLPEEK t%d post %d (hfd=%d) pending=%zd first=%02x%02x%02x%02x\n",
                                cpu.tid, i, hfd, pn, pb[0], pb[1], pb[2], pb[3]);
                    }
                }
            }
            ret_host(r);
            return 0;
        }
        case 85: { // timerfd_create(clockid, flags) — aarch64 syscall 85
            int hfd = ::timerfd_create(static_cast<int>(a0), static_cast<int>(a1));
            if (hfd < 0) { ret_errno(); return 0; }
            int gfd = register_host_fd(emu, hfd, O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d timerfd_create hfd=%d gfd=%d\n", cpu.tid, hfd, gfd);
            ret_host(gfd);
            return 0;
        }
        case 86: { // timerfd_settime(fd, flags, new, old) — aarch64 syscall 86
            if (!a2) { ret_err(EFAULT); return 0; }
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct itimerspec newv;
            struct itimerspec oldv;
            newv.it_interval.tv_sec  = static_cast<time_t>(mem_.load<uint64_t>(a2));
            newv.it_interval.tv_nsec = static_cast<long>(mem_.load<uint64_t>(a2 + 8));
            newv.it_value.tv_sec     = static_cast<time_t>(mem_.load<uint64_t>(a2 + 16));
            newv.it_value.tv_nsec    = static_cast<long>(mem_.load<uint64_t>(a2 + 24));
            int r = ::timerfd_settime(hfd, static_cast<int>(a1), &newv, a3 ? &oldv : nullptr);
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
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct itimerspec cur;
            int r = ::timerfd_gettime(hfd, &cur);
            if (r == 0 && a1) {
                mem_.store<uint64_t>(a1, static_cast<uint64_t>(cur.it_interval.tv_sec));
                mem_.store<uint64_t>(a1 + 8, static_cast<uint64_t>(cur.it_interval.tv_nsec));
                mem_.store<uint64_t>(a1 + 16, static_cast<uint64_t>(cur.it_value.tv_sec));
                mem_.store<uint64_t>(a1 + 24, static_cast<uint64_t>(cur.it_value.tv_nsec));
            }
            ret_host(r);
            return 0;
        }
        case 198: { // socket(domain, type, protocol) — aarch64 198
            // Forward to host. The host kernel creates a real socket fd
            // which we wrap in HostNode + FdTable.allocate so that:
            //   - close(guest_fd) actually closes the host fd
            //   - getsockopt/setsockopt/etc. can resolve the host fd
            //   - read()/write() on the socket work via fs.cpp's path
            int hfd = ::socket(static_cast<int>(a0), static_cast<int>(a1),
                               static_cast<int>(a2));
            if (hfd < 0) { ret_errno(); return 0; }
            int gfd = register_host_fd(emu, hfd, O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d socket dom=%llu type=%llu hfd=%d gfd=%d\n",
                        cpu.tid, (unsigned long long)a0, (unsigned long long)a1, hfd, gfd);
            ret_host(gfd);
            return 0;
        }
        case 199: { // socketpair(domain, type, protocol, sv) — aarch64 199
            int fds[2];
            int r = ::socketpair(static_cast<int>(a0), static_cast<int>(a1),
                                 static_cast<int>(a2), fds);
            if (r < 0) { ret_errno(); return 0; }
            // Register both ends in the FdTable.
            int g0 = register_host_fd(emu, fds[0], O_RDWR);
            int g1 = register_host_fd(emu, fds[1], O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d socketpair dom=%llu type=%llu hfd0=%d gfd0=%d hfd1=%d gfd1=%d\n",
                        cpu.tid, (unsigned long long)a0, (unsigned long long)a1, fds[0], g0, fds[1], g1);
            mem_.store<int>(a3, g0);
            mem_.store<int>(a3 + 4, g1);
            ret_host(0);
            return 0;
        }
        case 200: { // bind(sockfd, addr, addrlen) — aarch64 200
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct sockaddr_storage ss;
            socklen_t len = marshal_sockaddr_in(mem_, a1, a2, &ss);
            if (len == 0) { ret_err(EINVAL); return 0; }
            int r = ::bind(hfd, reinterpret_cast<struct sockaddr*>(&ss), len);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 201: { // listen(sockfd, backlog) — aarch64 201
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            int r = ::listen(hfd, static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 202: { // accept(sockfd, addr, addrlen) — aarch64 202
            // which discarded the peer address. Real accept(2) fills in the
            // peer address (when addr != NULL) and writes the actual length
            // back to *addrlen. Callers that pass NULL get NULL behavior.
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int new_hfd = ::accept(hfd, reinterpret_cast<struct sockaddr*>(&ss),
                                   &sslen);
            if (new_hfd < 0) { ret_errno(); return 0; }
            // Write peer address back to guest memory if requested.
            if (a1 && a2) {
                marshal_sockaddr_out(mem_, a1, a2, &ss, sslen);
            }
            int gfd = register_host_fd(emu, new_hfd, O_RDWR);
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d accept hfd=%d gfd=%d\n", cpu.tid, new_hfd, gfd);
            ret_host(gfd);
            return 0;
        }
        case 203: { // connect(sockfd, addr, addrlen) — aarch64 203
            int hfd = resolve_sock_fd(emu, a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct sockaddr_storage ss;
            socklen_t len = marshal_sockaddr_in(mem_, a1, a2, &ss);
            if (len == 0) { ret_err(EINVAL); return 0; }
            if (dbg().xtrace) {
                fprintf(stderr, "[XCONN] gfd=%llu hfd=%d fam=%d ", (unsigned long long)a0, hfd, ss.ss_family);
                if (ss.ss_family == AF_UNIX) {
                    const char* p = reinterpret_cast<const struct sockaddr_un*>(&ss)->sun_path;
                    fprintf(stderr, "path='%.120s'", p);
                }
                fprintf(stderr, "\n");
            }
            int r = ::connect(hfd, reinterpret_cast<struct sockaddr*>(&ss), len);
            if (dbg().xtrace) {
                fprintf(stderr, "[XCONN] gfd=%llu hfd=%d res=%d ", (unsigned long long)a0, hfd, r);
                if (r == 0) {
                    struct sockaddr_storage peer;
                    socklen_t plen = sizeof(peer);
                    if (::getpeername(hfd, reinterpret_cast<struct sockaddr*>(&peer), &plen) == 0) {
                        if (peer.ss_family == AF_UNIX) {
                            const struct sockaddr_un* un = reinterpret_cast<const struct sockaddr_un*>(&peer);
                            fprintf(stderr, "peer='" );
                            for (unsigned k = 0; k < 32; ++k) fprintf(stderr, "%02x", (unsigned char)un->sun_path[k]);
                            fprintf(stderr, "'\n");
                        } else fprintf(stderr, "peer_fam=%d\n", peer.ss_family);
                    } else fprintf(stderr, "getpeername-err\n");
                } else fprintf(stderr, "\n");
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 279: { // memfd_create(name, flags) — AArch64 279
            // Forward to host memfd_create. Used by glibc tmpfile(),
            // Rust memmap, Wayland, Chrome IPC.
            // Read the name string from guest memory.
            std::string name;
            if (a0 != 0) {
                try {
                    for (size_t i = 0; i < 256; i++) {
                        uint8_t c = mem_.load<uint8_t>(a0 + i);
                        if (c == 0) break;
                        name.push_back(static_cast<char>(c));
                    }
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            int hfd = static_cast<int>(::syscall(SYS_memfd_create, name.c_str(),
                                static_cast<unsigned int>(a1)));
            if (hfd < 0) { ret_errno(); return 0; }
            int gfd = register_host_fd(emu, hfd, O_RDWR);
            ret_host(gfd);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
}
} // namespace arm64emu
