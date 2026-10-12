// EventIO.cpp — event and readiness descriptors:
//   - eventfd2 (19), epoll_create1/epoll_ctl/epoll_pwait (20-22)
//   - pselect6/ppoll (72, 73)
//   - timerfd_create/settime/gettime (85-87)
//   - inotify_init1/add_watch/rm_watch (26-28), signalfd4 (74)
//   - memfd_create (279)
//
// Host fds are wrapped in HostNode and registered in the FdTable, so
// close() releases them and guest/host descriptor numbers stay
// independent. epoll_pwait honors the guest sigmask.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/VFS/Nodes/AudioNode.hpp"
#include <chrono>
#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <vector>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
namespace arm64emu {
int64_t syscall_event_io(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& fds_ = emu.fds();
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
            // AArch64 aligns data to 8 bytes (size 16); only x86 packs it.
            int epfd = resolve_sock_fd(emu, a0);
            int fd   = resolve_sock_fd(emu, a2);
            struct epoll_event ev{};
            // Linux ignores the event pointer for DEL, including NULL and
            // inaccessible addresses. Android Looper uses NULL on disposal.
            if (a1 != EPOLL_CTL_DEL) {
                ev.events = mem_.load<uint32_t>(a3);
                ev.data.u64 = mem_.load<uint64_t>(a3 + 8);
            }
            int r = ::epoll_ctl(epfd, static_cast<int>(a1), fd,
                                a1 == EPOLL_CTL_DEL ? nullptr : &ev);
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
                    uint64_t p = a1 + static_cast<uint64_t>(i) * 16;
                    try {
                        mem_.store<uint32_t>(p, evs[i].events);
                        mem_.store<uint64_t>(p + 8, evs[i].data.u64);
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
            std::vector<std::shared_ptr<yggdrasil::AudioNode>> audio_nodes(nfds);
            bool virtual_audio = false;
            for (int i = 0; i < nfds; i++) {
                const int guest_fd = mem_.load<int>(a0 + static_cast<uint64_t>(i) * 8);
                audio_nodes[i] = std::dynamic_pointer_cast<yggdrasil::AudioNode>(emu.fds().get(guest_fd));
                virtual_audio |= static_cast<bool>(audio_nodes[i]);
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
            const auto poll_start = std::chrono::steady_clock::now();
            while (true) {
                int wait_ms = timeout_ms;
                if (virtual_audio) {
                    bool ready = false;
                    for (int i = 0; i < nfds; ++i)
                        if (audio_nodes[i] && audio_nodes[i]->poll_events(pfds[i].events)) ready = true;
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - poll_start).count();
                    wait_ms = ready ? 0 : timeout_ms < 0 ? 5 :
                              static_cast<int>(std::min<int64_t>(5, std::max<int64_t>(0, timeout_ms - elapsed)));
                }
                r = ::poll(pfds.data(), nfds, wait_ms);
                if (r >= 0 && virtual_audio) {
                    r = 0;
                    for (int i = 0; i < nfds; ++i) {
                        if (audio_nodes[i]) pfds[i].revents |= audio_nodes[i]->poll_events(pfds[i].events);
                        if (pfds[i].revents) ++r;
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - poll_start).count();
                    if (!r && (timeout_ms < 0 || elapsed < timeout_ms)) continue;
                }
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
        // ── inotify_init1 (syscall 26) ───────────────────────────────
        // BUGFIX: AArch64 syscall numbers 75/76/77 are vmsplice/splice/tee,
        // NOT inotify. The real inotify numbers per asm-generic/unistd.h are:
        //   26 = inotify_init1
        //   27 = inotify_add_watch
        //   28 = inotify_rm_watch
        // inotify handlers (which would call host inotify with garbage
        // args and fail). Real guest inotify_init1 (syscall 26) returned
        // -ENOSYS. Fixed by renumbering to the correct AArch64 slots.
        case 26: { // inotify_init1(flags)
            int fd = ::inotify_init1(static_cast<int>(a0));
            if (fd < 0) { ret_errno(); return 0; }
            // Guest descriptors are allocated independently from host fds.
            // Wrap this fd so later inotify calls resolve the guest number
            // and close() releases the host descriptor through FdTable.
            int guest_fd = fds_.allocate(std::make_shared<yggdrasil::HostNode>(
                fd, O_RDONLY | (static_cast<int>(a0) & O_NONBLOCK)));
            ret_host(static_cast<uint64_t>(guest_fd));
            return 0;
        }
        // ── inotify_add_watch (syscall 27) ───────────────────────────
        case 27: { // inotify_add_watch(fd, pathname, mask)
            auto node = fds_.get(static_cast<int>(a0));
            int host_fd = node ? node->host_fd() : -1;
            if (host_fd < 0) { ret_err(EBADF); return 0; }
            std::string path = Yggdrasil::read_path(mem_, a1);
            int wd = ::inotify_add_watch(host_fd, path.c_str(),
                                         static_cast<uint32_t>(a2));
            if (wd < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(wd));
            return 0;
        }
        // ── inotify_rm_watch (syscall 28) ────────────────────────────
        case 28: { // inotify_rm_watch(fd, wd)
            auto node = fds_.get(static_cast<int>(a0));
            int host_fd = node ? node->host_fd() : -1;
            if (host_fd < 0) { ret_err(EBADF); return 0; }
            int r = ::inotify_rm_watch(host_fd, static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
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
    return 0;
}
} // namespace arm64emu
