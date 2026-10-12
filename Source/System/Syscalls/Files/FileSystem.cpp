// FileSystem.cpp — file syscall dispatcher and the helpers shared by the
// file families in this directory:
//   - Descriptors.cpp — operations on an open fd (read/write/dup/fcntl/sync)
//   - Paths.cpp       — namespace operations (openat/mkdirat/rename/chdir)
//   - Metadata.cpp    — stat/statx/statfs/getdents64/chmod/chown/utimensat
//   - Transfer.cpp    — splice/vmsplice/tee/sendfile
//
// Many case bodies use the Yggdrasil VFS (Source/System/VFS/) for synthetic
// /proc and /dev files and host passthrough.
#include "System/Core/Emulator.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
namespace arm64emu {
// AArch64 overrides these four asm-generic open flag positions. VFS nodes
// receive host flags; translate at the syscall boundary, including fcntl.
int open_flags_to_host(int guest) {
    constexpr int directory = 0x4000, nofollow = 0x8000;
    constexpr int direct = 0x10000, largefile = 0x20000;
    int host = guest & ~(directory | nofollow | direct | largefile);
    if (guest & directory) host |= O_DIRECTORY;
    if (guest & nofollow) host |= O_NOFOLLOW;
    if (guest & direct) host |= O_DIRECT;
    if (guest & largefile) host |= O_LARGEFILE;
    return host;
}
int open_flags_to_guest(int host) {
    // x86_64's kernel reports the large-file bit even though libc's
    // O_LARGEFILE macro is zero for its 64-bit callers.
    constexpr int host_largefile = 0x8000;
    int guest = host & ~(O_DIRECTORY | O_NOFOLLOW | O_DIRECT | host_largefile);
    if (host & O_DIRECTORY) guest |= 0x4000;
    if (host & O_NOFOLLOW) guest |= 0x8000;
    if (host & O_DIRECT) guest |= 0x10000;
    if (host & host_largefile) guest |= 0x20000;
    return guest;
}
void xseq_append(char dir, const uint8_t* p, size_t n) {
    if (dbg().xseqlog.empty()) return;
    FILE* f = fopen(dbg().xseqlog.c_str(), "ab");
    if (!f) return;
    fwrite(&dir, 1, 1, f);
    uint32_t l = static_cast<uint32_t>(n);
    fwrite(&l, 1, 4, f);
    if (n) fwrite(p, 1, n, f);
    fclose(f);
}
// ── Helper: resolve a guest dirfd to a host dirfd ─────────────────────
// The guest passes a dirfd to *at syscalls (openat, fstatat, unlinkat, etc.).
// This can be:
//   - AT_FDCWD (-100): use the current working directory (host fd = AT_FDCWD)
//   - A guest fd from the FdTable: resolve to the underlying host fd
//   - An invalid fd: return -1 (caller should return -EBADF)
//
// to host ::mkdirat/::unlinkat/etc. The FdTable uses arbitrary indices
// (allocated starting at 3) that have NO relationship to host fds. When
// a guest program opened a directory and passed its dirfd to fstatat,
// the host received a meaningless fd number and either returned EBADF
// or, worse, operated on the wrong file. This broke find, tar, cp -r,
// rsync, Python os.scandir, and every other program that uses the
// POSIX *at API.
//
// This helper resolves the guest dirfd to a real host fd that can be
// passed to host *at syscalls. For virtual Nodes (memfd, /proc, /dev),
// there is no host fd — in that case we return -1 and the caller should
// fall back to a path-based approach or return -EOPNOTSUPP.
int resolve_dirfd(FdTable& fds, uint64_t guest_dirfd) {
    constexpr int AT_FDCWD_LINUX = -100;  // Linux AT_FDCWD value
    int dirfd = static_cast<int>(static_cast<int64_t>(guest_dirfd));
    if (dirfd == AT_FDCWD_LINUX) {
        return AT_FDCWD_LINUX;  // pass through to host
    }
    if (dirfd < 0) {
        return -1;  // invalid (other negative values are reserved)
    }
    auto node = fds.get(dirfd);
    if (!node) {
        return -1;  // EBADF
    }
    int hfd = node->host_fd();
    return hfd;  // may be -1 for virtual nodes
}
// ── Helper: is /proc/self/exe or /proc/<pid>/exe (guest view) ────────
// Qt resolves its own binary via /proc/<getpid()>/exe (the guest's
// getpid() returns 1, so the path is /proc/1/exe). The host kernel's
// /proc/1/exe belongs to a DIFFERENT process, so stat/lstat/readlink on
// these paths must be served synthetically from elf_path_ instead of
// leaking the host PID 1 (or failing with EACCES for unprivileged runs).
bool is_proc_exe_path(const std::string& p) {
    if (p == "/proc/self/exe" || p == "/proc/self/exe/") return true;
    if (p.compare(0, 6, "/proc/") != 0) return false;
    size_t slash = p.find('/', 6);
    if (slash == std::string::npos) return false;
    std::string pid = p.substr(6, slash - 6);
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string tail = p.substr(slash + 1);
    return tail == "exe" || tail == "exe/";
}
// Return -2 for unrelated paths, -1 for malformed descriptor names, otherwise
// the guest fd. Never pass a guest fd number directly to the host procfs.
// With `rest`, a trailing "/component..." after the number is accepted and
// returned there (opening through a directory descriptor's link).
int proc_link_fd(const std::string& path, std::string* rest) {
    for (const char* prefix : {"/proc/self/fd/", "/proc/thread-self/fd/"}) {
        size_t length = std::strlen(prefix);
        if (path.compare(0, length, prefix) != 0) continue;
        if (path.size() == length) return -1;
        int fd = 0;
        size_t i = length;
        for (; i < path.size() && path[i] != '/'; ++i) {
            char c = path[i];
            if (c < '0' || c > '9' || fd > (INT_MAX - (c - '0')) / 10) return -1;
            fd = fd * 10 + (c - '0');
        }
        if (i == length || (i < path.size() && !rest)) return -1;
        if (rest) *rest = path.substr(i);
        return fd;
    }
    return -2;
}
std::unique_ptr<yggdrasil::Node> open_guest_at(Emulator& emu, uint64_t guest_dirfd,
    const std::string& path, int guest_flags, mode_t guest_mode, int* err) {
    auto& vfs = emu.vfs();
    auto& fds = emu.fds();
    const int host_flags = open_flags_to_host(guest_flags);
    // Apply guest umask to the creation mode (kernel behavior).
    const mode_t mode = guest_mode & ~emu.guest_umask();
    if (!path.empty() && path[0] != '/') {
        // Relative path: honor an explicit dirfd. AT_FDCWD keeps the
        // legacy host-cwd behavior; anything else invalid is EBADF.
        int hfd = resolve_dirfd(fds, guest_dirfd);
        if (hfd == -100) return vfs.open(path, host_flags, mode, err);
        if (hfd < 0) { *err = -EBADF; return nullptr; }
        return vfs.open_at(hfd, path, host_flags, mode, err);
    }
    // Absolute path: dirfd is ignored per POSIX. /proc/<own pid> is /proc/self.
    std::string rest;
    const int proc_fd = proc_link_fd(vfs.canonical_proc_path(path), &rest);
    if (proc_fd == -2) return vfs.open(path, host_flags, mode, err);
    // /proc/self/fd/N reopens the file behind GUEST fd N. Host procfs
    // would name the emulator's own fd with that number instead.
    auto target = proc_fd >= 0 ? fds.get(proc_fd) : nullptr;
    if (!target) { *err = -ENOENT; return nullptr; }
    // Sockets and anonymous inodes cannot be reopened (ENXIO on Linux).
    if (target->host_fd() < 0) { *err = -ENXIO; return nullptr; }
    if (rest.empty()) {
        char link[48];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", target->host_fd());
        int fd = ::open(link, host_flags, mode);
        if (fd < 0) { *err = -errno; return nullptr; }
        return std::make_unique<yggdrasil::HostNode>(fd, host_flags);
    }
    // Below a directory descriptor: confined like a relative openat().
    const size_t start = rest.find_first_not_of('/');
    return vfs.open_at(target->host_fd(),
                       start == std::string::npos ? "." : rest.substr(start),
                       host_flags, mode, err);
}
// Fill a synthetic struct stat for the guest ELF as seen through the
// /proc/<pid>/exe symlink: lstat (AT_SYMLINK_NOFOLLOW) yields a symlink
// node, stat follows it and reflects the real ELF file on disk.
void proc_exe_stat(const std::string& elf_path, bool nofollow, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_uid = static_cast<uid_t>(::getuid());
    st->st_gid = static_cast<gid_t>(::getgid());
    if (nofollow) {
        st->st_mode = S_IFLNK | 0777;
        st->st_nlink = 1;
        st->st_size = static_cast<off_t>(elf_path.size());
    } else {
        struct stat hst;
        std::string host_elf = yggdrasil::Yggdrasil::remap_path(elf_path);
        if (::stat(host_elf.c_str(), &hst) == 0) {
            *st = hst;
            st->st_mode = S_IFREG | (st->st_mode & 07777);
        } else {
            st->st_mode = S_IFREG | 0755;
            st->st_nlink = 1;
        }
    }
}
int64_t syscall_fs(Emulator& emu, CPU& cpu, uint64_t num) {
    // Descriptor I/O is the hottest family, so it is tried first. Each
    // family owns a disjoint set of syscall numbers.
    if (syscall_fs_descriptors(emu, cpu, num) != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_fs_paths(emu, cpu, num)       != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_fs_metadata(emu, cpu, num)    != SYSCALL_NOT_HANDLED) return 0;
    if (syscall_fs_transfer(emu, cpu, num)    != SYSCALL_NOT_HANDLED) return 0;
    return SYSCALL_NOT_HANDLED;
}
} // namespace arm64emu
