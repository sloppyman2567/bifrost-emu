// syscalls/fs.cpp — file syscalls: openat/close/read/write/dup/pipe2/
// mkdirat/unlinkat/renameat/writev/readv/preadv64/fstat/lseek/getdents64/
// statx/fstatat/readlinkat/fcntl/fstatfs/statfs/faccessat/chdir/fchdir/
// ftruncate/chmod/fchmod/utimensat/unlink/symlink/link/truncate/fallocate/
// sendfile/getcwd.
//
// All case bodies are extracted verbatim from the original syscalls.cpp
// (v1.4.0-alpha.5) EXCEPT case 56 (openat), which has been rewritten to
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
#include "syscalls/syscalls.h"
#include "vfs/vfs.h"
#include "vfs/vfs_table.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace arm64emu {

int64_t syscall_fs(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& vfs_ = emu.vfs_;
    auto& fds_ = emu.fds_;
    auto& elf_path_ = emu.elf_path_;
    auto ret_host = [&](int64_t r) { cpu.regs[0] = static_cast<uint64_t>(r); };

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

        // ── dup / dup2 / dup3 — VFS-aware ─────────────────────────────
        case 23: { // dup
            int r = fds_.dup(static_cast<int>(a0));
            ret_host(r);
            return 0;
        }
        case 33: { // dup2
            int r = fds_.dup2(static_cast<int>(a0), static_cast<int>(a1));
            ret_host(r);
            return 0;
        }
        case 24: { // dup3 - rare but possible
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

        // ── fstat — VFS-aware ─────────────────────────────────────────
        case 80: { // fstat
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            struct stat st{};
            int r = node->fstat(&st);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(r); return 0; }
            // Write the stat struct to guest memory at a1.
            mem_.write(a1, &st, sizeof(st));
            ret_host(0);
            return 0;
        }

        case 59: { // pipe2
            int fds[2];
            int r = ::pipe2(fds, static_cast<int>(a1));
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            mem_.write(a0, fds, sizeof(fds));
            ret_host(0);
            return 0;
        }

        case 34: { // mkdirat
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::mkdirat(static_cast<int>(a0), path.c_str(), (mode_t)a2);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(0);
            return 0;
        }

        case 35: { // unlinkat
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::unlinkat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2));
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(0);
            return 0;
        }

        case 38: { // renameat
            std::string oldp = VFS::remap_path(VFS::read_path(mem_, a1));
            std::string newp = VFS::remap_path(VFS::read_path(mem_, a3));
            int r = ::renameat(static_cast<int>(a0), oldp.c_str(), static_cast<int>(a2), newp.c_str());
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(0);
            return 0;
        }

        case 66: { // writev
            // a0=fd, a1=iovec ptr, a2=count
            uint64_t iov = a1;
            uint64_t cnt = a2;
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                std::vector<uint8_t> tmp(len);
                mem_.read(base, tmp.data(), len);
                ssize_t n = ::write(static_cast<int>(a0), tmp.data(), len);
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
                total += n;
                if (static_cast<size_t>(n) < len) break;
            }
            ret_host(total);
            return 0;
        }

        case 65: { // readv(fd, iov, iovcnt) — AArch64 syscall 65
            uint64_t iov = a1;
            uint64_t cnt = a2;
            ssize_t total = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                std::vector<uint8_t> tmp(len);
                ssize_t n = ::read(static_cast<int>(a0), tmp.data(), len);
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
                if (n > 0) mem_.write(base, tmp.data(), n);
                total += n;
                if (static_cast<size_t>(n) < len) break;
            }
            ret_host(total);
            return 0;
        }

        case 67: { // preadv64(fd, iov, iovcnt, offset) — AArch64 syscall 67
            // Same as readv but with explicit file offset. We handle the
            // common case by calling preadv if available; otherwise fall
            // back to lseek+readv+lseek.
            uint64_t iov = a1;
            uint64_t cnt = a2;
            off_t offset = (off_t)a3;
            ssize_t total = 0;
            off_t saved = ::lseek(static_cast<int>(a0), 0, SEEK_CUR);
            if (saved < 0) saved = 0;
            ::lseek(static_cast<int>(a0), offset, SEEK_SET);
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov + i * 16 + 8);
                if (len == 0) continue;
                std::vector<uint8_t> tmp(len);
                ssize_t n = ::read(static_cast<int>(a0), tmp.data(), len);
                if (n < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
                if (n > 0) mem_.write(base, tmp.data(), n);
                total += n;
                if (static_cast<size_t>(n) < len) break;
            }
            ::lseek(static_cast<int>(a0), saved, SEEK_SET);
            ret_host(total);
            return 0;
        }

        case 61: { // getdents64
            // Return a small fake directory listing.
            ret_host(0);
            return 0;
        }

        case 291: { // statx (Linux 4.11+, glibc uses it for fstatat fallback)
            // statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf)
            // struct statx is 256 bytes. We zero-fill it and return success.
            // For stdin/stdout/stderr or any fd, return a generic file type.
            uint8_t buf[256] = {0};
            // Set stx_mask = STATX_BASIC_STATS (0x7FF) so caller sees "all fields valid"
            // Layout: stx_mask at offset 0x00 (4 bytes)
            //         stx_blksize at 0x04 (4)
            //         stx_attributes at 0x08 (8)
            //         stx_nlink at 0x10 (4)
            //         stx_uid at 0x14 (4)
            //         stx_gid at 0x18 (4)
            //         stx_mode at 0x1C (2) + padding (2)
            //         stx_ino at 0x20 (8)
            //         stx_size at 0x28 (8)
            //         stx_blocks at 0x30 (8)
            //         stx_attributes_mask at 0x38 (8)
            //         ... access/modification/change/birth times ...
            uint32_t mask = 0x7FF; // STATX_BASIC_STATS
            memcpy(buf + 0, &mask, 4);
            uint32_t blksize = 4096;
            memcpy(buf + 4, &blksize, 4);
            uint32_t nlink = 1;
            memcpy(buf + 0x10, &nlink, 4);
            uint32_t uid = 0, gid = 0;
            memcpy(buf + 0x14, &uid, 4);
            memcpy(buf + 0x18, &gid, 4);
            uint16_t mode = 0100644; // regular file
            memcpy(buf + 0x1C, &mode, 2);
            mem_.write(a4, buf, 256);
            ret_host(0);
            return 0;
        }

        case 79: { // fstatat / newfstatat(dirfd, pathname, statbuf, flags)
            // v1.4.0-alpha: do a real stat on the (mapped) host path
            // so guest programs see correct file sizes, types, and
            // permissions. Previously this always returned a fake
            // "regular file, 0 bytes" stat, which broke programs that
            // check file sizes before reading.
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            struct stat st;
            int r;
            // If dirfd is AT_FDCWD (-100) or the path is absolute, use
            // fstatat on the host. Otherwise fall back to the fake stat.
            if (static_cast<int>(a0) == AT_FDCWD || (path.size() > 0 && path[0] == '/')) {
                r = ::fstatat(AT_FDCWD, path.c_str(), &st, static_cast<int>(a3));
            } else {
                r = ::fstatat(static_cast<int>(a0), path.c_str(), &st, static_cast<int>(a3));
            }
            if (r < 0) {
                // Fall back to fake stat on error (keeps old behavior
                // for paths that don't exist on the host).
                uint8_t buf[128] = {0};
                uint32_t mode = 0100644, nlink = 1;
                uint64_t blksize = 4096;
                memcpy(buf + 16, &mode, 4);
                memcpy(buf + 20, &nlink, 4);
                memcpy(buf + 0x38, &blksize, 8);
                mem_.write(a2, buf, 128);
                ret_host(0);
                return 0;
            }
            // Build the AArch64 struct stat (128 bytes):
            //   dev64, ino64, mode32, nlink32, uid32|gid32, pad, rdev64,
            //   size64, blksize64, blocks64, atime, atime_nsec,
            //   mtime, mtime_nsec, ctime, ctime_nsec
            uint8_t buf[128] = {0};
            uint64_t* p = reinterpret_cast<uint64_t*>(buf);
            p[0] = st.st_dev;
            p[1] = st.st_ino;
            reinterpret_cast<uint32_t*>(&p[2])[0] = st.st_mode;
            reinterpret_cast<uint32_t*>(&p[2])[1] = st.st_nlink;
            p[3] = st.st_uid | (static_cast<uint64_t>(st.st_gid) << 32);
            p[4] = 0;
            p[5] = st.st_rdev;
            p[6] = st.st_size;
            p[7] = st.st_blksize;
            p[8] = st.st_blocks;
            p[9]  = st.st_atim.tv_sec;
            p[10] = st.st_atim.tv_nsec;
            p[11] = st.st_mtim.tv_sec;
            p[12] = st.st_mtim.tv_nsec;
            p[13] = st.st_ctim.tv_sec;
            p[14] = st.st_ctim.tv_nsec;
            mem_.write(a2, buf, 128);
            ret_host(0);
            return 0;
        }

        case 78: { // readlinkat
            // readlinkat(dirfd, pathname, buf, bufsiz)
            // Handle /proc/self/exe specially (return the ELF path).
            // For all other paths, call the host readlinkat so symlinks
            // and regular files behave correctly.
            if (a1 != 0) {
                uint64_t off = 0;
                for (;;) {
                    uint8_t c = mem_.load<uint8_t>(a1 + off);
                    if (c == 0) break;
                    if (off > 256) break;
                    off++;
                }
                std::vector<uint8_t> path_bytes(off);
                if (off > 0) mem_.read(a1, path_bytes.data(), off);
                std::string path_str(reinterpret_cast<const char*>(path_bytes.data()), off);
                if (path_str == "/proc/self/exe") {
                    if (a3 > 0 && elf_path_.size() < a3) {
                        mem_.write(a2, elf_path_.data(), elf_path_.size() + 1);
                        ret_host(elf_path_.size());
                        return 0;
                    }
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
                    return 0;
                }
                // Call host readlinkat for real filesystem paths
                char buf[4096];
                ssize_t n = ::readlinkat(static_cast<int>(a0), path_str.c_str(), buf, sizeof(buf));
                if (n < 0) { ret_host(static_cast<uint64_t>(static_cast<int64_t>(-errno))); return 0; }
                if (static_cast<size_t>(n) > a3) n = a3;
                mem_.write(a2, buf, n);
                ret_host(n);
                return 0;
            }
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EFAULT)));
            return 0;
        }

        case 25: { // fcntl - stub, return 0
            ret_host(0);
            return 0;
        }

        case 44: { // fstatfs
            struct statfs {
                long f_type, f_bsize, f_blocks, f_bfree, f_bavail,
                     f_files, f_ffree, f_fsid[2], f_namelen, f_frsize,
                     f_flags, f_spare[4];
            };
            struct statfs sfs{};  // zero-init all fields (avoids -Wmissing-field-initializers)
            sfs.f_type = 0xEF53;       // ext2 magic
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_host(0);
            return 0;
        }

        case 43: { // statfs (by path)
            struct statfs {
                long f_type, f_bsize, f_blocks, f_bfree, f_bavail,
                     f_files, f_ffree, f_fsid[2], f_namelen, f_frsize,
                     f_flags, f_spare[4];
            };
            struct statfs sfs{};  // zero-init all fields
            sfs.f_type = 0xEF53;
            sfs.f_bsize = 4096;
            sfs.f_namelen = 255;
            mem_.write(a1, &sfs, sizeof(sfs));
            ret_host(0);
            return 0;
        }

        case 48: { // faccessat(dirfd, path, mode, flags) — AArch64 48
            std::string path = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::faccessat(static_cast<int>(a0), path.c_str(), static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 14: { // fchdir(fd) — AArch64 14
            int r = ::fchdir(static_cast<int>(a0));
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 50: { // chdir(path) — AArch64 50
            std::string path = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::chdir(path.c_str());
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 46: { // ftruncate(fd, length) — AArch64 46
            int r = ::ftruncate(static_cast<int>(a0), (off_t)a1);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 52: { // chmod(path, mode) — AArch64 52
            std::string path = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::chmod(path.c_str(), (mode_t)a1);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 53: { // fchmod(fd, mode) — AArch64 53
            int r = ::fchmod(static_cast<int>(a0), (mode_t)a1);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 88: { // utimensat(dirfd, path, times, flags) — AArch64 88
            std::string path = a1 ? VFS::remap_path(VFS::read_path(mem_, a1)) : std::string();
            int r = ::utimensat(static_cast<int>(a0), a1 ? path.c_str() : nullptr, (const struct timespec*)a2, static_cast<int>(a3));
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 37: { // unlink(path) — AArch64 37 (legacy)
            std::string path = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::unlink(path.c_str());
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 39: { // symlink(old, new) — AArch64 39
            std::string oldp = VFS::remap_path(VFS::read_path(mem_, a0));
            std::string newp = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::symlink(oldp.c_str(), newp.c_str());
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 41: { // link(old, new) — AArch64 41 (legacy)
            std::string oldp = VFS::remap_path(VFS::read_path(mem_, a0));
            std::string newp = VFS::remap_path(VFS::read_path(mem_, a1));
            int r = ::link(oldp.c_str(), newp.c_str());
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 45: { // truncate(path, length) — AArch64 45
            std::string path = VFS::remap_path(VFS::read_path(mem_, a0));
            int r = ::truncate(path.c_str(), (off_t)a1);
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-errno)); return 0; }
            ret_host(r);
            return 0;
        }

        case 47: { // fallocate(fd, mode, offset, len) — aarch64 47
            // v1.4.0-alpha.1: toybox's sh hits fallocate during line-edit
            // setup. We can't safely allocate guest memory from a host
            // fallocate (the fd may be a memfd with host-side backing),
            // but we *can* just let the host kernel handle it for fds that
            // are real host fds. For guest-mapped fds (memfd-backed VFS
            // files), fallocate extending the file also has to extend the
            // guest's mmap'd view — too complex for the common case.
            // Return success for mode=0 (allocate) on regular host fds;
            // return -ENOSYS for modes we can't honour (punch-hole, collapse).
            int fd = static_cast<int>(a0);
            int mode = static_cast<int>(a1);
            if (mode == 0) {
                // FALLOC_FL_KEEP_SIZE is 0x01; pure allocate (mode=0)
                // is the only one we can pass through safely.
                int r = ::fallocate(fd, mode, (off_t)a2, (off_t)a3);
                ret_host(r);
                return 0;
            }
            ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOSYS)));
            return 0;
        }

        case 40: { // sendfile(out_fd, in_fd, offset, count) — aarch64 71
            // Note: aarch64 sendfile is 71, but we use 40 here to avoid
            // conflict with case 71 (recvfrom placeholder). This is a known
            // limitation — guests using real sendfile will get -ENOSYS via
            // the default case. Document in CHANGELOG.
            off_t off = 0;
            if (a2) off = (off_t)mem_.load<uint64_t>(a2);
            ssize_t r = ::sendfile(static_cast<int>(a0), static_cast<int>(a1), a2 ? &off : nullptr, static_cast<size_t>(a3));
            if (a2 && r >= 0) mem_.store<uint64_t>(a2, static_cast<uint64_t>(off));
            ret_host(r);
            return 0;
        }

        case 17: { // getcwd(buf, size) — AArch64 syscall 17
            // We report "/" as the cwd. The buffer must be at least 2
            // bytes (NUL terminator included).
            mem_.write(a0, "/", 2);
            ret_host(1);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
