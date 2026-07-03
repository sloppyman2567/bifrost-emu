// syscalls/fs.cpp — file syscalls: openat/close/read/write/dup/pipe2/
// mkdirat/unlinkat/renameat/writev/readv/preadv64/fstat/lseek/getdents64/
// statx/fstatat/readlinkat/fcntl/fstatfs/statfs/faccessat/chdir/fchdir/
// ftruncate/chmod/fchmod/utimensat/unlink/symlink/link/truncate/fallocate/
// sendfile/getcwd.
//
// All case bodies are extracted verbatim from the original syscalls.cpp
//  EXCEPT case 56 (openat), which has been rewritten to
// use the new VFS abstraction (src/vfs/) instead of inline /proc//dev/
// else-if chains.
//
// References to private Emulator members (mem_, elf_path_, graphics_)
// work via the friend declaration in core/emulator.h.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include "vfs/vfs.h"
#include "vfs/vfs_table.h"

#include <cerrno>
#include <sys/statfs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>

namespace arm64emu {

int64_t syscall_fs(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& vfs_ = emu.vfs_;
    auto& fds_ = emu.fds_;
    auto& elf_path_ = emu.elf_path_;

    switch (num) {
        // ── openat — REWRITTEN to use VFS ─────────────────────────────
        // The original 164-line inline /proc//dev/ chain is replaced by
        // a single VFS::open() call. The VFS dispatches to procfs/devfs/
        // host passthrough internally.
        case 56: { // openat
            std::string path = VFS::VFS::read_path(mem_, a1);
            int err = 0;
            auto node = vfs_.open(path, static_cast<int>(a2), (mode_t)a3, &err);
            if (!node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(err != 0 ? err : -ENOENT));
                return 0;
            }
            // Adopt into the FdTable.
            int guest_fd = fds_.allocate(std::shared_ptr<VNode>(std::move(node)));
            ret_host(guest_fd);
            return 0;
        }

        // ── read — VFS-aware ──────────────────────────────────────────
        case 63: { // read
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            std::vector<uint8_t> tmp(std::max<uint64_t>(a2, 1));
            ssize_t r = node->read(UINT64_MAX, tmp.data(), a2);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(r); return 0; }
            if (r > 0) mem_.write(a1, tmp.data(), r);
            ret_host(r);
            return 0;
        }

        // ── write — VFS-aware ─────────────────────────────────────────
        case 64: { // write
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            std::vector<uint8_t> tmp(a2);
            if (a2 > 0) mem_.read(a1, tmp.data(), a2);
            ssize_t r = node->write(UINT64_MAX, tmp.data(), a2);
            ret_host(r);
            return 0;
        }

        // ── close — VFS-aware ─────────────────────────────────────────
        case 57: { // close
            int r = fds_.close(static_cast<int>(a0));
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
        case 33: { // mknodat(dirfd, path, mode, dev) — AArch64 33
            // BUGFIX: previously implemented as dup2 (which doesn't exist
            // on AArch64). The real syscall at 33 is mknodat. Forward to
            // host mknodat.
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::mknodat(static_cast<int>(a0), path.c_str(),
                              static_cast<mode_t>(a2), static_cast<dev_t>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
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

        // ── fstat — VFS-aware ─────────────────────────────────────────
        case 80: { // fstat(fd, statbuf) — AArch64 80
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            struct stat st{};
            int r = node->fstat(&st);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(r); return 0; }
            // Build AArch64 struct stat (128 bytes) — same layout as fstatat.
            uint8_t buf[128] = {0};
            uint64_t* p = reinterpret_cast<uint64_t*>(buf);
            p[0] = st.st_dev;
            p[1] = st.st_ino;
            reinterpret_cast<uint32_t*>(&p[2])[0] = st.st_mode;
            reinterpret_cast<uint32_t*>(&p[2])[1] = st.st_nlink;
            reinterpret_cast<uint32_t*>(&p[3])[0] = st.st_uid;
            reinterpret_cast<uint32_t*>(&p[3])[1] = st.st_gid;
            p[4] = st.st_rdev;
            p[6] = st.st_size;
            reinterpret_cast<uint32_t*>(&p[7])[0] = st.st_blksize;
            p[8] = st.st_blocks;
            p[9]  = st.st_atim.tv_sec;
            p[10] = st.st_atim.tv_nsec;
            p[11] = st.st_mtim.tv_sec;
            p[12] = st.st_mtim.tv_nsec;
            p[13] = st.st_ctim.tv_sec;
            p[14] = st.st_ctim.tv_nsec;
            mem_.write(a1, buf, 128);
            ret_host(0);
            return 0;
        }

        case 59: { // pipe2(pipefd, flags) — AArch64 59
            // BUGFIX: previously wrote raw host fds directly to guest
            // memory without registering them in FdTable. Subsequent
            // read/write/close calls on those fds went through FdTable::get()
            // → nullptr → -EBADF. Fix: wrap each pipe end in a HostVNode
            // and register via FdTable::allocate, returning the guest fds.
            int hfds[2];
            int r = ::pipe2(hfds, static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            int g0 = fds_.allocate(std::make_shared<HostVNode>(hfds[0], O_RDONLY));
            int g1 = fds_.allocate(std::make_shared<HostVNode>(hfds[1], O_WRONLY));
            uint32_t out[2] = { static_cast<uint32_t>(g0), static_cast<uint32_t>(g1) };
            try { mem_.write(a0, out, sizeof(out)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_host(0);
            return 0;
        }

        case 34: { // mkdirat
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::mkdirat(static_cast<int>(a0), path.c_str(), (mode_t)a2);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        case 35: { // unlinkat
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::unlinkat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }

        case 38: { // renameat
            std::string oldp = VFS::remap_path(VFS::read_path(mem_, a1));
            std::string newp = VFS::remap_path(VFS::read_path(mem_, a3));
            int r = ::renameat(static_cast<int>(a0), oldp.c_str(), static_cast<int>(a2), newp.c_str());
            if (r < 0) { ret_errno(); return 0; }
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
            uint64_t iov = a1;
            uint64_t cnt = std::min<uint64_t>(a2, 1024);  // IOV_MAX
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                // Defensive cap: if len is absurdly large, the iovec is
                // corrupted (JIT codegen bug). Clamp to avoid OOM crash.
                if (len > 64 * 1024 * 1024) {
                    len = 64 * 1024 * 1024;
                }
                std::vector<uint8_t> tmp(len);
                mem_.read(base, tmp.data(), len);
                ssize_t n = node->write(UINT64_MAX, tmp.data(), len);
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
                total += n;
                if (static_cast<size_t>(n) < len) break;
            }
            ret_host(total);
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
                ssize_t n = node->read(UINT64_MAX, tmp.data(), len);
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
                if (n > 0) mem_.write(base, tmp.data(), static_cast<size_t>(n));
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
            std::vector<uint8_t> tmp(a2);
            ssize_t n = node->read(static_cast<uint64_t>(a3), tmp.data(), a2);
            if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(n)); return 0; }
            if (n > 0) mem_.write(a1, tmp.data(), static_cast<size_t>(n));
            ret_host(static_cast<uint64_t>(n));
            return 0;
        }

        case 61: { // getdents64(fd, dirent_buf, count)
            // Read real directory entries from the host and convert to
            // the AArch64 linux_dirent64 layout.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_host(-EBADF); return 0; }
            // Use the host fd directly if it's a HostVNode.
            int host_fd = node->host_fd();
            if (host_fd < 0) { ret_host(-ENOTDIR); return 0; }
            // Call the host getdents64.
            char host_buf[8192];
            int n = ::syscall(SYS_getdents64, host_fd, host_buf, sizeof(host_buf));
            if (n < 0) { ret_host(-errno); return 0; }
            if (n > static_cast<int>(a2)) n = a2;  // truncate to count
            mem_.write(a1, host_buf, n);
            ret_host(n);
            return 0;
        }

        case 291: { // statx (Linux 4.11+, glibc uses it for fstatat fallback)
            // statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf)
            // Do a real stat on the (remapped) host path and convert to statx.
            std::string path = VFS::read_path(mem_, a1);
            std::string host_path = VFS::remap_path(path);
            struct stat st;
            int r;
            int host_flags = static_cast<int>(a2);
            // AT_SYMLINK_NOFOLLOW → don't follow symlinks
            // AT_EMPTY_PATH → stat the fd itself
            if (static_cast<int>(a0) == AT_FDCWD || (host_path.size() > 0 && host_path[0] == '/')) {
                r = ::fstatat(AT_FDCWD, host_path.c_str(), &st, host_flags & AT_SYMLINK_NOFOLLOW);
            } else {
                r = ::fstatat(static_cast<int>(a0), host_path.c_str(), &st, host_flags & AT_SYMLINK_NOFOLLOW);
            }
            if (r < 0) { ret_host(-errno); return 0; }
            // Build statx structure (256 bytes).
            uint8_t buf[256];
            memset(buf, 0, sizeof(buf));
            uint32_t stx_mask = 0x7FF; // STATX_BASIC_STATS
            memcpy(buf + 0, &stx_mask, 4);
            uint32_t blksize = st.st_blksize;
            memcpy(buf + 4, &blksize, 4);
            uint64_t attr = 0;
            memcpy(buf + 8, &attr, 8);
            uint32_t nlink = st.st_nlink;
            memcpy(buf + 0x10, &nlink, 4);
            uint32_t uid = st.st_uid, gid = st.st_gid;
            memcpy(buf + 0x14, &uid, 4);
            memcpy(buf + 0x18, &gid, 4);
            uint16_t mode = static_cast<uint16_t>(st.st_mode);
            memcpy(buf + 0x1C, &mode, 2);
            uint16_t spare = 0;
            memcpy(buf + 0x1E, &spare, 2);
            uint64_t ino = st.st_ino;
            memcpy(buf + 0x20, &ino, 8);
            uint64_t size = st.st_size;
            memcpy(buf + 0x28, &size, 8);
            uint64_t blocks = st.st_blocks;
            memcpy(buf + 0x30, &blocks, 8);
            uint64_t attr_mask = 0;
            memcpy(buf + 0x38, &attr_mask, 8);
            uint64_t atime_sec = st.st_atim.tv_sec;
            uint32_t atime_nsec = st.st_atim.tv_nsec;
            memcpy(buf + 0x40, &atime_sec, 8);
            memcpy(buf + 0x48, &atime_nsec, 4);
            uint64_t btime_sec = 0; uint32_t btime_nsec = 0;
            memcpy(buf + 0x4C, &btime_sec, 8);
            memcpy(buf + 0x54, &btime_nsec, 4);
            uint64_t ctime_sec = st.st_ctim.tv_sec;
            uint32_t ctime_nsec = st.st_ctim.tv_nsec;
            memcpy(buf + 0x58, &ctime_sec, 8);
            memcpy(buf + 0x60, &ctime_nsec, 4);
            uint64_t mtime_sec = st.st_mtim.tv_sec;
            uint32_t mtime_nsec = st.st_mtim.tv_nsec;
            memcpy(buf + 0x64, &mtime_sec, 8);
            memcpy(buf + 0x6C, &mtime_nsec, 4);
            uint32_t rdev_major = major(st.st_rdev), rdev_minor = minor(st.st_rdev);
            memcpy(buf + 0x70, &rdev_major, 4);
            memcpy(buf + 0x74, &rdev_minor, 4);
            uint32_t dev_major = major(st.st_dev), dev_minor = minor(st.st_dev);
            memcpy(buf + 0x78, &dev_major, 4);
            memcpy(buf + 0x7C, &dev_minor, 4);
            mem_.write(a4, buf, 256);
            ret_host(0);
            return 0;
        }

        case 79: { // fstatat / newfstatat(dirfd, pathname, statbuf, flags)
            // Do a real stat on the (mapped) host path so guest programs
            // see correct file sizes, types, and permissions.
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            struct stat st;
            int r;
            if (static_cast<int>(a0) == AT_FDCWD || (path.size() > 0 && path[0] == '/')) {
                r = ::fstatat(AT_FDCWD, path.c_str(), &st, static_cast<int>(a3));
            } else {
                r = ::fstatat(static_cast<int>(a0), path.c_str(), &st, static_cast<int>(a3));
            }
            if (r < 0) {
                ret_host(-errno);
                return 0;
            }
            // Build the AArch64 struct stat (128 bytes):
            //   offset  0: st_dev     (8)
            //   offset  8: st_ino     (8)
            //   offset 16: st_mode    (4)
            //   offset 20: st_nlink   (4)
            //   offset 24: st_uid     (4)
            //   offset 28: st_gid     (4)
            //   offset 32: st_rdev    (8)
            //   offset 40: __pad1     (8) = 0
            //   offset 48: st_size    (8)
            //   offset 56: st_blksize (4)
            //   offset 60: __pad2     (4) = 0
            //   offset 64: st_blocks  (8)
            //   offset 72: st_atime   (8)
            //   offset 80: st_atime_nsec (8)
            //   offset 88: st_mtime   (8)
            //   offset 96: st_mtime_nsec (8)
            //   offset 104: st_ctime  (8)
            //   offset 112: st_ctime_nsec (8)
            //   offset 120: __unused4 (4)
            //   offset 124: __unused5 (4)
            uint8_t buf[128] = {0};
            uint64_t* p = reinterpret_cast<uint64_t*>(buf);
            p[0] = st.st_dev;                                         // 0
            p[1] = st.st_ino;                                         // 8
            reinterpret_cast<uint32_t*>(&p[2])[0] = st.st_mode;       // 16
            reinterpret_cast<uint32_t*>(&p[2])[1] = st.st_nlink;      // 20
            reinterpret_cast<uint32_t*>(&p[3])[0] = st.st_uid;        // 24
            reinterpret_cast<uint32_t*>(&p[3])[1] = st.st_gid;        // 28
            p[4] = st.st_rdev;                                        // 32
            // p[5] = 0 (__pad1, already zeroed)                       // 40
            p[6] = st.st_size;                                        // 48
            reinterpret_cast<uint32_t*>(&p[7])[0] = st.st_blksize;    // 56
            // __pad2 at 60 already zeroed
            p[8] = st.st_blocks;                                      // 64
            p[9]  = st.st_atim.tv_sec;                                // 72
            p[10] = st.st_atim.tv_nsec;                               // 80
            p[11] = st.st_mtim.tv_sec;                                // 88
            p[12] = st.st_mtim.tv_nsec;                               // 96
            p[13] = st.st_ctim.tv_sec;                                // 104
            p[14] = st.st_ctim.tv_nsec;                               // 112
            mem_.write(a2, buf, 128);
            ret_host(0);
            return 0;
        }

        case 78: { // readlinkat(dirfd, pathname, buf, bufsiz) — AArch64 78
            // Handle /proc/self/exe specially (return the ELF path).
            // For all other paths, call the host readlinkat so symlinks
            // and regular files behave correctly.
            if (a1 != 0) {
                // Read the path string from guest memory, bounded by a
                // fixed max length. The old code checked `off > 256`
                // AFTER the byte load, allowing a 257-byte read past the
                // NUL terminator. Fixed: check BEFORE load, and use a
                // named constant for the max path length.
                constexpr size_t MAX_PATH_SCAN = 256;
                std::vector<uint8_t> path_bytes;
                path_bytes.reserve(MAX_PATH_SCAN);
                for (size_t off = 0; off < MAX_PATH_SCAN; off++) {
                    uint8_t c;
                    try {
                        c = mem_.load<uint8_t>(a1 + off);
                    } catch (...) {
                        // Bad pointer — return EFAULT.
                        ret_err(EFAULT);
                        return 0;
                    }
                    if (c == 0) break;
                    path_bytes.push_back(c);
                }
                std::string path_str(reinterpret_cast<const char*>(path_bytes.data()),
                                     path_bytes.size());
                if (path_str == "/proc/self/exe") {
                    if (a3 > 0 && elf_path_.size() < a3) {
                        try {
                            mem_.write(a2, elf_path_.data(), elf_path_.size() + 1);
                        } catch (...) {
                            ret_err(EFAULT);
                            return 0;
                        }
                        ret_host(elf_path_.size());
                        return 0;
                    }
                    ret_err(ENOSYS);
                    return 0;
                }
                // Call host readlinkat for real filesystem paths.
                char buf[4096];
                ssize_t n = ::readlinkat(static_cast<int>(a0), path_str.c_str(),
                                         buf, sizeof(buf));
                if (n < 0) { ret_errno(); return 0; }
                if (static_cast<size_t>(n) > a3) n = static_cast<ssize_t>(a3);
                try {
                    mem_.write(a2, buf, static_cast<size_t>(n));
                } catch (...) {
                    ret_err(EFAULT);
                    return 0;
                }
                ret_host(static_cast<uint64_t>(n));
                return 0;
            }
            ret_err(EFAULT);
            return 0;
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
                case F_DUPFD: {  // 0
                    int r = fds_.dup(static_cast<int>(a0));
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
                    // VNode's stored flags if there's no host fd (virtual
                    // VNodes like /dev/fb0, memfd-backed /proc/*).
                    if (hfd >= 0) {
                        int r = ::fcntl(hfd, F_GETFL);
                        ret_host(r < 0 ? node->flags() : r);
                    } else {
                        ret_host(node->flags());
                    }
                    return 0;
                }
                case F_SETFL: {  // 4 — set file status flags (O_NONBLOCK etc.)
                    if (hfd >= 0) {
                        // Only apply flags that can be changed via F_SETFL:
                        // O_APPEND, O_NONBLOCK, O_ASYNC, O_DIRECT, O_NOATIME.
                        int settable = a2 & (O_APPEND | O_NONBLOCK | O_ASYNC | O_DIRECT | O_NOATIME);
                        int r = ::fcntl(hfd, F_SETFL, settable);
                        if (r < 0) { ret_errno(); return 0; }
                    }
                    ret_host(0);
                    return 0;
                }
                case F_GETLK:    // 5
                case F_SETLK:    // 6
                case F_SETLKW: { // 7
                    // POSIX file locks — forward to host for real fds.
                    if (hfd >= 0) {
                        int r = ::fcntl(hfd, cmd, reinterpret_cast<void*>(a2));
                        if (r < 0) { ret_errno(); return 0; }
                        ret_host(r);
                    } else {
                        ret_host(0);
                    }
                    return 0;
                }
                default:
                    // Unknown cmd — return 0 (success) to avoid breaking
                    // guests that probe exotic fcntl cmds.
                    ret_host(0);
                    return 0;
            }
        }

        case 44: { // fstatfs
            struct statfs sfs{};
            sfs.f_type = 0xEF53;       // ext2 magic
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_ok();
            return 0;
        }

        case 43: { // statfs (by path)
            struct statfs sfs{};
            sfs.f_type = 0xEF53;
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_ok();
            return 0;
        }

        case 48: { // faccessat(dirfd, path, mode, flags) — AArch64 48
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::faccessat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 50: { // fchdir(fd) — AArch64 50
            // BUGFIX: previously called ::fchdir(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::fchdir(hfd);
            if (r < 0) { ret_errno(); return 0; }
            char buf[PATH_MAX];
            if (::getcwd(buf, sizeof(buf))) {
                emu.vfs_.apply_chdir(std::string(buf));
            }
            ret_host(r);
            return 0;
        }

        case 49: { // chdir(path) — AArch64 49
            std::string guest_path = VFS::read_path(mem_, a0);
            // Update the guest-side cwd first (resolves relative paths
            // against the current cwd). Then call host chdir on the
            // remapped path so any subsequent host-relative opens work.
            emu.vfs_.apply_chdir(guest_path);
            std::string path = VFS::remap_path(guest_path);
            int r = ::chdir(path.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 46: { // ftruncate(fd, length) — AArch64 46
            // BUGFIX: previously called ::ftruncate(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::ftruncate(hfd, (off_t)a1);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 52: { // fchmod(fd, mode) — AArch64 52
            // BUGFIX: previously labeled "chmod" but AArch64 has no chmod
            // (only fchmodat at 53). The real syscall at 52 is fchmod.
            // Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            int r = ::fchmod(hfd, (mode_t)a1);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 53: { // fchmodat(dirfd, path, mode, flags) — AArch64 53
            // BUGFIX: previously labeled "fchmod" but 53 is fchmodat.
            // The old code called ::fchmod(fd, mode) treating the dirfd as
            // a fd. Fix: call ::fchmodat(dirfd, path, mode, flags).
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::fchmodat(static_cast<int>(a0), path.c_str(),
                               (mode_t)a2, static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 88: { // utimensat(dirfd, path, times, flags) — AArch64 88
            // SECURITY FIX: do NOT cast the guest pointer `a2` directly to
            // `const struct timespec*` — that dereferences garbage host
            // memory and crashes. Read the guest's times array into a
            // local buffer first, then pass that to ::utimensat.
            std::string path = a1 ? VFS::remap_path(VFS::read_path(mem_, a1)) : std::string();
            struct timespec times_buf[2];
            struct timespec* times_ptr = nullptr;
            if (a2 != 0) {
                // Read 2× timespec from guest memory. Each is 16 bytes
                // (tv_sec:8, tv_nsec:8) on AArch64.
                try {
                    mem_.read(a2, times_buf, sizeof(times_buf));
                    times_ptr = times_buf;
                } catch (...) {
                    // Bad guest pointer — return EFAULT.
                    ret_err(EFAULT);
                    return 0;
                }
            }
            int r = ::utimensat(static_cast<int>(a0),
                                a1 ? path.c_str() : nullptr,
                                times_ptr,
                                static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 37: { // linkat(olddirfd, oldpath, newdirfd, newpath, flags) — AArch64 37
            // AArch64 syscall 37 is linkat, NOT unlink (there is no legacy
            // unlink on AArch64 — only unlinkat at syscall 35). The old
            // code dispatched 37 to unlink(), which broke `ln` (toybox
            // calls linkat() via musl's link() wrapper). unlink was being
            // called with olddirfd (AT_FDCWD=-100) as a path pointer,
            // returning ENOENT.
            std::string oldp = VFS::remap_path(VFS::read_path(mem_, a1));
            std::string newp = VFS::remap_path(VFS::read_path(mem_, a3));
            int r = ::linkat(static_cast<int>(a0), oldp.c_str(),
                             static_cast<int>(a2), newp.c_str(),
                             static_cast<int>(a4));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 39: { // umount2(target, flags) — AArch64 39
            // AArch64 syscall 39 is umount2, NOT symlink (which is
            // symlinkat at syscall 36). The old code dispatched 39 to
            // symlink(), but musl's symlink() wrapper calls syscall 36
            // (symlinkat). symlinkat is now correctly handled in misc.cpp.
            std::string target = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::umount2(target.c_str(), static_cast<int>(a1));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }

        case 41: { // pivot_root(new_root, put_old) — AArch64 41
            // AArch64 syscall 41 is pivot_root, NOT link (which is
            // linkat at syscall 37). The old code dispatched 41 to
            // link(), but musl's link() wrapper calls syscall 37
            // (linkat). linkat is now correctly handled above.
            // pivot_root is rarely used by user-space programs; return
            // EPERM (requires CAP_SYS_ADMIN).
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
            return 0;
        }

        case 45: { // truncate(path, length) — AArch64 45
            std::string path = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::truncate(path.c_str(), (off_t)a1);
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

        case 40: { // mount(source, target, fstype, flags, data) — AArch64 40
            // BUGFIX: previously implemented as sendfile (which is at 71,
            // already handled in misc.cpp). The old code dereferenced
            // `fstype` (a string pointer) as an `off_t*` and passed
            // `source`/`target` (string pointers) as fds to ::sendfile.
            // We don't support mount; return -ENOSYS (or -EPERM).
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
            return 0;
        }

        case 17: { // getcwd(buf, size) — AArch64 syscall 17
            // BUGFIX: the old code always returned "/" regardless of
            // chdir() calls. Now we track the guest cwd in the VFS
            // (updated by chdir/fchdir) and return the real path here.
            // The host cwd is meaningless because BIFROST_ROOT sandboxing
            // decouples them.
            std::string cwd = emu.vfs_.get_cwd();
            size_t need = cwd.size() + 1;  // include NUL terminator
            if (a1 < need) { ret_err(ERANGE); return 0; }
            try {
                mem_.write(a0, cwd.data(), need);
            } catch (...) { ret_err(EFAULT); return 0; }
            ret_host(static_cast<uint64_t>(cwd.size()));
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
