// syscalls/misc.cpp — catch-all syscall handler for syscalls that don't
// fit into fs/mem/threads/time/ioctls. Handles the remaining "miscellaneous"
// syscalls after signal, I/O, and process sub-handlers have run:
//   - exit / exit_group (case 93, 94)
//   - inotify_init1 / inotify_add_watch / inotify_rm_watch (case 26, 27, 28)
//   - vmsplice / splice / tee (case 75, 76, 77 — return ENOSYS)
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
#include "core/signal.h"
#include "frontend/dynamic_linker.h"  // DynamicLinker (for _dl_allocate_tls syscall)
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "frost/audio_thunk.hpp"    // v1.5.0.alpha
#include "frost/display_thunk.hpp"  // v1.5.0.alpha
#include "syscalls/syscalls.h"

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
    // Turn 68 refactor: dispatch to sub-handlers first. Each returns
    // SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
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
            // BUGFIX (Turn 64): siginfo_t layout was wrong — si_code was
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

        // ── Bifrost-emu internal TLS-alloc syscall (Turn 76) ──────────
        // _dl_allocate_tls stub in the ld-linux shim calls this syscall
        // (number 0x1001 = 4097) to allocate a per-thread TLS block.
        //
        // glibc's pthread_create calls _dl_allocate_tls(NULL) to get a
        // fresh TCB + initialized static TLS block for each new thread.
        // The old stub just returned 0 (NULL), which caused the assertion
        // `allocatestack.c:333: size != 0` because glibc treated NULL
        // as a zero-size allocation.
        //
        // This handler:
        //   1. Reads the static TLS size from the dynamic linker.
        //   2. Allocates static_tls_size + PTHREAD_SLACK bytes.
        //   3. Places the TCB at the end (16-aligned), TLS data below it.
        //   4. Copies the static TLS template (initialized .tdata + .bss)
        //      from the main thread's static TLS block.
        //   5. Copies the TCB header fields (stack_guard, pointer_guard,
        //      etc.) from the main thread's TCB so canary checks pass.
        //   6. Sets the TCB self-pointer (tcbhead_t.tcb at offset 0).
        //   7. Returns the TCB pointer in x0.
        //
        // a0 (x0) is the `mem` argument from glibc:
        //   - 0 → allocate a new block (the normal pthread_create path)
        //   - non-zero → re-initialize the existing block at a0 (the
        //     fork() re-init path; we leave it as-is since fork inherits
        //     the parent's memory)
        case 0x1001: {
            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[tls-alloc] syscall 0x1001 called: "
                        "a0=0x%llx\n", static_cast<unsigned long long>(a0));
            }
            // glibc's _dl_allocate_tls(void *mem) semantics:
            //   mem == NULL → allocate a new block of dl_tls_static_size
            //                 bytes, initialize the TLS template + TCB in
            //                 it, return the TCB pointer.
            //   mem != NULL → mem is a CALLER-ALLOCATED block (glibc's
            //                 allocate_stack mmaps the thread stack +
            //                 struct pthread together, then passes
            //                 pd + 0x740 as mem). Initialize the TLS
            //                 template + TCB in it, return the TCB
            //                 pointer (= mem).
            //
            // BUGFIX (Turn 77 cont.): the old code treated mem != 0 as
            // the fork() re-init path and returned mem UNINITIALIZED.
            // But glibc's pthread_create → allocate_stack ALWAYS passes
            // a non-NULL mem (pd+0x740, the TCB slot inside the
            // mmap'd struct pthread). So the TLS template was never
            // copied, the TCB canary fields (stack_guard,
            // pointer_guard) were never set, and per-thread __thread
            // variables (accessed at NEGATIVE offsets from TPIDR_EL0)
            // read garbage / collided across threads. The 2-thread
            // stress test surfaced this: thread A's __thread array was
            // stomped because the TLS block was never initialized.
            //
            // The fix: in BOTH cases, initialize the TLS template below
            // the TCB and the TCB header fields. The only difference is
            // where the block comes from (mmap_alloc vs caller-provided).

            // Need the dynamic linker for static TLS info.
            auto* dl = emu.dyn_linker_.get();
            uint64_t tls_size = dl ? dl->static_tls_size() : 0;

            uint64_t tcb;       // the TCB pointer (= TPIDR_EL0 for child)
            uint64_t tls_dst;   // where to copy the TLS template (below TCB)

            if (a0 != 0) {
                // Caller-allocated block (the normal pthread_create path).
                // mem = TCB pointer. TLS data goes at [mem-tls_size, mem).
                // glibc already zeroed this region via memset before
                // calling us, but we re-copy the template to be safe.
                tcb = a0;
                tls_dst = (tls_size > 0) ? (a0 - tls_size) : a0;
            } else if (tls_size == 0) {
                // No dynamic linker (static binary) or no TLS — return a
                // minimal zeroed block so glibc doesn't crash. This path
                // shouldn't be reached for static binaries (they don't
                // call _dl_allocate_tls), but handle it gracefully.
                constexpr uint64_t FALLBACK_SIZE = 4096;
                uint64_t block = mem_.mmap_alloc(FALLBACK_SIZE);
                if (block == 0) { ret_err(ENOMEM); return 0; }
                std::vector<uint8_t> zeros(FALLBACK_SIZE, 0);
                mem_.write(block, zeros.data(), FALLBACK_SIZE);
                tcb = (block + FALLBACK_SIZE - 16) & ~0xFULL;
                mem_.store<uint64_t>(tcb, tcb);  // self pointer
                ret_host(tcb);
                return 0;
            } else {
                // mem == NULL: allocate a new block.
                // Extra space for the TCB header + struct pthread (glibc's
                // pthread descriptor is ~2 KiB; 8 KiB gives generous
                // headroom for future glibc versions and any additional
                // fields).
                constexpr uint64_t PTHREAD_SLACK = 8192;
                uint64_t alloc_size = tls_size + PTHREAD_SLACK;
                // Align the allocation to 64 bytes (TLS_TCB_ALIGN on AArch64).
                alloc_size = (alloc_size + 63) & ~63ULL;

                uint64_t block = mem_.mmap_alloc(alloc_size);
                if (block == 0) { ret_err(ENOMEM); return 0; }

                // Zero the whole block first (mmap_alloc gives zeros, but be
                // explicit in case the page was reused).
                std::vector<uint8_t> zeros(alloc_size, 0);
                mem_.write(block, zeros.data(), alloc_size);

                // TCB / struct pthread placement (BUGFIX Turn 77):
                //   [TLS data: block .. block+tls_size)
                //   [struct pthread: block+tls_size .. block+alloc_size)
                //   ^tls_dst        ^tcb (returned)            ^end
                // struct pthread starts at the TCB and extends UPWARD for
                // ~2 KiB, so we leave PTHREAD_SLACK above the TCB for
                // pd->start_routine, pd->arg, pd->tid, etc.
                tcb = block + tls_size;
                tls_dst = block;
            }

            // ── Initialize the TLS template (both cases) ────────────
            // Copy the static TLS template (initialized .tdata values).
            // The .bss portion (memsz - filesz) is already zeroed.
            if (tls_size > 0 && dl != 0) {
                try {
                    std::vector<uint8_t> tpl(tls_size);
                    mem_.read(dl->static_tls_base(), tpl.data(), tls_size);
                    mem_.write(tls_dst, tpl.data(), tls_size);
                } catch (...) {
                    // If the read fails, leave the TLS zeroed — glibc
                    // will reinitialize the critical fields itself.
                }
            }

            // ── Initialize the TCB header (both cases) ──────────────
            // Copy TCB header fields from the main thread's TCB so that
            // stack_guard, pointer_guard, and other canary values match.
            // The main thread's TPIDR_EL0 points to its TCB.
            uint64_t main_tp = emu.main_cpu_.tpidr_el0;
            if (main_tp != 0) {
                // TCB header on AArch64 glibc (tcbhead_t):
                //   +0:  void *tcb           (self pointer — we set this below)
                //   +8:  dtv_t *dtv
                //   +16: void *thread
                //   +24: void *self
                //   +32: int multiple_threads
                //   +36: int gscope_flag
                //   +40: uintptr_t sysinfo
                //   +48: uintptr_t stack_guard      ← must match main thread
                //   +56: uintptr_t pointer_guard     ← must match main thread
                //   +64: unsigned long vgetcpu_cache[2]
                //   +80: ... more fields ...
                //
                // Copy the first 128 bytes to capture all canary fields.
                // The self-pointer (offset 0) will be overwritten below.
                try {
                    std::vector<uint8_t> tcb_hdr(128);
                    mem_.read(main_tp, tcb_hdr.data(), 128);
                    mem_.write(tcb, tcb_hdr.data(), 128);
                } catch (...) {
                    // If the read fails, leave TCB zeroed — glibc will
                    // set up the fields it needs.
                }
            }

            // Set the TCB self-pointer (tcbhead_t.tcb at offset 0).
            // This is critical: glibc reads TPIDR_EL0 to get the TCB,
            // then reads tcb->tcb to verify it matches. Without this,
            // __pthread_self() returns garbage.
            mem_.store<uint64_t>(tcb, tcb);

            // Set tcb->self (offset 24) = tcb as well (some glibc paths
            // use the `self` field instead of `tcb`).
            mem_.store<uint64_t>(tcb + 24, tcb);

            if (getenv("BIFROST_DYNLINK_TRACE")) {
                fprintf(stderr, "[tls-alloc] TCB @0x%llx (TLS data "
                        "@0x%llx, size=%llu, %s)\n",
                        static_cast<unsigned long long>(tcb),
                        static_cast<unsigned long long>(tls_dst),
                        static_cast<unsigned long long>(tls_size),
                        (a0 != 0) ? "caller-alloc" : "new-block");
            }

            ret_host(tcb);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
