// Sockets.cpp — BSD socket syscalls: socket/socketpair/bind/listen/
// accept/accept4/connect, getsockname/getpeername, sendto/recvfrom,
// setsockopt/getsockopt, shutdown and sendmsg/recvmsg (including
// SCM_RIGHTS descriptor passing).
//
// Every socket host fd is wrapped in a HostNode and registered in the
// FdTable; guest fds are resolved to host fds through it, so guest and
// host descriptor numbers stay independent.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
namespace arm64emu {
// recvmsg installs new host descriptors. Keep them owned even if guest
// copyback faults; only a completed receive transfers ownership to FdTable.
class ReceivedSocketRights {
public:
    ReceivedSocketRights(yggdrasil::FdTable& fds, msghdr& message)
        : fds_(fds), message_(message) {}
    ~ReceivedSocketRights() {
        size_t index = 0;
        for (auto* c = CMSG_FIRSTHDR(&message_); c; c = CMSG_NXTHDR(&message_, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            for (size_t off = 0; off + sizeof(int) <= c->cmsg_len - CMSG_LEN(0); off += sizeof(int)) {
                int fd; memcpy(&fd, CMSG_DATA(c) + off, sizeof(fd));
                if (index++ >= converted_ && fd >= 0) ::close(fd);
            }
        }
        if (!committed_) for (int fd : guest_fds_) fds_.close(fd);
    }
    void adopt() {
        size_t count = 0;
        for (auto* c = CMSG_FIRSTHDR(&message_); c; c = CMSG_NXTHDR(&message_, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                count += (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        guest_fds_.reserve(count);
        for (auto* c = CMSG_FIRSTHDR(&message_); c; c = CMSG_NXTHDR(&message_, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            for (size_t off = 0; off + sizeof(int) <= c->cmsg_len - CMSG_LEN(0); off += sizeof(int)) {
                int host; memcpy(&host, CMSG_DATA(c) + off, sizeof(host));
                int flags = ::fcntl(host, F_GETFL);
                auto node = std::make_shared<yggdrasil::HostNode>(host, flags < 0 ? O_RDWR : flags);
                int invalid = -1; memcpy(CMSG_DATA(c) + off, &invalid, sizeof(invalid));
                int guest = fds_.allocate(node);
                guest_fds_.push_back(guest);
                memcpy(CMSG_DATA(c) + off, &guest, sizeof(guest));
                ++converted_;
            }
        }
    }
    void commit() { committed_ = true; }
private:
    yggdrasil::FdTable& fds_;
    msghdr& message_;
    std::vector<int> guest_fds_;
    size_t converted_ = 0;
    bool committed_ = false;
};
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
int64_t syscall_sockets(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& fds_ = emu.fds();
    switch (num) {
        case 198: { // socket(domain, type, protocol) — aarch64 198
            // Forward to host. The host kernel creates a real socket fd
            // which we wrap in HostNode + FdTable.allocate so that:
            //   - close(guest_fd) actually closes the host fd
            //   - getsockopt/setsockopt/etc. can resolve the host fd
            //   - read()/write() on the socket work via FileSystem.cpp's path
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
            // A pathname socket that exists inside BIFROST_ROOT (e.g. Android's
            // /dev/socket/logdw provided by a test harness) is reached through
            // the confined parent directory. Otherwise the guest path stays a
            // host path: guest libX11 connects to the host's /tmp/.X11-unix.
            struct sockaddr_un rooted = {};
            const auto* un = reinterpret_cast<const struct sockaddr_un*>(&ss);
            if (ss.ss_family == AF_UNIX && un->sun_path[0] == '/' &&
                yggdrasil::Yggdrasil::sandbox_inside_root()) {
                const std::string guest(un->sun_path,
                    strnlen(un->sun_path, len - offsetof(struct sockaddr_un, sun_path)));
                auto pd = yggdrasil::Yggdrasil::open_parent(-1, guest);
                struct stat st;
                if (pd.valid() &&
                    ::fstatat(pd.fd, pd.base.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISSOCK(st.st_mode)) {
                    rooted.sun_family = AF_UNIX;
                    const int n = snprintf(rooted.sun_path, sizeof(rooted.sun_path),
                                           "/proc/self/fd/%d/%s", pd.fd, pd.base.c_str());
                    if (n > 0 && static_cast<size_t>(n) < sizeof(rooted.sun_path)) {
                        int r = ::connect(hfd, reinterpret_cast<struct sockaddr*>(&rooted),
                                          static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + n + 1));
                        if (r < 0) { ret_errno(); return 0; }
                        ret_host(0);
                        return 0;
                    }
                }
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
        // ── accept4 (syscall 242) ────────────────────────────────────
        // NOTE: AArch64 syscall 88 is utimensat (handled in FileSystem.cpp), NOT
        // accept4. Real accept4 is syscall 242. The old code at case 88
        // was dead — FileSystem.cpp's utimensat handler always won the dispatch
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
            //   +40: size_t    msg_controllen (8 bytes)
            //   +48: int       msg_flags     (4 bytes)
            if (!a1) { ret_err(EFAULT); return 0; }
            uint64_t msg_name = mem_.load<uint64_t>(a1);
            uint32_t msg_namelen = mem_.load<uint32_t>(a1 + 8);
            uint64_t msg_iov = mem_.load<uint64_t>(a1 + 16);
            uint64_t msg_iovlen = mem_.load<uint64_t>(a1 + 24);
            uint64_t msg_control = mem_.load<uint64_t>(a1 + 32);
            uint64_t msg_controllen = mem_.load<uint64_t>(a1 + 40);
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
            // LP64 cmsghdr layouts match, but SCM_RIGHTS numbers belong to
            // the guest descriptor table and must be translated individually.
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
            std::vector<std::shared_ptr<yggdrasil::Node>> sent_rights;
            for (auto* c = CMSG_FIRSTHDR(&host_msg); c; c = CMSG_NXTHDR(&host_msg, c)) {
                size_t remaining = ctrl_buf.data() + ctrl_buf.size() - reinterpret_cast<uint8_t*>(c);
                if (c->cmsg_len < CMSG_LEN(0) || c->cmsg_len > remaining) { ret_err(EINVAL); return 0; }
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                for (size_t off = 0; off + sizeof(int) <= c->cmsg_len - CMSG_LEN(0); off += sizeof(int)) {
                    int guest; memcpy(&guest, CMSG_DATA(c) + off, sizeof(guest));
                    auto right = fds_.get(guest);
                    int host = right ? right->host_fd() : -1;
                    if (host < 0) { ret_err(EBADF); return 0; }
                    sent_rights.push_back(right); // pin through sendmsg against concurrent close
                    memcpy(CMSG_DATA(c) + off, &host, sizeof(host));
                }
            }
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
            uint64_t msg_controllen = mem_.load<uint64_t>(a1 + 40);
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
            ReceivedSocketRights received_rights(fds_, host_msg);
            received_rights.adopt();
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
            // A short receive changes only bytes actually delivered. MSG_TRUNC
            // can report more bytes than fit; limit copyback to staged capacity.
            uint64_t remaining = static_cast<uint64_t>(r);
            for (uint64_t i = 0; i < msg_iovlen && remaining; ++i) {
                uint64_t base = mem_.load<uint64_t>(msg_iov + i * 16);
                size_t received = std::min<uint64_t>(remaining, iov_bufs[i].size());
                if (received) mem_.write(base, iov_bufs[i].data(), received);
                remaining -= received;
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
                mem_.store<uint64_t>(a1 + 40, actual_ctrl);
            }
            mem_.store<uint64_t>(a1 + 40, host_msg.msg_controllen);
            // AArch64 LP64 flags follow the full eight-byte control length.
            mem_.store<uint32_t>(a1 + 48, static_cast<uint32_t>(host_msg.msg_flags));
            received_rights.commit();
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
