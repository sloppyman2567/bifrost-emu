// syscalls/misc.cpp — catch-all syscall handler for syscalls that don't
// fit into fs/mem/threads/time/ioctls. Handles the remaining "miscellaneous"
// syscalls after signal, I/O, and process sub-handlers have run:
//   - exit / exit_group (case 93, 94)
//   - inotify_init1 / inotify_add_watch / inotify_rm_watch (case 26, 27, 28)
//   - vmsplice / splice / tee (case 75, 76, 77 — host passthrough)
//   - accept4 (case 242) and the socket ops (case 204-212)
//   - fadvise64 (case 223)
//   - close_range / openat2 / faccessat2 (case 436, 437, 439)
//   - signalfd4 (case 74)
//   - symlinkat / nfsservctl / chroot / fchownat / fchown (case 36, 42, 51, 54, 55)
//   - pwrite64 / preadv2 / sendfile (case 68, 69, 71)
//   - sync / fsync / fdatasync / sync_file_range (case 81, 82, 83, 84)
//   - waitid (case 95)
//   - unshare (case 97)
//   - setresgid (case 149)
//   - flock (case 32)
//   - GraphicThunk trampoline (case 0x1000)
//
// NOTE: I/O syscalls (eventfd2, epoll_*, pselect6, ppoll, timerfd_*, socket,
// socketpair, bind, listen, accept, connect — cases 19, 20, 21, 22, 72, 73,
// 85, 86, 87, 198-203) have been moved to misc_io.cpp.
//
// NOTE: signal syscalls (sigaltstack, rt_sigaction, rt_sigprocmask,
// rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo, rt_sigreturn,
// rt_sigsuspend — cases 132-139) have been moved to misc_signal.cpp.
//
// NOTE: process/identity/resource syscalls (ptrace, sched_*, setpriority,
// getpriority, times, getgroups, setgroups, uname, getrlimit, getrusage,
// umask, prctl, getcpu, getpid/getppid/getuid/..., sysinfo, readahead,
// add_key/request_key/keyctl, swapon, mincore, waitpid, wait4, prlimit64,
// process_vm_readv, kcmp, getrandom, execveat, rseq — cases 117, 122-126,
// 140, 141, 153, 155, 158-160, 163, 165-168, 172-179, 213, 217-219, 224,
// 232, 247, 260, 261, 270, 272, 278, 281, 293) have been moved to
// misc_process.cpp.
//
// All case bodies are extracted verbatim from the original syscalls.cpp.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include <mutex>
#include "core/signal.h"
#include "debug_flags.h"
#include "frontend/dynamic_linker.h"  // DynamicLinker (for _dl_allocate_tls syscall)
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "frost/audio_thunk.hpp"    //
#include "frost/display_thunk.hpp"  //
#include "syscalls/syscalls.h"
#include "yggdrasil/host_node.hpp"  // HostNode (for socket fd registration)
#include <errno.h>
#include <fcntl.h>
#include <algorithm>
#include <poll.h>
#include <signal.h>
#include <syscall.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
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
static void xseq_append(const char* path, char dir, const uint8_t* p, size_t n) {
    if (!path) return;
    FILE* f = fopen(path, "ab");
    if (!f) return;
    fwrite(&dir, 1, 1, f);
    uint32_t l = static_cast<uint32_t>(n);
    fwrite(&l, 1, 4, f);
    if (n) fwrite(p, 1, n, f);
    fclose(f);
}
int64_t syscall_misc(Emulator& emu, CPU& cpu, uint64_t num) {
    if (syscall_misc_signal(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_misc_io(emu, cpu, num)     != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_misc_process(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    // v1.5.0.alpha: extended syscalls (xattr, kcmp, membarrier,
    // copy_file_range, pkey_*, pidfd_*, io_uring stubs, capget/capset,
    // personality, mseal, etc.).
    if (syscall_misc_extended(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& fds_ = emu.fds_;
    switch (num) {
        // ── Process / identity / resource syscalls (117, 122-126, 140, 141,
        //    153, 155, 158-160, 163, 165-168, 172-179, 213, 217-219, 224,
        //    232, 247, 260, 261, 270, 272, 278, 281, 293) have been moved
        //    to misc_process.cpp. syscall_misc() dispatches to
        //    syscall_misc_process() at the top of this function.
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
        // These are real AArch64 syscalls that move data through pipes.
        // We implement them as host passthroughs, resolving guest fds via
        // FdTable and marshaling iovec arrays from guest memory.
        case 75: { // vmsplice(fd, iov, nr_segs, flags)
            // Resolve guest fd to host fd via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            // Marshal iovec array from guest memory.
            // Guest iovec: { void* iov_base; size_t iov_len; } — 16 bytes each.
            uint64_t iov_g = a1;
            uint64_t nr_segs = a2;
            if (nr_segs > 1024) nr_segs = 1024;  // sanity cap
            std::vector<struct iovec> iovs(nr_segs);
            std::vector<std::vector<uint8_t>> iov_bufs(nr_segs);
            for (uint64_t i = 0; i < nr_segs; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_g + i * 16);
                uint64_t len = mem_.load<uint64_t>(iov_g + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                iov_bufs[i].resize(len);
                if (len) mem_.read(base, iov_bufs[i].data(), len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            ssize_t r = ::vmsplice(hfd, iovs.data(), nr_segs,
                                   static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 76: { // splice(fd_in, off_in, fd_out, off_out, len, flags)
            // Resolve guest fds to host fds via FdTable.
            auto node_in = fds_.get(static_cast<int>(a0));
            int hfd_in = node_in ? node_in->host_fd() : static_cast<int>(a0);
            if (hfd_in < 0) { ret_err(EBADF); return 0; }
            auto node_out = fds_.get(static_cast<int>(a2));
            int hfd_out = node_out ? node_out->host_fd() : static_cast<int>(a2);
            if (hfd_out < 0) { ret_err(EBADF); return 0; }
            // off_in/off_out are pointers to loff_t (64-bit) in guest memory.
            // They can be NULL (for pipes). Read them into host loff_t.
            loff_t off_in = 0, off_out = 0;
            loff_t* poff_in = nullptr;
            loff_t* poff_out = nullptr;
            if (a1) {
                off_in = static_cast<loff_t>(mem_.load<uint64_t>(a1));
                poff_in = &off_in;
            }
            if (a3) {
                off_out = static_cast<loff_t>(mem_.load<uint64_t>(a3));
                poff_out = &off_out;
            }
            ssize_t r = ::splice(hfd_in, poff_in, hfd_out, poff_out,
                                 static_cast<size_t>(a4),
                                 static_cast<int>(a5));
            if (r < 0) { ret_errno(); return 0; }
            // Write back offsets if the guest provided pointers.
            if (a1) mem_.store<uint64_t>(a1, static_cast<uint64_t>(off_in));
            if (a3) mem_.store<uint64_t>(a3, static_cast<uint64_t>(off_out));
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 77: { // tee(fd_in, fd_out, len, flags)
            // Both fds must be pipes. Resolve via FdTable.
            auto node_in = fds_.get(static_cast<int>(a0));
            int hfd_in = node_in ? node_in->host_fd() : static_cast<int>(a0);
            if (hfd_in < 0) { ret_err(EBADF); return 0; }
            auto node_out = fds_.get(static_cast<int>(a1));
            int hfd_out = node_out ? node_out->host_fd() : static_cast<int>(a1);
            if (hfd_out < 0) { ret_err(EBADF); return 0; }
            ssize_t r = ::tee(hfd_in, hfd_out,
                              static_cast<size_t>(a2),
                              static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        // ── accept4 (syscall 242) ────────────────────────────────────
        // NOTE: AArch64 syscall 88 is utimensat (handled in fs.cpp), NOT
        // accept4. Real accept4 is syscall 242. The old code at case 88
        // was dead — fs.cpp's utimensat handler always won the dispatch
        // order, so this case never ran. Moved to the correct number.
        case 242: { // accept4(sockfd, addr, addrlen, flags) — AArch64 242
            // Resolve guest fd to host fd via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            // Marshal sockaddr from host to guest memory.
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int new_hfd = ::accept4(hfd,
                               reinterpret_cast<struct sockaddr*>(&ss), &sslen,
                               static_cast<int>(a3));
            if (new_hfd < 0) { ret_errno(); return 0; }
            if (a1 && a2) {
                // Read guest addrlen, clamp to our result.
                try {
                    socklen_t guest_len = static_cast<socklen_t>(mem_.load<uint32_t>(a2));
                    if (guest_len > sslen) guest_len = sslen;
                    mem_.write(a1, &ss, guest_len);
                    mem_.store<uint32_t>(a2, guest_len);
                } catch (...) {
                    // Bad addr/addrlen pointer — close fd, return EFAULT.
                    ::close(new_hfd);
                    ret_err(EFAULT);
                    return 0;
                }
            }
            // Register the new socket fd in the FdTable.
            int gfd = fds_.allocate(std::make_shared<yggdrasil::HostNode>(new_hfd, O_RDWR));
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d accept4 hfd=%d gfd=%d\n", cpu.tid, new_hfd, gfd);
            ret_host(static_cast<uint64_t>(gfd));
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
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getsockname(hfd,
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
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int r = ::getpeername(hfd,
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
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
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
            ssize_t r = ::sendto(hfd, buf.data(), len,
                                 static_cast<int>(a3), dest_ptr,
                                 static_cast<socklen_t>(a5));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        // ── recvfrom (syscall 207) ───────────────────────────────────
        case 207: { // recvfrom(sockfd, buf, len, flags, src_addr, addrlen)
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            size_t len = a2;
            if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
            std::vector<uint8_t> buf(len);
            struct sockaddr_storage src_ss;
            socklen_t srclen = sizeof(src_ss);
            ssize_t r = ::recvfrom(hfd, buf.data(), len,
                                   static_cast<int>(a3),
                                   reinterpret_cast<struct sockaddr*>(&src_ss),
                                   &srclen);
            if (r < 0) { ret_errno(); return 0; }
            if (static_cast<int>(a0) == 3) {
                xseq_append(dbg().xseqlog.c_str(), 'R', buf.data(), static_cast<size_t>(r));
                if (dbg().xtrace) {
                    fprintf(stderr, "[XRECVF] fd=%d hfd=%d ret=%zd first=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                            static_cast<int>(a0), hfd, r, buf[0], buf[1], buf[2], buf[3],
                            buf.size()>4?buf[4]:0, buf.size()>5?buf[5]:0, buf.size()>6?buf[6]:0, buf.size()>7?buf[7]:0);
                }
            }
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
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            if (a3 == 0 || a4 == 0) { ret_err(EFAULT); return 0; }
            size_t optlen = a4;
            if (optlen > 4096) optlen = 4096;  // sanity cap
            std::vector<uint8_t> optval(optlen);
            mem_.read(a3, optval.data(), optlen);
            int r = ::setsockopt(hfd, static_cast<int>(a1),
                                 static_cast<int>(a2), optval.data(),
                                 static_cast<socklen_t>(optlen));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        // ── getsockopt (syscall 209) ─────────────────────────────────
        case 209: { // getsockopt(sockfd, level, optname, optval, optlen*)
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            if (a4 == 0) { ret_err(EFAULT); return 0; }
            socklen_t host_optlen = static_cast<socklen_t>(mem_.load<uint32_t>(a4));
            if (host_optlen > 4096) host_optlen = 4096;
            std::vector<uint8_t> optval(host_optlen);
            int r = ::getsockopt(hfd, static_cast<int>(a1),
                                 static_cast<int>(a2), optval.data(), &host_optlen);
            if (r < 0) { ret_errno(); return 0; }
            if (a3) mem_.write(a3, optval.data(), host_optlen);
            mem_.store<uint32_t>(a4, host_optlen);
            ret_host(0);
            return 0;
        }
        // ── shutdown (syscall 210) ───────────────────────────────────
        case 210: { // shutdown(sockfd, how)
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            int r = ::shutdown(hfd, static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        // ── sendmsg (syscall 211) ────────────────────────────────────
        case 211: { // sendmsg(sockfd, msg, flags)
            // Resolve guest fd to host fd via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
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
            uint64_t msg_control = mem_.load<uint64_t>(a1 + 32);
            uint32_t msg_controllen = mem_.load<uint32_t>(a1 + 40);
            // Marshal iovec array: each entry is (void* base, size_t len).
            if (msg_iovlen > 1024) msg_iovlen = 1024;  // sanity cap
            std::vector<iovec> iovs(msg_iovlen);
            std::vector<std::vector<uint8_t>> iov_bufs(msg_iovlen);
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t base = mem_.load<uint64_t>(msg_iov + i * 16);
                uint64_t len = mem_.load<uint64_t>(msg_iov + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                iov_bufs[i].resize(len);
                if (len) mem_.read(base, iov_bufs[i].data(), len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            // Marshal msg_name.
            std::vector<uint8_t> name_buf;
            if (msg_name && msg_namelen) {
                name_buf.resize(msg_namelen);
                mem_.read(msg_name, name_buf.data(), msg_namelen);
            }
            // Marshal msg_control (cmsg buffer).
            // The control buffer is opaque to the host kernel; we just
            // copy it verbatim. The guest and host share the same
            // cmsghdr layout (both are Linux AArch64).
            std::vector<uint8_t> ctrl_buf;
            if (msg_control && msg_controllen) {
                if (msg_controllen > 4096) msg_controllen = 4096;
                ctrl_buf.resize(msg_controllen);
                mem_.read(msg_control, ctrl_buf.data(), msg_controllen);
            }
            struct msghdr host_msg;
            memset(&host_msg, 0, sizeof(host_msg));
            host_msg.msg_name = name_buf.empty() ? nullptr : name_buf.data();
            host_msg.msg_namelen = msg_namelen;
            host_msg.msg_iov = iovs.data();
            host_msg.msg_iovlen = msg_iovlen;
            host_msg.msg_control = ctrl_buf.empty() ? nullptr : ctrl_buf.data();
            host_msg.msg_controllen = msg_controllen;
            ssize_t r = ::sendmsg(hfd, &host_msg, static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            if (static_cast<int>(a0) == 3 && r > 0) {
                const std::string& sp = dbg().xseqlog;
                if (!sp.empty()) {
                    ssize_t rem = r;
                    for (uint64_t i = 0; i < msg_iovlen && rem > 0; i++) {
                        uint64_t n = std::min<uint64_t>(iov_bufs[i].size(), static_cast<uint64_t>(rem));
                        if (n > 0) xseq_append(sp.c_str(), 'W', iov_bufs[i].data(), n);
                        rem -= n;
                    }
                }
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        // ── recvmsg (syscall 212) ────────────────────────────────────
        // BUGFIX: was at case 211 (wrong — 211 is sendmsg). Moved to 212
        // which is the correct AArch64 syscall number per asm-generic/unistd.h.
        case 212: { // recvmsg(sockfd, msg, flags)
            // Resolve guest fd to host fd via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            if (!a1) { ret_err(EFAULT); return 0; }
            uint64_t msg_name = mem_.load<uint64_t>(a1);
            uint32_t msg_namelen = mem_.load<uint32_t>(a1 + 8);
            uint64_t msg_iov = mem_.load<uint64_t>(a1 + 16);
            uint64_t msg_iovlen = mem_.load<uint64_t>(a1 + 24);
            uint64_t msg_control = mem_.load<uint64_t>(a1 + 32);
            uint32_t msg_controllen = mem_.load<uint32_t>(a1 + 40);
            if (msg_iovlen > 1024) msg_iovlen = 1024;
            std::vector<iovec> iovs(msg_iovlen);
            std::vector<std::vector<uint8_t>> iov_bufs(msg_iovlen);
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t len = mem_.load<uint64_t>(msg_iov + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                iov_bufs[i].resize(len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            std::vector<uint8_t> name_buf;
            if (msg_name && msg_namelen) name_buf.resize(msg_namelen);
            // Allocate a control buffer for the host to write cmsgs into.
            std::vector<uint8_t> ctrl_buf;
            if (msg_control && msg_controllen) {
                if (msg_controllen > 4096) msg_controllen = 4096;
                ctrl_buf.resize(msg_controllen);
            }
            struct msghdr host_msg;
            memset(&host_msg, 0, sizeof(host_msg));
            host_msg.msg_name = name_buf.empty() ? nullptr : name_buf.data();
            host_msg.msg_namelen = msg_namelen;
            host_msg.msg_iov = iovs.data();
            host_msg.msg_iovlen = msg_iovlen;
            host_msg.msg_control = ctrl_buf.empty() ? nullptr : ctrl_buf.data();
            host_msg.msg_controllen = msg_controllen;
            if (static_cast<int>(a0) == 3 && dbg().xtrace) {
                uint8_t peekbuf[64] = {0};
                ssize_t pn = ::recv(hfd, peekbuf, sizeof(peekbuf), MSG_PEEK | MSG_DONTWAIT);
                struct sockaddr_storage peer;
                socklen_t plen = sizeof(peer);
                const char* ps = "?";
                char phex[128] = {0};
                if (::getpeername(hfd, reinterpret_cast<struct sockaddr*>(&peer), &plen) == 0 && peer.ss_family == AF_UNIX) {
                    const unsigned char* pp = reinterpret_cast<const unsigned char*>(reinterpret_cast<const struct sockaddr_un*>(&peer)->sun_path);
                    snprintf(phex, sizeof(phex), "%02x%02x%02x%02x%02x%02x%02x%02x", pp[0],pp[1],pp[2],pp[3],pp[4],pp[5],pp[6],pp[7]);
                    ps = phex;
                }
                fprintf(stderr, "[XPRE] t%d gfd=3 hfd=%d peer=%s pre_peek=%zd pre=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                        cpu.tid, hfd, ps, pn, peekbuf[0], peekbuf[1], peekbuf[2], peekbuf[3],
                        peekbuf[4], peekbuf[5], peekbuf[6], peekbuf[7]);
            }
            {
                if (dbg().xdelay && static_cast<int>(a0) == 3) {
                    for (int k = 0; k < 8; k++) {
                        uint8_t pb[64] = {0};
                        ssize_t pn = ::recv(hfd, pb, sizeof(pb), MSG_PEEK | MSG_DONTWAIT);
                        fprintf(stderr, "[XDELAY hfd=%d t=%d] peek=%zd %02x%02x%02x%02x %02x%02x%02x%02x\n",
                                hfd, k * 25, pn, pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6], pb[7]);
                        usleep(25000);
                    }
                }
            }
            ssize_t r = ::recvmsg(hfd, &host_msg, static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            if (static_cast<int>(a0) == 3 && dbg().xtrace && r > 0) {
                // peek what is actually sitting in the kernel socket queue
                uint8_t peekbuf[128] = {0};
                ssize_t pn = ::recv(hfd, peekbuf, sizeof(peekbuf), MSG_PEEK | MSG_DONTWAIT);
                fprintf(stderr, "[XHOST] t%d gfd=3 hfd=%d ret=%zd peek=%zd peek_first=%02x%02x%02x%02x first=%02x%02x%02x%02x\n",
                        cpu.tid, hfd, r, pn, peekbuf[0], peekbuf[1], peekbuf[2], peekbuf[3],
                        iov_bufs.empty()?0:iov_bufs[0].data()[0],
                        iov_bufs.empty()||iov_bufs[0].size()<2?0:iov_bufs[0].data()[1],
                        iov_bufs.empty()||iov_bufs[0].size()<3?0:iov_bufs[0].data()[2],
                        iov_bufs.empty()||iov_bufs[0].size()<4?0:iov_bufs[0].data()[3]);
            }
            if (static_cast<int>(a0) == 3 && r > 0) {
                const std::string& sp = dbg().xseqlog;
                if (!sp.empty()) {
                    ssize_t rem = r;
                    for (uint64_t i = 0; i < msg_iovlen && rem > 0; i++) {
                        uint64_t n = std::min<uint64_t>(iov_bufs[i].size(), static_cast<uint64_t>(rem));
                        if (n > 0) xseq_append(sp.c_str(), 'R', iov_bufs[i].data(), n);
                        rem -= n;
                    }
                }
            }
            if (dbg().xrecv && !iov_bufs.empty() && iov_bufs[0].size() >= 1) {
                uint8_t* p = iov_bufs[0].data();
                fprintf(stderr, "[XRECV] fd=%d ret=%zd first=%02x %02x %02x %02x | %02x %02x %02x %02x\n",
                        static_cast<int>(a0), r, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
            if (!dbg().xcap.empty() && static_cast<int>(a0) == 3 && r > 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s.recv", dbg().xcap.c_str());
                FILE* f = fopen(path, "ab");
                if (f) {
                    uint32_t blen = static_cast<uint32_t>(r);
                    fwrite(&blen, 1, 4, f);
                    ssize_t rem = r;
                    for (uint64_t i = 0; i < msg_iovlen && rem > 0; i++) {
                        uint64_t n = std::min<uint64_t>(iov_bufs[i].size(), static_cast<uint64_t>(rem));
                        if (n > 0) fwrite(iov_bufs[i].data(), 1, n, f);
                        rem -= n;
                    }
                    fclose(f);
                }
            }
            // Write received data back to guest iovec buffers.
            for (uint64_t i = 0; i < msg_iovlen; i++) {
                uint64_t base = mem_.load<uint64_t>(msg_iov + i * 16);
                if (iov_bufs[i].size() > 0) {
                    mem_.write(base, iov_bufs[i].data(), iov_bufs[i].size());
                }
            }
            // Write source address back.
            if (msg_name && msg_namelen) {
                socklen_t actual = static_cast<socklen_t>(host_msg.msg_namelen);
                if (actual > msg_namelen) actual = msg_namelen;
                if (actual > 0) mem_.write(msg_name, name_buf.data(), actual);
                mem_.store<uint32_t>(a1 + 8, actual);
            }
            // Write control buffer back.
            // host_msg.msg_controllen may have been modified by the host
            // to reflect the actual size of the cmsgs written.
            if (msg_control && msg_controllen) {
                socklen_t actual_ctrl = static_cast<socklen_t>(host_msg.msg_controllen);
                if (actual_ctrl > msg_controllen) actual_ctrl = msg_controllen;
                if (actual_ctrl > 0) {
                    mem_.write(msg_control, ctrl_buf.data(), actual_ctrl);
                }
                mem_.store<uint32_t>(a1 + 40, actual_ctrl);
            }
            // Write msg_flags back.
            mem_.store<uint32_t>(a1 + 44, static_cast<uint32_t>(host_msg.msg_flags));
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
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d openat2-ish path=%.60s\n", cpu.tid, path.c_str());
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
            // written at offset 4 (where si_errno belongs) and si_pid at
            // offset 8 (where si_code belongs). The correct AArch64
            // siginfo_t layout is:
            //   offset  0: si_signo  (4 bytes)
            //   offset  4: si_errno  (4 bytes)
            //   offset  8: si_code   (4 bytes)
            //   offset 12: __pad     (4 bytes)
            //   offset 16: _sigchld.si_pid    (4 bytes)
            //   offset 20: _sigchld.si_uid    (4 bytes)
            //   offset 24: _sigchld.si_status (4 bytes)
            //   offset 28: __pad              (4 bytes)
            //   offset 32: _sigchld.si_utime  (8 bytes)
            //   offset 40: _sigchld.si_stime  (8 bytes)
            // This matches build_siginfo() in src/core/signal.cpp and the
            // Linux kernel's struct siginfo for AArch64.
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
                    mem_.store<uint32_t>(a2 + 0,  static_cast<uint32_t>(si.si_signo));
                    mem_.store<uint32_t>(a2 + 4,  0);  // si_errno (always 0)
                    mem_.store<uint32_t>(a2 + 8,  static_cast<uint32_t>(si.si_code));
                    mem_.store<uint32_t>(a2 + 12, 0);  // __pad
                    mem_.store<uint32_t>(a2 + 16, static_cast<uint32_t>(si.si_pid));
                    mem_.store<uint32_t>(a2 + 20, static_cast<uint32_t>(si.si_uid));
                    mem_.store<uint32_t>(a2 + 24, static_cast<uint32_t>(si.si_status));
                    mem_.store<uint32_t>(a2 + 28, 0);  // __pad
                    // si_utime / si_stime (offset 32/40) — only valid when
                    // si_code == CLD_TRAPPED with WUNTRACED; leave as 0.
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
        // ── Bifrost-emu internal thunk syscall ────────────
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
            // v1.5.0.alpha: the thunk syscall is shared by GraphicThunk,
            // AudioThunk, and DisplayThunk. Each has its own per-thunk
            // symbol_id namespace starting from 0, so we try each in
            // order until one accepts the symbol_id.
            //
            // We try GraphicThunk first because most guests use it; if
            // it returns -ENOENT (symbol_id out of range), we fall
            // through to AudioThunk, then DisplayThunk.
            uint32_t sym_id = static_cast<uint32_t>(cpu.regs[9]);
            auto* gthunk = emu.graphics_.thunk();
            if (gthunk && gthunk->enabled()) {
                int64_t r = gthunk->dispatch(cpu, sym_id);
                if (r == 0) return 0;            // handled
                if (r != -ENOENT) {               // real error from GraphicThunk
                    ret_host(r);
                    return 0;
                }
            }
            // Try AudioThunk.
            auto* athunk = emu.graphics_.audio_thunk();
            if (athunk && athunk->enabled()) {
                int64_t r = athunk->dispatch(cpu, sym_id);
                if (r == 0) return 0;
                if (r != -ENOENT) {
                    ret_host(r);
                    return 0;
                }
            }
            // Try DisplayThunk.
            auto* dthunk = emu.graphics_.display_thunk();
            if (dthunk && dthunk->enabled()) {
                int64_t r = dthunk->dispatch(cpu, sym_id);
                if (r == 0) return 0;
                if (r != -ENOENT) {
                    ret_host(r);
                    return 0;
                }
            }
            // None of the thunks recognized the symbol_id.
            ret_err(ENOSYS);
            return 0;
        }
        // ── Bifrost-emu internal TLS-alloc syscall  ────────
        // Called by _dl_allocate_tls AND _dl_allocate_tls_init stubs.
        //
        //   1. Copies lib TLS template to [tcb-lib_size, tcb) (negative TP offsets)
        //   2. ZEROS [tcb, tcb+main_memsz) to clear stale .tbss data
        //      (this is the main exe TLS area at positive TP offsets)
        //   3. Does NOT copy TCB header fields — glibc's create_thread
        //      sets tcb/self/dtv/stack_guard/pointer_guard AFTER this
        //      function returns. The previous version copied a 128-byte
        //      TCB header from the main thread, which included the DTV
        //      pointer — when the new thread exited, glibc tried to free
        //      the main thread's DTV, causing "double free or corruption".
        //
        // This is SAFE because _dl_allocate_tls/init run BEFORE
        // create_thread. The sequence in glibc's allocate_stack is:
        //   1. Zero struct pthread (new stacks only)
        //   2. Call _dl_allocate_tls OR _dl_allocate_tls_init ← WE FIRE HERE
        //   3. Set pd->start_routine, pd->arg, etc.
        //   4. Call create_thread → sets TCB fields, calls clone3
        //
        // For stack-cache reuse (waves 2+), step 1 is skipped. Our zeroing
        // in step 2 replaces it, clearing stale .tbss data. Glibc's
        // create_thread in step 4 then sets TCB fields, overwriting any
        // zeros we wrote to TCB field offsets.
        //
        // a0 (x0) = `mem` from glibc = TCB pointer (= future TPIDR_EL0).
        //   - non-zero: caller-allocated (normal pthread_create path)
        //   - 0: allocate a new block (rare)
        case 0x1001: {
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[tls-alloc] syscall 0x1001 called: "
                        "a0=0x%llx\n", static_cast<unsigned long long>(a0));
            }
            auto* dl = emu.dyn_linker_.get();
            // Hold the loader lock while accounting TLS sizes and copying
            // per-thread TLS templates — a concurrent dlopen() from another
            // guest thread could otherwise reorder objects_ mid-iteration.
            // (When the linker is absent — static binary, no thunk — the
            // fallback mutex is uncontended and effectively free.)
            std::recursive_mutex fallback_mu;
            std::lock_guard<std::recursive_mutex> tls_lk(
                dl ? dl->loader_lock() : fallback_mu);
            // ── Compute lib_size, main TLS info, and tcb_size ─────────
            // Uses variant-I TLS layout (glibc AArch64):
            //   - Main exe TLS: at POSITIVE TP offsets (TP + tcb_size ..)
            //   - Lib TLS: at NEGATIVE TP offsets (TP - lib_size .. TP)
            //   - TCB header (tcbhead_t): at [TP, TP + tcb_size)
            //
            // See allocate_static_tls() in dynamic_linker.cpp for the
            // full layout description.
            uint64_t lib_size = 0;
            uint64_t main_memsz = 0;
            uint64_t main_align = 1;
            if (dl) {
                for (const auto& obj : dl->objects()) {
                    if (!obj.tls.present || obj.tls.memsz == 0) continue;
                    if (obj.is_main) {
                        main_memsz = obj.tls.memsz;
                        main_align = obj.tls.align ? obj.tls.align : 16;
                    } else {
                        uint64_t a = obj.tls.align ? obj.tls.align : 16;
                        lib_size = (lib_size + a - 1) & ~(a - 1);
                        lib_size += obj.tls.memsz;
                    }
                }
                lib_size = (lib_size + 15) & ~15ULL;
            }
            // TLS_TCB_SIZE: matches allocate_static_tls. The TCB header
            // (tcbhead_t) occupies [TP, TP + tcb_size). Round up to main
            // exe's TLS alignment so local-exec TPREL offsets match.
            constexpr uint64_t TLS_TCB_SIZE_BASE = 0x20;
            uint64_t tcb_size = (main_align > 1)
                ? (TLS_TCB_SIZE_BASE + main_align - 1) & ~(main_align - 1)
                : TLS_TCB_SIZE_BASE;
            uint64_t total_tls_size = lib_size + tcb_size + main_memsz;
            uint64_t tcb;
            if (a0 != 0) {
                // Caller-allocated (normal pthread_create path).
                // glibc computes TCB = stack_block + lib_size, so the lib
                // TLS area is [tcb - lib_size, tcb) and main TLS is at
                // [tcb + tcb_size, tcb + tcb_size + main_memsz).
                tcb = a0;
            } else if (total_tls_size == 0) {
                // No TLS — return a minimal zeroed block.
                constexpr uint64_t FALLBACK_SIZE = 4096;
                uint64_t block = mem_.mmap_alloc(FALLBACK_SIZE);
                if (block == 0) { ret_err(ENOMEM); return 0; }
                std::vector<uint8_t> zeros(FALLBACK_SIZE, 0);
                mem_.write(block, zeros.data(), FALLBACK_SIZE);
                tcb = (block + FALLBACK_SIZE - 16) & ~0xFULL;
                ret_host(tcb);
                return 0;
            } else {
                // mem == NULL: allocate a new block.
                // Layout: [block .. block+lib_size) = lib TLS,
                //         [block+lib_size .. block+lib_size+tcb_size) = TCB,
                //         [block+lib_size+tcb_size .. block+total) = main TLS.
                constexpr uint64_t PTHREAD_SLACK = 8192;
                uint64_t alloc_size = total_tls_size + PTHREAD_SLACK;
                alloc_size = (alloc_size + 63) & ~63ULL;
                uint64_t block = mem_.mmap_alloc(alloc_size);
                if (block == 0) { ret_err(ENOMEM); return 0; }
                std::vector<uint8_t> zeros(alloc_size, 0);
                mem_.write(block, zeros.data(), alloc_size);
                tcb = block + lib_size;  // TP points to TCB header start
            }
            // ── Copy each module's TLS template to its per-thread slot ──
            // Variant-I layout per-thread (TP = tcb):
            //   - lib TLS: [tcb - lib_size, tcb)  (negative TP offset)
            //   - TCB header: [tcb, tcb + tcb_size)  (glibc fills this)
            //   - main TLS: [tcb + tcb_size, tcb + tcb_size + main_memsz)
            //                (positive TP offset)
            //
            // Each object's tls_tp_offset is:
            //   - main: +tcb_size (positive)
            //   - libs: (lib_cursor - lib_size) (negative)
            // So dst = tcb + obj.tls_tp_offset gives the correct per-thread
            // address for both main and lib TLS.
            //
            // negative TP offsets). This broke local-exec TLS access for
            // the main exe — the binary's hardcoded positive TPREL offset
            // (e.g. +0x20) landed in the TCB header instead of the main
            // exe's TLS block. With variant-I, main TLS is at TP + tcb_size
            // (positive), matching the linker's TPREL computation.
            if (dl && total_tls_size > 0) {
                for (const auto& obj : dl->objects()) {
                    if (!obj.tls.present || obj.tls.memsz == 0) continue;
                    int64_t tp_off = obj.tls_tp_offset;
                    uint64_t dst = static_cast<uint64_t>(
                        static_cast<int64_t>(tcb) + tp_off);
                    uint64_t src = dl->static_tls_base() + obj.tls_block_offset;
                    uint64_t filesz = obj.tls.filesz;
                    if (filesz > 0 && filesz <= obj.tls.memsz) {
                        try {
                            std::vector<uint8_t> tpl(filesz);
                            mem_.read(src, tpl.data(), filesz);
                            mem_.write(dst, tpl.data(), filesz);
                        } catch (...) {}
                    }
                    // The .bss portion (memsz - filesz) is left as zero
                    // (mmap gives zero pages). For stack-cache reuse,
                    // glibc's allocate_stack zeros the whole struct
                    // pthread area before calling _dl_allocate_tls_init,
                    // so stale .bss from a previous thread is cleared.
                }
            }
            // ── Zero ONLY the DTV pointer in tcbhead_t ──────────────
            // The TCB (tcbhead_t) starts at TP. Its layout (glibc 2.36+):
            //   +0:  tcb (self pointer)
            //   +8:  dtv pointer
            //   +16: self pointer
            //   +24: multiple_threads, gscope_flag
            //   +32: sysinfo
            //   +40: stack_guard
            //   +48: pointer_guard
            //   ...
            //
            // Glibc's create_thread sets tcb, self, stack_guard, and
            // pointer_guard AFTER _dl_allocate_tls returns. The DTV
            // pointer is set by _dl_allocate_tls (which we shim). Since
            // our shim doesn't allocate a real DTV (we use static TLS
            // only), we MUST zero the DTV pointer so glibc's
            // __nptl_deallocate_tsd sees dtv==NULL and skips the DTV
            // free. A non-NULL stale DTV pointer causes "munmap_chunk():
            // invalid pointer" when glibc tries to free it.
            //
            // clobbered glibc's struct pthread fields. This caused hangs
            // when threads exited (cleanup walked garbage pointers). Now
            // we zero ONLY the 8-byte DTV pointer at tcb+8.
            //
            // For stack-cache reuse (waves 2+), glibc's allocate_stack
            // zeros the struct pthread area itself before calling
            // _dl_allocate_tls_init, so stale TCB fields are already
            // cleared. For new stacks, glibc zeros the whole struct
            // pthread in allocate_stack. So we only need to ensure the
            // DTV is NULL (in case glibc's zeroing was incomplete or
            // the page came from a previous allocation).
            constexpr uint64_t TCB_DTV_OFFSET = 8;  // tcbhead_t.dtv
            try {
                mem_.store<uint64_t>(tcb + TCB_DTV_OFFSET, 0);
            } catch (...) {}
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[tls-alloc] TCB @0x%llx (total_tls=%llu, "
                        "lib=%llu, main=%llu, %s)\n",
                        static_cast<unsigned long long>(tcb),
                        static_cast<unsigned long long>(total_tls_size),
                        static_cast<unsigned long long>(lib_size),
                        static_cast<unsigned long long>(main_memsz),
                        (a0 != 0) ? "caller-alloc" : "new-block");
            }
            ret_host(tcb);
            return 0;
        }
        // ── Bifrost-emu internal dlopen syscall ──────────
        // Called by the _dl_open stub in the ld-linux shim.
        // a0 (x0) = guest pointer to library path string (null-terminated)
        // a1 (x1) = dlopen mode flags (RTLD_LAZY, RTLD_NOW, etc.)
        // Returns: a handle (>0) on success, 0 on failure.
        case 0x1002: {
            // Read the library path from guest memory.
            std::string path = yggdrasil::Yggdrasil::read_path(mem_, a0);
            if (path.empty()) {
                if (getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[dlopen] empty path\n");
                }
                ret_host(0);
                return 0;
            }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dlopen] path='%s' mode=0x%llx\n",
                        path.c_str(), static_cast<unsigned long long>(a1));
            }
            // Load the library via the dynamic linker.
            auto* dl = emu.dyn_linker_.get();
            if (!dl) {
                ret_host(0);
                return 0;
            }
            uint64_t handle = dl->load_library(cpu, path);
            if (handle == 0) {
                if (getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[dlopen] failed: %s\n", dl->error().c_str());
                }
                ret_host(0);
                return 0;
            }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dlopen] OK handle=0x%llx\n",
                        static_cast<unsigned long long>(handle));
            }
            ret_host(handle);
            return 0;
        }
        // ── Bifrost-emu internal dlsym syscall ───────────
        // a0 (x0) = dlopen handle (base address of the library)
        // a1 (x1) = guest pointer to symbol name string
        // Returns: symbol address on success, 0 on failure.
        case 0x1003: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl) { ret_host(0); return 0; }
            std::string symname = yggdrasil::Yggdrasil::read_path(mem_, a1);
            if (symname.empty()) {
                if (getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[dlsym] empty symbol name\n");
                }
                dl->set_last_error("empty symbol name");
                ret_host(0);
                return 0;
            }
            // If handle is RTLD_DEFAULT (0) or RTLD_NEXT (-1), search all
            // loaded objects via the global symbol table. Otherwise, use
            // resolve_symbol_in which searches the library's own .dynsym
            // first, then falls back to the global table.
            uint64_t addr = 0;
            if (a0 == 0 || static_cast<int64_t>(a0) == -1) {
                addr = dl->resolve_symbol(symname);
            } else {
                addr = dl->resolve_symbol_in(a0, symname);
            }
            if (addr == 0) {
                dl->set_last_error("symbol '" + symname + "' not found");
            }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dlsym] '%s' handle=0x%llx → 0x%llx\n",
                        symname.c_str(),
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(addr));
            }
            ret_host(addr);
            return 0;
        }
        // ── Bifrost-emu internal dlclose syscall ──────────
        // a0 (x0) = dlopen handle (base address of the library)
        // Returns: 0 on success, -1 on error (with dlerror set).
        case 0x1004: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl || a0 == 0) { ret_host(static_cast<int64_t>(-1)); return 0; }
            int rc = dl->close_library(cpu, a0);
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dlclose] handle=0x%llx → %d\n",
                        static_cast<unsigned long long>(a0), rc);
            }
            ret_host(static_cast<int64_t>(rc));
            return 0;
        }
        // ── Bifrost-emu internal dladdr syscall ───────────
        // a0 (x0) = address to look up
        // a1 (x1) = guest pointer to Dl_info struct (4 × uint64 = 32 bytes)
        // Returns: 1 on success (address found), 0 on failure.
        case 0x1005: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl || a0 == 0 || a1 == 0) { ret_host(0); return 0; }
            DynamicLinker::DlInfo info;
            int found = dl->dladdr(a0, info);
            // Write the Dl_info struct to guest memory.
            // struct Dl_info { const char* dli_fname; void* dli_fbase;
            //                 const char* dli_sname; void* dli_saddr; }
            try {
                mem_.store<uint64_t>(a1 + 0,  info.dli_fname);
                mem_.store<uint64_t>(a1 + 8,  info.dli_fbase);
                mem_.store<uint64_t>(a1 + 16, info.dli_sname);
                mem_.store<uint64_t>(a1 + 24, info.dli_saddr);
            } catch (...) { ret_host(0); return 0; }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dladdr] addr=0x%llx → found=%d fbase=0x%llx sname=0x%llx\n",
                        static_cast<unsigned long long>(a0), found,
                        static_cast<unsigned long long>(info.dli_fbase),
                        static_cast<unsigned long long>(info.dli_sname));
            }
            ret_host(static_cast<int64_t>(found));
            return 0;
        }
        // ── Bifrost-emu internal _dl_find_dso_for_object syscall ──
        // a0 (x0) = address to find the containing DSO for
        // Returns: the DSO's base address (handle), or 0 if not found.
        // Used by glibc's dladdr and _dl_open to determine the caller's
        // namespace. The old stub returned 0 (not found), which caused
        // dladdr to always fail and _dl_open to skip namespace detection.
        case 0x1006: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            const LoadedObject* obj = dl->find_object_by_addr(a0);
            uint64_t base = obj ? obj->base_addr : 0;
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[_dl_find_dso] addr=0x%llx → base=0x%llx (%s)\n",
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(base),
                        obj ? obj->name.c_str() : "not found");
            }
            ret_host(base);
            return 0;
        }
        // ── Bifrost-emu internal dl_iterate_phdr syscall ──
        // a0 (x0) = guest callback function pointer
        // a1 (x1) = user data pointer (passed as x2 to callback)
        // Returns: sum of callback return values (matches glibc semantics).
        // The callback is called for each loaded object with:
        //   x0 = pointer to dl_phdr_info struct (64 bytes)
        //   x1 = sizeof(dl_phdr_info) = 64
        //   x2 = data pointer (a1)
        case 0x1007: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            int rc = dl->iterate_phdr(cpu, a0, a1);
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[dl_iterate_phdr] callback=0x%llx data=0x%llx → %d\n",
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(a1), rc);
            }
            ret_host(static_cast<int64_t>(rc));
            return 0;
        }
        // ── Bifrost-emu internal _dl_find_object syscall ──
        // a0 (x0) = guest address to locate (typically the faulting pc
        //           inside a C++ throw; libgcc_s's _Unwind_Find_FDE calls
        //           _dl_find_object with the pc of the frame it's unwinding)
        // a1 (x1) = guest pointer to a 48-byte struct dl_find_object
        // Returns: 0 on success, -1 if the address isn't in a loaded object.
        // glibc 2.42 dl_find_object layout (from elf/dl-find-object.h):
        //   +0  dlfo_flags   (0 = memory-based .eh_frame_hdr, NOT fd-based;
        //                      the fd path would read() the host fd)
        //   +8  dlfo_map_start
        //   +16 dlfo_map_end
        //   +24 dlfo_link_map
        //   +32 dlfo_eh_frame (absolute .eh_frame_hdr guest address)
        //   +40 dlfo_sframe
        // libgcc_s's _Unwind_Find_FDE checks flags (must not have the
        // DLFO_EH_FRAME_FD bit) then binary-searches dlfo_eh_frame; every
        // C++ throw/catch walks this path. The previous all-zero stub made
        // it return NULL and _Unwind_RaiseException aborted. We return the
        // host LoadedObject's real PT_GNU_EH_FRAME address, which is the
        // actual linker-produced .eh_frame_hdr (version/encodings/table all
        // valid, pcrel/datarel offsets preserved by mapping at load base).
        case 0x1008: {
            auto* dl = emu.dyn_linker_.get();
            if (!dl || a0 == 0 || a1 == 0) { ret_host(-1); return 0; }
            const LoadedObject* obj = dl->find_object_by_addr(a0);
            if (!obj || obj->eh_frame_hdr_addr == 0) {
                if (getenv("BIFROST_DYNLINK_TRACE")) {
                    fprintf(stderr, "[_dl_find_object] addr=0x%llx → not found"
                            " (obj=%p)\n",
                            static_cast<unsigned long long>(a0), (void*)obj);
                }
                ret_host(-1);
                return 0;
            }
            uint64_t flags = 0, link_map = 0, sframe = 0;
            try {
                mem_.store<uint64_t>(a1 + 0,  flags);
                mem_.store<uint64_t>(a1 + 8,  obj->base_addr);
                mem_.store<uint64_t>(a1 + 16, obj->base_addr + obj->map_size);
                mem_.store<uint64_t>(a1 + 24, link_map);
                mem_.store<uint64_t>(a1 + 32, obj->eh_frame_hdr_addr);
                mem_.store<uint64_t>(a1 + 40, sframe);
            } catch (...) { ret_host(-1); return 0; }
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[_dl_find_object] addr=0x%llx → %s eh_frame=0x%llx"
                        " start=0x%llx end=0x%llx\n",
                        static_cast<unsigned long long>(a0), obj->name.c_str(),
                        static_cast<unsigned long long>(obj->eh_frame_hdr_addr),
                        static_cast<unsigned long long>(obj->base_addr),
                        static_cast<unsigned long long>(obj->base_addr + obj->map_size));
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
