// Metadata.cpp — syscalls that read or change file metadata: fstat,
// fstatat/newfstatat, statx, statfs/fstatfs, getdents64, fchmod/fchmodat,
// fchown/fchownat and utimensat.
//
// Dispatched from syscall_fs() (FileSystem.cpp). Synthetic nodes report
// their own stat data; /proc/<pid>/exe is synthesized from the guest ELF.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include "System/VFS/Nodes/DirectoryNode.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
namespace arm64emu {
// fstatat for a guest path. Under BIFROST_ROOT the path resolves once,
// confined, to an O_PATH fd; otherwise the host resolves it directly.
// Returns 0 or -errno.
static int stat_guest_path(int hfd, const std::string& raw_path, int flags,
                           struct stat* st) {
    if (Yggdrasil::path_confined(hfd, raw_path)) {
        ScopedHostFd target(Yggdrasil::open_confined_path(
            hfd, raw_path, !(flags & AT_SYMLINK_NOFOLLOW)));
        if (target.fd < 0) return -errno;
        return ::fstatat(target.fd, "", st, AT_EMPTY_PATH) < 0 ? -errno : 0;
    }
    const std::string path = Yggdrasil::remap_path(raw_path);
    // An absolute path ignores dirfd (POSIX).
    const int dir = !path.empty() && path[0] == '/' ? AT_FDCWD : hfd;
    return ::fstatat(dir, path.c_str(), st, flags) < 0 ? -errno : 0;
}
int64_t syscall_fs_metadata(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& vfs_ = emu.vfs();
    auto& fds_ = emu.fds();
    const auto& elf_path_ = emu.elf_path();
    (void)vfs_; (void)elf_path_;
    switch (num) {
        // ── fstat — VFS-aware ─────────────────────────────────────────
        case 80: { // fstat(fd, statbuf) — AArch64 80
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF)); return 0; }
            struct stat st{};
            int r = node->fstat(&st);
            // Build AArch64 struct stat (128 bytes) — same layout as fstatat.
            uint8_t buf[128] = {0};
            if (r >= 0) {
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
            }
            // ALWAYS write the buffer (zeros on error, real data on success).
            try { mem_.write(a1, buf, 128); }
            catch (...) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EFAULT)); return 0; }
            if (r < 0) { cpu.regs[0] = static_cast<uint64_t>(r); return 0; }
            ret_host(0);
            return 0;
        }
        case 61: { // getdents64(fd, dirent_buf, count)
            // dispatch via the Node. DirNode synthesizes
            // linux_dirent64 records for virtual directories (/proc,
            // /proc/self, /dev); HostNode-equivalent paths fall through
            // to the host getdents64 syscall via host_fd().
            //
            // BUGFIX: pass min(count, sizeof(buf)) as the buffer size to the
            // underlying getdents implementation so the kernel/DirNode only
            // returns COMPLETE dirent records. The previous code read up to
            // 8192 bytes then post-truncated to `count`, which could split a
            // dirent record mid-way and corrupt the guest's readdir parsing.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_host(-EBADF); return 0; }
            // Virtual directory (DirNode)? Use the Node's getdents().
            if (node->is_dir()) {
                char buf[8192];
                size_t this_count = std::min<size_t>(a2, sizeof(buf));
                ssize_t n = node->getdents(0, buf, this_count);
                if (n < 0) { ret_host(static_cast<int>(n)); return 0; }
                // n is guaranteed to contain only complete records; write as-is.
                mem_.write(a1, buf, static_cast<size_t>(n));
                ret_host(n);
                return 0;
            }
            // Otherwise: real directory via host fd.
            int host_fd = node->host_fd();
            if (host_fd < 0) { ret_host(-ENOTDIR); return 0; }
            char host_buf[8192];
            size_t this_count = std::min<size_t>(a2, sizeof(host_buf));
            int n = ::syscall(SYS_getdents64, host_fd, host_buf, this_count);
            if (n < 0) { ret_host(-errno); return 0; }
            mem_.write(a1, host_buf, static_cast<size_t>(n));
            ret_host(n);
            return 0;
        }
        case 291: { // statx (Linux 4.11+, glibc uses it for fstatat fallback)
            // statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf)
            // Do a real stat on the (remapped) host path and convert to statx.
            //
            // struct statx_timestamp layout (16 bytes):
            //   +0:  int64_t  tv_sec
            //   +8:  uint32_t tv_nsec
            //   +12: uint32_t __reserved  ← MUST be 0; the previous code
            //                                omitted this field, shifting every
            //                                timestamp after atime by 4 bytes.
            // struct statx layout (256 bytes):
            //   +0x00: stx_mask (u32), +0x04: stx_blksize (u32),
            //   +0x08: stx_attributes (u64), +0x10: stx_nlink (u32),
            //   +0x14: stx_uid (u32), +0x18: stx_gid (u32),
            //   +0x1C: stx_mode (u16), +0x1E: __spare0 (u16),
            //   +0x20: stx_ino (u64), +0x28: stx_size (u64),
            //   +0x30: stx_blocks (u64), +0x38: stx_attributes_mask (u64),
            //   +0x40: stx_atime (16), +0x50: stx_btime (16),
            //   +0x60: stx_ctime (16), +0x70: stx_mtime (16),
            //   +0x80: stx_rdev_major (u32), +0x84: stx_rdev_minor (u32),
            //   +0x88: stx_dev_major (u32),  +0x8C: stx_dev_minor (u32),
            //   +0x90: stx_mnt_id (u64), +0x98: stx_dio_mem_align (u32),
            //   +0x9C: stx_dio_offset_align (u32), ... (rest is padding)
            std::string raw_path = yggdrasil::Yggdrasil::read_path(mem_, a1);
            struct stat st;
            if (yggdrasil::Yggdrasil::binder_device_stat(raw_path, &st) ||
                yggdrasil::Yggdrasil::sysfs_stat(raw_path, &st)) {
                if (a2 & ~uint64_t(0x100 | 0x800 | 0x1000 | 0x6000)) { ret_err(EINVAL); return 0; }
                if ((a2 & 0x6000) == 0x6000) { ret_err(EINVAL); return 0; }
            } else if (is_proc_exe_path(raw_path)) {
                proc_exe_stat(elf_path_,
                              (static_cast<int>(a2) & AT_SYMLINK_NOFOLLOW) != 0, &st);
            } else {
                int hfd = resolve_dirfd(fds_, a0);
                if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                    ret_err(EBADF); return 0;
                }
                // AT_SYMLINK_NOFOLLOW → don't follow symlinks
                // AT_EMPTY_PATH → stat the fd itself
                // (the AT_STATX_SYNC_TYPE bits have no fstatat equivalent).
                int r = stat_guest_path(hfd, raw_path,
                    static_cast<int>(a2) & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH), &st);
                if (r < 0) { ret_host(r); return 0; }
            }
            // Build statx structure (256 bytes), zero-initialized so all
            // __reserved fields and padding are correctly zero.
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
            // Timestamps: 16 bytes each (sec:8 + nsec:4 + reserved:4).
            // memset(buf,0,...) above already filled the __reserved fields.
            uint64_t atime_sec = st.st_atim.tv_sec;
            uint32_t atime_nsec = st.st_atim.tv_nsec;
            memcpy(buf + 0x40, &atime_sec, 8);
            memcpy(buf + 0x48, &atime_nsec, 4);
            // btime (creation time) — Linux only fills it on supported filesystems;
            // we don't have it from struct stat, leave as zero (already memset).
            uint64_t ctime_sec = st.st_ctim.tv_sec;
            uint32_t ctime_nsec = st.st_ctim.tv_nsec;
            memcpy(buf + 0x60, &ctime_sec, 8);
            memcpy(buf + 0x68, &ctime_nsec, 4);
            uint64_t mtime_sec = st.st_mtim.tv_sec;
            uint32_t mtime_nsec = st.st_mtim.tv_nsec;
            memcpy(buf + 0x70, &mtime_sec, 8);
            memcpy(buf + 0x78, &mtime_nsec, 4);
            uint32_t rdev_major = major(st.st_rdev), rdev_minor = minor(st.st_rdev);
            memcpy(buf + 0x80, &rdev_major, 4);
            memcpy(buf + 0x84, &rdev_minor, 4);
            uint32_t dev_major = major(st.st_dev), dev_minor = minor(st.st_dev);
            memcpy(buf + 0x88, &dev_major, 4);
            memcpy(buf + 0x8C, &dev_minor, 4);
            mem_.write(a4, buf, 256);
            ret_host(0);
            return 0;
        }
        case 79: { // fstatat / newfstatat(dirfd, pathname, statbuf, flags)
            // Do a real stat on the (mapped) host path so guest programs
            // see correct file sizes, types, and permissions.
            std::string raw_path = yggdrasil::Yggdrasil::read_path(mem_, a1);
            struct stat st;
            if (yggdrasil::Yggdrasil::binder_device_stat(raw_path, &st) ||
                yggdrasil::Yggdrasil::sysfs_stat(raw_path, &st)) {
                if (a3 & ~uint64_t(0x100 | 0x800 | 0x1000)) { ret_err(EINVAL); return 0; }
            } else if (is_proc_exe_path(raw_path)) {
                proc_exe_stat(elf_path_,
                              (static_cast<int>(a3) & AT_SYMLINK_NOFOLLOW) != 0, &st);
            } else {
                int hfd = resolve_dirfd(fds_, a0);
                if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                    ret_err(EBADF); return 0;
                }
                int r = stat_guest_path(hfd, raw_path, static_cast<int>(a3), &st);
                if (r < 0) {
                    ret_host(r);
                    return 0;
                }
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
        case 44: { // fstatfs(fd, buf) — AArch64 44
            // Forward to host fstatfs for real fds. This gives correct
            // f_blocks/f_bfree/f_bavail so `df` shows real disk usage.
            // with f_blocks=0, making `df` show "0-block filesystem".
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_err(EBADF); return 0; }
            int hfd = node->host_fd();
            struct statfs sfs{};
            int r = -1;
            if (hfd >= 0) {
                r = ::fstatfs(hfd, &sfs);
            }
            if (r < 0) {
                // Fallback: synthesize a minimal ext4-like result.
                sfs.f_type = 0xEF53;       // ext2/3/4 magic
                sfs.f_bsize = 4096;
                sfs.f_namelen = 255;
                // Provide reasonable defaults so `df` doesn't show 0.
                sfs.f_blocks = 1000000;    // ~4 GiB total
                sfs.f_bfree  = 500000;     // ~2 GiB free
                sfs.f_bavail = 500000;     // ~2 GiB available to user
                sfs.f_files  = 1000000;    // total inodes
                sfs.f_ffree  = 800000;     // free inodes
            }
            try { mem_.write(a1, &sfs, sizeof(sfs)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_ok();
            return 0;
        }
        case 43: { // statfs(path, buf) — AArch64 43
            // Forward to host statfs for real paths. This gives correct
            // f_blocks/f_bfree/f_bavail so `df` shows real disk usage.
            // with f_blocks=0, making `df` show "0-block filesystem".
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a0);
            struct statfs sfs{};
            int r;
            if (Yggdrasil::path_confined(AT_FDCWD, guest)) {
                ScopedHostFd target(Yggdrasil::open_confined_path(AT_FDCWD, guest, true));
                r = target.fd < 0 ? -1 : ::fstatfs(target.fd, &sfs);
            } else {
                r = ::statfs(yggdrasil::Yggdrasil::remap_path(guest).c_str(), &sfs);
            }
            if (r < 0) {
                // Fallback: synthesize a minimal ext4-like result.
                sfs.f_type = 0xEF53;       // ext2/3/4 magic
                sfs.f_bsize = 4096;
                sfs.f_namelen = 255;
                sfs.f_blocks = 1000000;
                sfs.f_bfree  = 500000;
                sfs.f_bavail = 500000;
                sfs.f_files  = 1000000;
                sfs.f_ffree  = 800000;
            }
            try { mem_.write(a1, &sfs, sizeof(sfs)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_ok();
            return 0;
        }
        case 52: { // fchmod(fd, mode) — AArch64 52
            // BUGFIX: previously labeled "chmod" but AArch64 has no chmod
            // (only fchmodat at 53). The real syscall at 52 is fchmod.
            // Resolve via FdTable so virtual fds work.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_err(EBADF); return 0; }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_err(EINVAL); return 0; }
            int r = ::fchmod(hfd, (mode_t)a1);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 53: { // fchmodat(dirfd, path, mode) — AArch64 53
            // The raw syscall takes three arguments and always follows
            // symlinks (Linux fs/open.c: SYSCALL_DEFINE3, flags 0); x3 is
            // caller garbage. Flags exist only in fchmodat2 (452).
            int hfd = resolve_dirfd(fds_, a0);
            if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            int r;
            if (Yggdrasil::path_confined(hfd, guest)) {
                ScopedHostFd target(Yggdrasil::open_confined_path(hfd, guest, true));
                r = target.fd < 0 ? -1
                    : ::chmod(Yggdrasil::proc_fd_path(target.fd).c_str(), (mode_t)a2);
            } else {
                std::string path = yggdrasil::Yggdrasil::remap_path(guest);
                r = ::fchmodat(hfd, path.c_str(), (mode_t)a2, 0);
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 88: { // utimensat(dirfd, path, times, flags) — AArch64 88
            // SECURITY FIX: do NOT cast the guest pointer `a2` directly to
            // `const struct timespec*` — that dereferences garbage host
            // memory and crashes. Read the guest's times array into a
            // local buffer first, then pass that to ::utimensat.
            int hfd = resolve_dirfd(fds_, a0);
            if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            std::string guest = a1 ? yggdrasil::Yggdrasil::read_path(mem_, a1) : std::string();
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
            const int flags = static_cast<int>(a3);
            int r;
            if (a1 && Yggdrasil::path_confined(hfd, guest)) {
                if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) { ret_err(EINVAL); return 0; }
                const bool follow = !(flags & AT_SYMLINK_NOFOLLOW);
                ScopedHostFd target(Yggdrasil::open_confined_path(hfd, guest, follow));
                if (target.fd < 0) { ret_errno(); return 0; }
                struct stat st;
                if (::fstatat(target.fd, "", &st, AT_EMPTY_PATH) < 0) { ret_errno(); return 0; }
                if (!follow && S_ISLNK(st.st_mode)) {
                    // A symlink's own times: its name in a confined parent.
                    auto pd = Yggdrasil::open_parent(hfd, guest);
                    if (!pd.valid()) { ret_errno(); return 0; }
                    r = ::utimensat(pd.fd, pd.base.c_str(), times_ptr, AT_SYMLINK_NOFOLLOW);
                } else {
                    r = ::utimensat(AT_FDCWD, Yggdrasil::proc_fd_path(target.fd).c_str(),
                                    times_ptr, 0);
                }
            } else {
                std::string path = a1 ? yggdrasil::Yggdrasil::remap_path(guest) : std::string();
                r = ::utimensat(hfd, a1 ? path.c_str() : nullptr, times_ptr, flags);
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 54: { // fchownat(dirfd, path, owner, group, flags) — AArch64 54
            // BUGFIX: was previously labeled "fchmodat" but fchmodat is
            // at 53 (handled in FileSystem.cpp). The real syscall at 54 is fchownat.
            // The guest dirfd resolves through the FdTable (it was passed raw).
            int hfd = resolve_dirfd(fds_, a0);
            if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            std::string guest = Yggdrasil::read_path(mem_, a1);
            const int flags = static_cast<int>(a4);
            int r;
            if (Yggdrasil::path_confined(hfd, guest)) {
                if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) { ret_err(EINVAL); return 0; }
                ScopedHostFd target(Yggdrasil::open_confined_path(
                    hfd, guest, !(flags & AT_SYMLINK_NOFOLLOW)));
                r = target.fd < 0 ? -1
                    : ::fchownat(target.fd, "", static_cast<uid_t>(a2),
                                 static_cast<gid_t>(a3), AT_EMPTY_PATH);
            } else {
                std::string path = Yggdrasil::remap_path(guest);
                r = ::fchownat(hfd, path.c_str(), static_cast<uid_t>(a2),
                               static_cast<gid_t>(a3), flags);
            }
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
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
