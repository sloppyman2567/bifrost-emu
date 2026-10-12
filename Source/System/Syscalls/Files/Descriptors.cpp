// Descriptors.cpp — syscalls that act on an open file descriptor:
// read/write and their vectored/positioned forms, close/close_range,
// dup/dup3/pipe2, lseek, fcntl, ftruncate/fallocate, fadvise64, flock and
// the sync family (sync/fsync/fdatasync/sync_file_range/syncfs).
//
// Dispatched from syscall_fs() (FileSystem.cpp). Guest descriptors are
// resolved through the FdTable; VFS nodes handle synthetic files.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/Signals.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
namespace arm64emu {
int64_t syscall_fs_descriptors(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& vfs_ = emu.vfs();
    auto& fds_ = emu.fds();
    (void)vfs_;
    switch (num) {
        // ── read — VFS-aware ──────────────────────────────────────────
        case 63: { // read
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            // Sanity-check the buffer size: real Linux caps read() at
            // SSIZE_MAX (~2 GiB on 64-bit). A buggy/malicious guest
            // passing a2 = SIZE_MAX would otherwise OOM the host.
            if (a2 > static_cast<uint64_t>(SSIZE_MAX)) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EFAULT));
                return 0;
            }
            std::vector<uint8_t> tmp(std::max<uint64_t>(a2, 1));
            ssize_t r;
            while (true) {
                r = node->read(UINT64_MAX, tmp.data(), a2);
                if (r != -EINTR) break;
                // Pre-set cpu.regs[0] = -EINTR so the signal frame (if a
                // signal is delivered) saves -EINTR. After sigreturn,
                // cpu.regs[0] will be restored to -EINTR.
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                // Drain signals. If a signal was delivered to a real
                // handler, return immediately (the handler is set up to
                // run; cpu.regs[0] = signo). Otherwise, check if SIGINT
                // was ignored — if so, inject a newline so the shell
                // prints a new prompt (mimicking bash/dash behavior).
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                if (cpu.sigint_ignored) {
                    // SIGINT was received but the guest has it set to
                    // SIG_IGN (e.g., toybox sh in interactive mode).
                    // On real Linux, the shell would just ignore it and
                    // stay blocked on read(). But for usability, we exit
                    // the emulator — this lets the user kill the shell
                    // with Ctrl+C (matching the expectation that Ctrl+C
                    // exits the foreground program). During a running
                    // command, the child gets killed first (it has
                    // SIG_DFL), so this only fires at the empty prompt.
                    cpu.sigint_ignored = false;
                    cpu.running = false;
                    cpu.exit_code = 130;  // 128 + SIGINT(2)
                    return 0;
                }
                break;  // no signal delivered, return -EINTR
            }
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(r); return 0; }
            if (r > 0) {
                mem_.write(a1, tmp.data(), r);
                if (static_cast<int>(a0) == 3) xseq_append('R', tmp.data(), r);
                if (!dbg().xcap.empty() && static_cast<int>(a0) == 3) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s.read", dbg().xcap.c_str());
                    FILE* f = fopen(path, "ab");
                    if (f) { fwrite(tmp.data(), 1, r, f); fclose(f); }
                }
                if (dbg().xtrace && static_cast<int>(a0) == 3) {
                    fprintf(stderr, "[XREAD] fd=%d ret=%d first=%02x%02x%02x%02x\n",
                            static_cast<int>(a0), (int)r,
                            tmp[0], tmp[1], tmp[2], tmp[3]);
                }
            }
            ret_host(r);
            return 0;
        }
        // ── write — VFS-aware ─────────────────────────────────────────
        case 64: { // write
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            // Sanity-check the buffer size: real Linux caps write() at
            // SSIZE_MAX (~2 GiB on 64-bit). A buggy/malicious guest
            // passing a2 = SIZE_MAX would otherwise OOM the host.
            if (a2 > static_cast<uint64_t>(SSIZE_MAX)) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EFAULT));
                return 0;
            }
            std::vector<uint8_t> tmp(a2);
            if (a2 > 0) mem_.read(a1, tmp.data(), a2);
            if (static_cast<int>(a0) == 3) xseq_append('W', tmp.data(), a2);
            if (!dbg().xcap.empty() && static_cast<int>(a0) == 3) {
                FILE* f = fopen(dbg().xcap.c_str(), "ab");
                if (f) {
                    uint32_t blen = static_cast<uint32_t>(a2);
                    fwrite(&blen, 1, 4, f);
                    fwrite(tmp.data(), 1, tmp.size(), f);
                    fclose(f);
                }
            }
            if (dbg().xtrace && static_cast<int>(a0) == 3 && a2 >= 4) {
                fprintf(stderr, "[XWRITE] fd=%d bytes=%llu hex=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                        static_cast<int>(a0), static_cast<unsigned long long>(a2),
                        tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7]);
            }
            ssize_t r = node->write(UINT64_MAX, tmp.data(), a2);
            ret_host(r);
            return 0;
        }
        // ── close — VFS-aware ─────────────────────────────────────────
        case 57: { // close
            if (dbg().xtrace) {
                auto n = fds_.get(static_cast<int>(a0));
                fprintf(stderr, "[FDLIFE] t%d close gfd=%llu hfd=%d\n", cpu.tid, (unsigned long long)a0,
                        n ? n->host_fd() : -1);
            }
            int r = fds_.close(static_cast<int>(a0));
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d close gfd=%llu res=%d\n", cpu.tid, (unsigned long long)a0, r);
            ret_host(r);
            return 0;
        }
        // ── dup / dup3 — VFS-aware ─────────────────────────────
        // BUGFIX: AArch64 has NO dup2 syscall (only dup3 at 24). The old
        // case 33 was labeled "dup2" but 33 is actually mknodat — when
        // the guest called mknodat(dirfd, path, mode, dev), the code
        // called fds_.dup2(dirfd, (int)path_ptr), treating the path
        // pointer as an fd. Removed the dup2-at-33 handler; dup is at 23
        // and dup3 is at 24 (both kept).
        case 23: { // dup
            int r = fds_.dup(static_cast<int>(a0));
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d dup old=%d new=%d\n", cpu.tid, (int)a0, r);
            ret_host(r);
            return 0;
        }
        case 24: { // dup3(oldfd, newfd, flags) — AArch64 24
            // BUGFIX: AArch64 has no dup2, only dup3. dup3 requires
            // oldfd != newfd (returns -EINVAL otherwise) and honors
            // O_CLOEXEC in flags.
            if (static_cast<int>(a0) == static_cast<int>(a1)) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINVAL));
                return 0;
            }
            // Note: O_CLOEXEC tracking is not yet implemented in FdTable;
            // for now we accept the flag and ignore it (no execve in
            // single-process guests, so FD_CLOEXEC is moot).
            int r = fds_.dup2(static_cast<int>(a0), static_cast<int>(a1));
            ret_host(r);
            return 0;
        }
        // ── lseek — VFS-aware ─────────────────────────────────────────
        case 62: { // lseek
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            ssize_t r = node->lseek(static_cast<int64_t>(a1), static_cast<int>(a2));
            ret_host(r);
            return 0;
        }
        case 59: { // pipe2(pipefd, flags) — AArch64 59
            // BUGFIX: previously wrote raw host fds directly to guest
            // memory without registering them in FdTable. Subsequent
            // read/write/close calls on those fds went through FdTable::get()
            // → nullptr → -EBADF. Fix: wrap each pipe end in a yggdrasil::HostNode
            // and register via FdTable::allocate, returning the guest fds.
            int hfds[2];
            int r = ::pipe2(hfds, open_flags_to_host(static_cast<int>(a1)));
            if (r < 0) { ret_errno(); return 0; }
            int g0 = fds_.allocate(std::make_shared<yggdrasil::HostNode>(hfds[0], O_RDONLY));
            int g1 = fds_.allocate(std::make_shared<yggdrasil::HostNode>(hfds[1], O_WRONLY));
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d pipe2 hfd0=%d gfd0=%d hfd1=%d gfd1=%d\n",
                        cpu.tid, hfds[0], g0, hfds[1], g1);
            uint32_t out[2] = { static_cast<uint32_t>(g0), static_cast<uint32_t>(g1) };
            try { mem_.write(a0, out, sizeof(out)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_host(0);
            return 0;
        }
        case 66: { // writev(fd, iov, iovcnt) — AArch64 66
            // a0=fd, a1=iovec ptr, a2=count
            // BUGFIX: previously called ::write(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds
            // (memfd-backed /proc/*, /dev/fb0) work. Also cap iovcnt
            // at IOV_MAX (1024) to prevent OOM from a corrupted count.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            if (node->host_fd() >= 0) {
                uint64_t iov0 = a1;
                uint64_t base0 = mem_.load<uint64_t>(iov0);
                uint64_t len0  = mem_.load<uint64_t>(iov0 + 8);
                uint8_t hdr[16] = {0};
                mem_.read(base0, hdr, std::min<uint64_t>(len0, 16));
                uint16_t xlen = static_cast<uint16_t>(hdr[2]) << 8 | hdr[3];
                if (dbg().xfull) { uint8_t fb[80]={0}; mem_.read(base0, fb, std::min<uint64_t>(len0,80));
                    fprintf(stderr,"[XFULL] len=%llu\n", (unsigned long long)len0);
                    for(uint64_t q=0;q<std::min<uint64_t>(len0,80);q+=16){ fprintf(stderr,"  %02x.%02x.%02x.%02x %02x.%02x.%02x.%02x | %02x.%02x.%02x.%02x %02x.%02x.%02x.%02x\n",
                        fb[q],fb[q+1],fb[q+2],fb[q+3],fb[q+4],fb[q+5],fb[q+6],fb[q+7],
                        fb[q+8],fb[q+9],fb[q+10],fb[q+11],fb[q+12],fb[q+13],fb[q+14],fb[q+15]); } }
                if (dbg().xtrace) {
                    fprintf(stderr, "[XREQ] t%d fd=%d opcode=%d len16=%u base0=%#llx iov0=%#llx bytes=%llu hex=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                            cpu.tid, static_cast<int>(a0), hdr[0], xlen,
                            static_cast<unsigned long long>(base0),
                            static_cast<unsigned long long>(iov0),
                            static_cast<unsigned long long>(len0),
                            hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7],
                            hdr[8], hdr[9], hdr[10], hdr[11], hdr[12], hdr[13], hdr[14], hdr[15]);
                }
                if (dbg().xbt && hdr[0] == 8) {
                    uint64_t fp = cpu.regs[29];
                    uint64_t lr = cpu.regs[30];
                    fprintf(stderr, "[XBT t%d] MapWindow pc=0x%llx fp=0x%llx lr=0x%llx\n",
                            cpu.tid, (unsigned long long)cpu.pc,
                            (unsigned long long)fp, (unsigned long long)lr);
                    for (int i = 0; i < 48 && fp && (fp & 7) == 0 && (fp >> 47) != 0; ++i) {
                        uint64_t w0 = 0, w1 = 0;
                        try { mem_.read(fp, &w0, 8); mem_.read(fp + 8, &w1, 8); }
                        catch (...) { break; }
                        if (w1 == 0 || (w1 & 3)) break;
                        fprintf(stderr, "  #%02d lr=%#llx\n", i, (unsigned long long)w1);
                        fp = w0;
                    }
                }
            }
            uint64_t iov = a1;
            uint64_t cnt = std::min<uint64_t>(a2, 1024);  // IOV_MAX
            if (static_cast<int>(a0) == 3) {
                for (uint64_t i = 0; i < cnt; i++) {
                    uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                    uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                    std::vector<uint8_t> tmp(len > 1<<20 ? 1<<20 : len);
                    if (tmp.size()) mem_.read(base, tmp.data(), tmp.size());
                    xseq_append('W', tmp.data(), tmp.size());
                }
            }
            FILE* xcap = dbg().xcap.empty() ? nullptr : fopen(dbg().xcap.c_str(), "ab");
            if (xcap && static_cast<int>(a0) == 3) {
                uint32_t blen = 0;
                for (uint64_t i = 0; i < cnt; i++) {
                    uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                    blen += static_cast<uint32_t>(std::min<uint64_t>(len, 1 << 20));
                }
                fwrite(&blen, 1, 4, xcap);
                for (uint64_t i = 0; i < cnt; i++) {
                    uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                    uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                    std::vector<uint8_t> tmp(len > 1<<20 ? 1<<20 : len);
                    if (tmp.size()) mem_.read(base, tmp.data(), tmp.size());
                    fwrite(tmp.data(), 1, tmp.size(), xcap);
                }
                fclose(xcap);
            }
            ssize_t total = 0;
            uint64_t req_total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                req_total += len;
            }
            bool xprobe = dbg().xprobe;
            // Gather into ONE write: writev is a single operation. Per-iovec
            // writes split a datagram (liblog's header/priority/tag/message
            // reached logd as four packets) and let other threads interleave
            // on pipes and stream sockets. Same 64 MiB defensive cap as
            // before, applied to the total.
            constexpr uint64_t kWritevCap = 64 * 1024 * 1024;
            std::vector<uint8_t> gathered;
            gathered.reserve(static_cast<size_t>(std::min<uint64_t>(req_total, kWritevCap)));
            for (uint64_t i = 0; i < cnt && gathered.size() < kWritevCap; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                len = std::min<uint64_t>(len, kWritevCap - gathered.size());
                if (len == 0) continue;
                const size_t at = gathered.size();
                gathered.resize(at + static_cast<size_t>(len));
                mem_.read(base, gathered.data() + at, static_cast<size_t>(len));
            }
            if (!gathered.empty()) {
                ssize_t n = node->write(UINT64_MAX, gathered.data(), gathered.size());
                if (xprobe && static_cast<int>(a0) == 3 && n > 0) {
                    const auto& t = gathered;
                    fprintf(stderr, "[XWBYTES] hfd=%d n=%zd hex=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                            node->host_fd(), n, t[0], t.size()>1?t[1]:0, t.size()>2?t[2]:0,
                            t.size()>3?t[3]:0, t.size()>4?t[4]:0, t.size()>5?t[5]:0,
                            t.size()>6?t[6]:0, t.size()>7?t[7]:0);
                }
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
                total = n;
            }
            if (xprobe && static_cast<int>(a0) == 3 && node->host_fd() >= 0 && total > 0) {
                uint8_t rb[2000] = {0};
                for (int k = 0; k < 8; k++) {
                    ssize_t rn = ::recv(node->host_fd(), rb, sizeof(rb), MSG_PEEK | MSG_DONTWAIT);
                    fprintf(stderr, "[XWRDELAY hfd=%d t=%d] peek=%zd hex=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                            node->host_fd(), k * 25, rn, rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
                    usleep(25000);
                }
            }
            ret_host(total);
            if (dbg().xwrcheck && static_cast<int>(a0) == 3
                && static_cast<uint64_t>(total) != req_total) {
                fprintf(stderr, "[XWRSHORT] t=%d req=%llu wrote=%zd\n",
                        cpu.tid, (unsigned long long)req_total, total);
            }
            return 0;
        }
        case 65: { // readv(fd, iov, iovcnt) — AArch64 65
            // BUGFIX: previously called ::read(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable. Also cap iovcnt
            // at IOV_MAX (1024).
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            uint64_t iov = a1;
            uint64_t cnt = std::min<uint64_t>(a2, 1024);  // IOV_MAX
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                // Defensive cap (same as writev).
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                std::vector<uint8_t> tmp(len);
                ssize_t n;
                while (true) {
                    n = node->read(UINT64_MAX, tmp.data(), len);
                    if (n != -EINTR) break;
                    cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                    if (emu.handle_eintr(cpu)) return 0;
                    break;
                }
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
                if (n > 0) {
                    mem_.write(base, tmp.data(), static_cast<size_t>(n));
                    if (static_cast<int>(a0) == 3) xseq_append('R', tmp.data(), n);
                    if (dbg().xtrace && static_cast<int>(a0) == 3) {
                        fprintf(stderr, "[XREADV] fd=%d ret=%d first=%02x%02x%02x%02x\n",
                                static_cast<int>(a0), (int)n, tmp[0], tmp[1], tmp[2], tmp[3]);
                    }
                    if (!dbg().xcap.empty() && static_cast<int>(a0) == 3) {
                        char path[512];
                        snprintf(path, sizeof(path), "%s.readv", dbg().xcap.c_str());
                        FILE* f = fopen(path, "ab");
                        if (f) { fwrite(tmp.data(), 1, n, f); fclose(f); }
                    }
                }
                total += n;
                if (static_cast<size_t>(n) < len) break;
            }
            ret_host(total);
            return 0;
        }
        case 67: { // pread64(fd, buf, count, offset) — AArch64 67
            // BUGFIX: previously implemented as preadv64 (iovec array),
            // but 67 is pread64 (single buffer). The old code interpreted
            // the guest's `buf` pointer as an iovec array, reading garbage
            // memory as (base, len) pairs. Fix: read into a single buffer.
            // Also: resolve via FdTable so virtual fds (memfd-backed
            // /proc/*, /dev/fb0) work correctly.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            if (a2 == 0) { ret_host(0); return 0; }
            // Sanity-check the buffer size (same as read/write).
            if (a2 > static_cast<uint64_t>(SSIZE_MAX)) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EFAULT));
                return 0;
            }
            std::vector<uint8_t> tmp(a2);
            ssize_t n;
            while (true) {
                n = node->read(static_cast<uint64_t>(a3), tmp.data(), a2);
                if (n != -EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;
                break;
            }
            if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
            if (n > 0) mem_.write(a1, tmp.data(), static_cast<size_t>(n));
            ret_host(static_cast<uint64_t>(n));
            return 0;
        }
        case 69: { // preadv(fd, iov, iovcnt, offset) — AArch64 69
            // Vectorized pread: read from `fd` at `offset` into an array
            // of iovecs. Marshals the guest iovec array into a single host
            // bounce buffer, calls ::preadv, then scatters the result back
            // to each guest iovec base address.
            int fd = static_cast<int>(a0);
            auto node = fds_.get(fd);
            if (!node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0;
            }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_err(EBADF); return 0; }
            uint64_t iov_g = a1;
            int iovcnt = static_cast<int>(a2);
            off_t offset = static_cast<off_t>(a3);
            if (iovcnt <= 0 || iovcnt > 1024) { ret_err(EINVAL); return 0; }
            // Marshal guest iovec array and compute total length.
            std::vector<struct iovec> giovs(iovcnt);
            size_t total_len = 0;
            for (int i = 0; i < iovcnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_g + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov_g + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                giovs[i].iov_base = reinterpret_cast<void*>(base);
                giovs[i].iov_len = len;
                total_len += len;
            }
            if (total_len > 64 * 1024 * 1024) total_len = 64 * 1024 * 1024;
            std::vector<uint8_t> bounce(total_len);
            struct iovec host_iov = { bounce.data(), total_len };
            ssize_t r;
            while (true) {
                r = ::preadv(hfd, &host_iov, 1, offset);
                if (r != -EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;
                break;
            }
            if (r < 0) { ret_errno(); return 0; }
            // Scatter back to guest iovecs in order.
            size_t copied = 0;
            for (int i = 0; i < iovcnt && copied < static_cast<size_t>(r); i++) {
                size_t take = std::min<size_t>(giovs[i].iov_len, static_cast<uint64_t>(r) - copied);
                if (take > 0) {
                    mem_.write(reinterpret_cast<uint64_t>(giovs[i].iov_base), bounce.data() + copied, take);
                }
                copied += take;
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 70: { // pwritev(fd, iov, iovcnt, offset) — AArch64 70
            // Vectorized pwrite: writes to `fd` at `offset` from an array
            // of iovecs. Gathers guest data into a single host bounce
            // buffer then calls ::pwritev.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_err(EBADF); return 0; }
            uint64_t iov_g = a1;
            int iovcnt = static_cast<int>(a2);
            off_t offset = static_cast<off_t>(a3);
            if (iovcnt <= 0 || iovcnt > 1024) { ret_err(EINVAL); return 0; }
            // Marshal guest iovec array.
            std::vector<struct iovec> giovs(iovcnt);
            size_t total_len = 0;
            for (int i = 0; i < iovcnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_g + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov_g + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                giovs[i].iov_base = reinterpret_cast<void*>(base);
                giovs[i].iov_len = len;
                total_len += len;
            }
            if (total_len > 64 * 1024 * 1024) total_len = 64 * 1024 * 1024;
            std::vector<uint8_t> bounce(total_len);
            // Gather from guest iovecs into bounce buffer.
            size_t copied = 0;
            for (int i = 0; i < iovcnt && copied < total_len; i++) {
                size_t take = std::min<size_t>(giovs[i].iov_len, total_len - copied);
                if (take > 0) {
                    mem_.read(reinterpret_cast<uint64_t>(giovs[i].iov_base), bounce.data() + copied, take);
                }
                copied += take;
            }
            struct iovec host_iov = { bounce.data(), copied };
            ssize_t r = ::pwritev(hfd, &host_iov, 1, offset);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
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
        case 25: { // fcntl(fd, cmd, arg) — AArch64 25
            // BUGFIX: previously a no-op stub returning 0. This broke
            // F_GETFL/F_SETFL (O_NONBLOCK never applied), F_GETFD/F_SETFD
            // (FD_CLOEXEC never tracked), and F_DUPFD. We now implement
            // the common cmds by forwarding to the host fd (resolved via
            // FdTable) when one exists.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            int cmd = static_cast<int>(a1);
            int hfd = node->host_fd();
            switch (cmd) {
                case F_DUPFD: {  // 0 — duplicate fd, new fd >= arg
                    // in a2. The old code ignored it and always allocated
                    // the lowest available fd, breaking programs that rely
                    // on F_DUPFD returning a fd >= the requested minimum
                    // (e.g., Python's os.dup(fd, min_fd=3)).
                    int min_fd = static_cast<int>(a2);
                    if (min_fd < 0) min_fd = 0;
                    int r = fds_.dup(static_cast<int>(a0), min_fd);
                    if (r < 0) { ret_err(EBADF); return 0; }
                    ret_host(r);
                    return 0;
                }
                case F_DUPFD_CLOEXEC: {  // 1030 — duplicate fd with FD_CLOEXEC
                    // default case, returning 0 (success). This made the
                    // guest think it got fd=0 (stdin), corrupting stdin
                    // for Python's os.dup, Java fd management, etc.
                    // We don't track per-fd FD_CLOEXEC in FdTable, but we
                    // can still allocate a new fd pointing to the same Node.
                    // The FD_CLOEXEC flag is lost — that's a known
                    // limitation (only matters if the guest later execve's,
                    // which is rare). The fd allocation is correct.
                    int min_fd = static_cast<int>(a2);
                    if (min_fd < 0) min_fd = 0;
                    int r = fds_.dup(static_cast<int>(a0), min_fd);
                    if (r < 0) { ret_err(EBADF); return 0; }
                    ret_host(r);
                    return 0;
                }
                case F_GETFD: {  // 1 — get fd flags (FD_CLOEXEC)
                    if (hfd >= 0) {
                        int r = ::fcntl(hfd, F_GETFD);
                        ret_host(r < 0 ? 0 : r);
                    } else {
                        ret_host(0);
                    }
                    return 0;
                }
                case F_SETFD: {  // 2 — set fd flags (FD_CLOEXEC)
                    if (hfd >= 0) {
                        int r = ::fcntl(hfd, F_SETFD, static_cast<int>(a2));
                        if (r < 0) { ret_errno(); return 0; }
                        ret_host(r);
                    } else {
                        ret_host(0);
                    }
                    return 0;
                }
                case F_GETFL: {  // 3 — get file status flags
                    // Query the host fd for live flags (including any
                    // previously applied via F_SETFL). Fall back to the
                    // yggdrasil::Node's stored flags if there's no host fd (virtual
                    // VNodes like /dev/fb0, memfd-backed /proc/*).
                    if (hfd >= 0) {
                        int r = ::fcntl(hfd, F_GETFL);
                        ret_host(open_flags_to_guest(r < 0 ? node->flags() : r));
                    } else {
                        ret_host(open_flags_to_guest(node->flags()));
                    }
                    return 0;
                }
                case F_SETFL: {  // 4 — set file status flags (O_NONBLOCK etc.)
                    int settable = open_flags_to_host(static_cast<int>(a2)) &
                                   (O_APPEND | O_NONBLOCK | O_ASYNC | O_DIRECT | O_NOATIME);
                    if (hfd >= 0) {
                        // Host first: on failure guest state must not change.
                        int r = ::fcntl(hfd, F_SETFL, settable);
                        if (r < 0) { ret_errno(); return 0; }
                    }
                    node->set_flags((node->flags() & ~ (O_APPEND | O_NONBLOCK | O_ASYNC | O_DIRECT | O_NOATIME)) | settable);
                    ret_host(0);
                    return 0;
                }
                case F_GETLK:    // 5
                case F_SETLK:    // 6
                case F_SETLKW: { // 7
                    // POSIX file locks — forward to host for real fds.
                    if (hfd >= 0) {
                        // asm-generic's 64-bit flock ABI (AArch64): shorts
                        // at 0/2, signed off_t at 8/16, pid_t at 24.
                        struct GuestFlock {
                            int16_t type, whence;
                            uint32_t pad;
                            int64_t start, len;
                            int32_t pid;
                            uint32_t tail_pad;
                        } guest{};
                        static_assert(sizeof(GuestFlock) == 32, "AArch64 flock ABI");
                        try { mem_.read(a2, &guest, sizeof(guest)); }
                        catch (...) { ret_err(EFAULT); return 0; }
                        struct flock lock{};
                        lock.l_type = guest.type;
                        lock.l_whence = guest.whence;
                        lock.l_start = guest.start;
                        lock.l_len = guest.len;
                        lock.l_pid = guest.pid;
                        int r = ::fcntl(hfd, cmd, &lock);
                        if (r < 0) { ret_errno(); return 0; }
                        if (cmd == F_GETLK) {
                            guest.type = lock.l_type;
                            guest.whence = lock.l_whence;
                            guest.start = lock.l_start;
                            guest.len = lock.l_len;
                            guest.pid = lock.l_pid;
                            try { mem_.write(a2, &guest, sizeof(guest)); }
                            catch (...) { ret_err(EFAULT); return 0; }
                        }
                        ret_host(r);
                    } else {
                        ret_host(0);
                    }
                    return 0;
                }
                case F_ADD_SEALS: // 1033, integer bitmask (not a guest pointer)
                case F_GET_SEALS: { // 1034
                    if (hfd < 0) { ret_err(EINVAL); return 0; }
                    int r = ::fcntl(hfd, cmd, static_cast<int>(a2));
                    if (r < 0) { ret_errno(); return 0; }
                    ret_host(r);
                    return 0;
                }
                default:
                    // Unknown cmd — return -EINVAL (not 0!) so the guest
                    // knows the cmd is unsupported. The old code returned 0
                    // (success), which made F_DUPFD_CLOEXEC silently return
                    // "fd 0" (stdin) — corrupting stdin for Python/Java.
                    ret_err(EINVAL);
                    return 0;
            }
        }
        case 46: { // ftruncate(fd, length) — AArch64 46
            // BUGFIX: previously called ::ftruncate(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_err(EBADF); return 0; }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_err(EINVAL); return 0; }
            int r = ::ftruncate(hfd, (off_t)a1);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 47: { // fallocate(fd, mode, offset, len) — aarch64 47
            // BUGFIX: previously rejected any mode != 0, but
            // FALLOC_FL_KEEP_SIZE (0x01) is a commonly-supported mode that
            // we can pass through safely. Also: resolve via FdTable so
            // virtual fds work. Only punch-hole / collapse-range /
            // zero-range / insert-range / collate-range modes are rejected.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int mode = static_cast<int>(a1);
            // FALLOC_FL_KEEP_SIZE = 0x01 (allowed). All other bits
            // (PUNCH_HOLE=0x02, COLLAPSE_RANGE=0x08, ZERO_RANGE=0x10,
            // INSERT_RANGE=0x20, COLLATE_RANGE=0x40) require kernel
            // support that may not be present and may interact badly
            // with our memory model. Reject them.
            if (mode & ~0x01) { ret_err(ENOSYS); return 0; }
            int r = ::fallocate(hfd, mode, (off_t)a2, (off_t)a3);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
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
        case 267: { // syncfs(fd) — AArch64 267
            // Forward to host syncfs.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_err(EBADF); return 0; }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_host(0); return 0; }  // virtual fd — no-op
#ifdef SYS_syncfs
            int r = ::syscall(SYS_syncfs, hfd);
            if (r < 0) { ret_errno(); return 0; }
#else
            ::sync();  // fallback: sync everything
#endif
            ret_host(0);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
