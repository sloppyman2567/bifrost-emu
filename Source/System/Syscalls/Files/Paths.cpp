// Paths.cpp — syscalls that act on the guest file namespace: openat/openat2,
// mknodat/mkdirat, unlinkat, renameat/renameat2, linkat/symlinkat,
// readlinkat, faccessat/faccessat2, chdir/fchdir/getcwd, chroot, truncate,
// mount/umount2/pivot_root and nfsservctl.
//
// Dispatched from syscall_fs() (FileSystem.cpp). Guest dirfds are resolved
// with resolve_dirfd(); /proc/<pid>/exe and /proc/<pid>/fd links are served
// from the guest view rather than the host's procfs.
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
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
namespace arm64emu {
// faccessat2 for a guest path (flags: AT_EACCESS, AT_SYMLINK_NOFOLLOW,
// AT_EMPTY_PATH). Under BIFROST_ROOT the path resolves once, confined,
// and the check runs on that exact file through its /proc magic link.
// Returns 0 or -errno.
static int access_guest_path(int hfd, const std::string& guest, int mode, int flags) {
    if (Yggdrasil::path_confined(hfd, guest)) {
        if ((mode & ~7) || (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)))
            return -EINVAL;
        ScopedHostFd target(Yggdrasil::open_confined_path(
            hfd, guest, !(flags & AT_SYMLINK_NOFOLLOW)));
        if (target.fd < 0) return -errno;
        return ::faccessat(AT_FDCWD, Yggdrasil::proc_fd_path(target.fd).c_str(), mode,
                           flags & AT_EACCESS) < 0 ? -errno : 0;
    }
    const std::string path = Yggdrasil::remap_path(guest);
    // An absolute path ignores dirfd (POSIX).
    const int dir = !path.empty() && path[0] == '/' ? AT_FDCWD : hfd;
    return ::faccessat(dir, path.c_str(), mode, flags) < 0 ? -errno : 0;
}
int64_t syscall_fs_paths(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    auto& vfs_ = emu.vfs();
    auto& fds_ = emu.fds();
    const auto& elf_path_ = emu.elf_path();
    (void)vfs_; (void)elf_path_;
    switch (num) {
        // ── openat — REWRITTEN to use VFS ─────────────────────────────
        // The original 164-line inline /proc//dev/ chain is replaced by
        // a single yggdrasil::Yggdrasil::open() call. The VFS dispatches to procfs/devfs/
        // host passthrough internally.
        case 56: { // openat
            std::string path = yggdrasil::Yggdrasil::read_path(mem_, a1);
            int err = 0;
            auto node = open_guest_at(emu, a0, path, static_cast<int>(a2),
                                      static_cast<mode_t>(a3), &err);
            if (!node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(err != 0 ? err : -ENOENT));
                return 0;
            }
            // Adopt into the FdTable.
            int guest_fd = fds_.allocate(std::shared_ptr<yggdrasil::Node>(std::move(node)));
            if (dbg().xtrace)
                fprintf(stderr, "[FDLIFE] t%d openat gfd=%d path=%.60s\n",
                        cpu.tid, guest_fd, path.c_str());
            ret_host(guest_fd);
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
            // Same resolution as openat: guest flag translation, umask,
            // dirfd and /proc/self/fd. struct open_how's resolve field is
            // not modeled.
            auto node = open_guest_at(emu, a0, path, static_cast<int>(flags),
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
            struct stat device_stat;
            if (Yggdrasil::binder_device_stat(path, &device_stat) ||
                Yggdrasil::sysfs_stat(path, &device_stat)) {
                if ((a2 & ~uint64_t(7)) || (a3 & ~uint64_t(0x100 | 0x200 | 0x1000))) {
                    ret_err(EINVAL); return 0;
                }
                if (a2 & 1) { ret_err(EACCES); return 0; }
                ret_host(0); return 0;
            }
            // The guest dirfd resolves through the FdTable (it was passed raw).
            int hfd = resolve_dirfd(fds_, a0);
            if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            int r = access_guest_path(hfd, path, static_cast<int>(a2), static_cast<int>(a3));
            if (r < 0) { ret_host(r); return 0; }
            ret_host(0);
            return 0;
        }
        case 33: { // mknodat(dirfd, path, mode, dev) — AArch64 33
            // BUGFIX: previously implemented as dup2 (which doesn't exist
            // on AArch64). The real syscall at 33 is mknodat. Forward to
            // host mknodat.
            // Confined via open_parent: the parent traversal goes through
            // RESOLVE_IN_ROOT when sandboxed, so a symlink/.. in the path
            // cannot escape BIFROST_ROOT.
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            int hfd = resolve_dirfd(fds_, a0);
            if (!guest.empty() && guest[0] != '/' && hfd == -1 &&
                static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            auto pd = yggdrasil::Yggdrasil::open_parent(hfd, guest);
            if (!pd.valid()) { ret_errno(); return 0; }
            int r = ::mknodat(pd.fd, pd.base.c_str(),
                              static_cast<mode_t>(a2), static_cast<dev_t>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 34: { // mkdirat(dirfd, path, mode)
            // static_cast<int>(a0) which passed the guest fd index
            // directly to the host.
            // Confined via open_parent (see mknodat above).
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            int hfd = resolve_dirfd(fds_, a0);
            if (!guest.empty() && guest[0] != '/' && hfd == -1 &&
                static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            auto pd = yggdrasil::Yggdrasil::open_parent(hfd, guest);
            if (!pd.valid()) { ret_errno(); return 0; }
            mode_t mode = static_cast<mode_t>(a2) & ~emu.guest_umask();
            int r = ::mkdirat(pd.fd, pd.base.c_str(), mode);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 35: { // unlinkat(dirfd, path, flags)
            // Confined via open_parent (see mknodat above).
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            int hfd = resolve_dirfd(fds_, a0);
            if (!guest.empty() && guest[0] != '/' && hfd == -1 &&
                static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            auto pd = yggdrasil::Yggdrasil::open_parent(hfd, guest);
            if (!pd.valid()) { ret_errno(); return 0; }
            int r = ::unlinkat(pd.fd, pd.base.c_str(), static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 38: { // renameat(olddirfd, oldpath, newdirfd, newpath)
            // Both sides confined independently (either side could smuggle
            // a symlink/.. escape).
            std::string old_guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            std::string new_guest = yggdrasil::Yggdrasil::read_path(mem_, a3);
            int old_hfd = resolve_dirfd(fds_, a0);
            int new_hfd = resolve_dirfd(fds_, a2);
            if ((!old_guest.empty() && old_guest[0] != '/' && old_hfd == -1 &&
                 static_cast<int64_t>(a0) != -100) ||
                (!new_guest.empty() && new_guest[0] != '/' && new_hfd == -1 &&
                 static_cast<int64_t>(a2) != -100)) {
                ret_err(EBADF); return 0;
            }
            auto old_pd = yggdrasil::Yggdrasil::open_parent(old_hfd, old_guest);
            if (!old_pd.valid()) { ret_errno(); return 0; }
            auto new_pd = yggdrasil::Yggdrasil::open_parent(new_hfd, new_guest);
            if (!new_pd.valid()) { ret_errno(); return 0; }
            int r = ::renameat(old_pd.fd, old_pd.base.c_str(),
                               new_pd.fd, new_pd.base.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 276: { // renameat2(olddirfd, oldpath, newdirfd, newpath, flags)
            // Forward to host renameat2. Used for atomic file swap,
            // RENAME_NOREPLACE, RENAME_EXCHANGE. Both sides are confined
            // like renameat: rename never follows the final component.
            std::string old_guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            std::string new_guest = yggdrasil::Yggdrasil::read_path(mem_, a3);
            int old_hfd = resolve_dirfd(fds_, a0);
            int new_hfd = resolve_dirfd(fds_, a2);
            if ((!old_guest.empty() && old_guest[0] != '/' && old_hfd == -1 &&
                 static_cast<int64_t>(a0) != -100) ||
                (!new_guest.empty() && new_guest[0] != '/' && new_hfd == -1 &&
                 static_cast<int64_t>(a2) != -100)) {
                ret_err(EBADF); return 0;
            }
            auto old_pd = yggdrasil::Yggdrasil::open_parent(old_hfd, old_guest);
            if (!old_pd.valid()) { ret_errno(); return 0; }
            auto new_pd = yggdrasil::Yggdrasil::open_parent(new_hfd, new_guest);
            if (!new_pd.valid()) { ret_errno(); return 0; }
#ifdef SYS_renameat2
            int r = ::syscall(SYS_renameat2, old_pd.fd, old_pd.base.c_str(),
                              new_pd.fd, new_pd.base.c_str(),
                              static_cast<unsigned int>(a4));
#else
            // Fallback: plain renameat (no flags support)
            if (a4 != 0) { ret_err(ENOSYS); return 0; }
            int r = ::renameat(old_pd.fd, old_pd.base.c_str(),
                               new_pd.fd, new_pd.base.c_str());
#endif
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0);
            return 0;
        }
        case 37: { // linkat(olddirfd, oldpath, newdirfd, newpath, flags) — AArch64 37
            // AArch64 syscall 37 is linkat, NOT unlink (there is no legacy
            // unlink on AArch64 — only unlinkat at syscall 35). The old
            // code dispatched 37 to unlink(), which broke `ln` (toybox
            // calls linkat() via musl's link() wrapper). unlink was being
            // called with olddirfd (AT_FDCWD=-100) as a path pointer,
            // returning ENOENT.
            int old_hfd = resolve_dirfd(fds_, a0);
            int new_hfd = resolve_dirfd(fds_, a2);
            std::string old_guest = yggdrasil::Yggdrasil::read_path(mem_, a1);
            std::string new_guest = yggdrasil::Yggdrasil::read_path(mem_, a3);
            if ((!old_guest.empty() && old_guest[0] != '/' && old_hfd == -1 &&
                 static_cast<int64_t>(a0) != -100) ||
                (!new_guest.empty() && new_guest[0] != '/' && new_hfd == -1 &&
                 static_cast<int64_t>(a2) != -100)) {
                ret_err(EBADF); return 0;
            }
            // Both sides confined (see renameat above): the source lookup
            // and the link creation each resolve their parent confined.
            auto old_pd = yggdrasil::Yggdrasil::open_parent(old_hfd, old_guest);
            if (!old_pd.valid()) { ret_errno(); return 0; }
            auto new_pd = yggdrasil::Yggdrasil::open_parent(new_hfd, new_guest);
            if (!new_pd.valid()) { ret_errno(); return 0; }
            int r;
            if ((a4 & AT_SYMLINK_FOLLOW) &&
                yggdrasil::Yggdrasil::path_confined(old_hfd, old_guest)) {
                // A followed source symlink must resolve inside the root, or
                // the new hard link would expose an outside file. Link the
                // resolved file through its /proc magic link (linkat(2)).
                if (a4 & ~uint64_t(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) { ret_err(EINVAL); return 0; }
                ScopedHostFd source(yggdrasil::Yggdrasil::open_confined_path(old_hfd, old_guest, true));
                r = source.fd < 0 ? -1
                    : ::linkat(AT_FDCWD, yggdrasil::Yggdrasil::proc_fd_path(source.fd).c_str(),
                               new_pd.fd, new_pd.base.c_str(), AT_SYMLINK_FOLLOW);
            } else {
                r = ::linkat(old_pd.fd, old_pd.base.c_str(),
                             new_pd.fd, new_pd.base.c_str(),
                             static_cast<int>(a4));
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 36: { // symlinkat(old, newdirfd, new) — AArch64 36
            // AArch64 syscall 36 is symlinkat, NOT unlinkat (which is 35).
            // (toybox calls symlinkat() via musl). unlinkat is correctly
            // handled at syscall 35 in FileSystem.cpp.
            // The target (oldp) is stored verbatim — never resolved now —
            // so it needs no remap. The link CREATION (newp) is confined
            // via open_parent; the guest dirfd is resolved through FdTable
            // (it was previously passed raw to the host).
            std::string oldp = Yggdrasil::read_path(mem_, a0);
            std::string new_guest = Yggdrasil::read_path(mem_, a2);
            // Absolute newpath ignores dirfd per POSIX (mirror the FileSystem.cpp
            // convention: EBADF only for relative + bad dirfd).
            int hfd = AT_FDCWD;
            if (!new_guest.empty() && new_guest[0] != '/' &&
                static_cast<int64_t>(a1) != -100) {
                auto node = fds_.get(static_cast<int>(a1));
                if (!node) { ret_err(EBADF); return 0; }
                hfd = node->host_fd();
                if (hfd < 0) { ret_err(EBADF); return 0; }
            }
            auto pd = yggdrasil::Yggdrasil::open_parent(hfd, new_guest);
            if (!pd.valid()) { ret_errno(); return 0; }
            int r = ::symlinkat(oldp.c_str(), pd.fd, pd.base.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 78: { // readlinkat(dirfd, pathname, buf, bufsiz) — AArch64 78
            // Handle /proc/self/exe specially (return the ELF path).
            // For all other paths, call the host readlinkat so symlinks
            // and regular files behave correctly.
            if (a1 != 0) {
                if (a3 == 0) { ret_err(EINVAL); return 0; }
                // Read the path string from guest memory, bounded by
                // PATH_MAX (4096 incl nul). Reuse the same limit as
                // Yggdrasil::read_path so long paths don't truncate.
                constexpr size_t MAX_PATH_SCAN = 4096;
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
                std::string path_str(path_bytes.begin(), path_bytes.end());
                if (path_bytes.size() == MAX_PATH_SCAN) { ret_err(ENAMETOOLONG); return 0; }
                std::string proc_path = path_str;
                if (!path_str.empty() && path_str[0] != '/' && static_cast<int>(a0) != -100) {
                    auto directory = std::dynamic_pointer_cast<yggdrasil::DirNode>(
                        fds_.get(static_cast<int>(a0)));
                    if (directory) proc_path = directory->name() + "/" + path_str;
                } else if (!path_str.empty() && path_str[0] != '/') {
                    proc_path = vfs_.get_cwd() + "/" + path_str;
                }
                int proc_fd = proc_link_fd(vfs_.canonical_proc_path(proc_path));
                if (proc_fd != -2) {
                    auto node = proc_fd >= 0 ? fds_.get(proc_fd) : nullptr;
                    if (!node || node->host_fd() < 0) { ret_err(ENOENT); return 0; }
                    std::string target;
                    int error = yggdrasil::Yggdrasil::fd_link_target(node->host_fd(), target);
                    if (error) { ret_err(-error); return 0; }
                    size_t count = std::min<size_t>(a3, target.size());
                    try { mem_.write(a2, target.data(), count); }
                    catch (...) { ret_err(EFAULT); return 0; }
                    ret_host(count);
                    return 0;
                }
                if (is_proc_exe_path(path_str)) {
                    if (a3 == 0) { ret_err(EINVAL); return 0; }
                    // readlink truncates to bufsiz and never appends a NUL.
                    const size_t count = std::min<size_t>(a3, elf_path_.size());
                    try { mem_.write(a2, elf_path_.data(), count); }
                    catch (...) { ret_err(EFAULT); return 0; }
                    ret_host(count);
                    return 0;
                }
                // Call host readlinkat for real filesystem paths.
                int hfd = resolve_dirfd(fds_, a0);
                if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                    ret_err(EBADF); return 0;
                }
                // Remap through BIFROST_ROOT: glibc realpath() walks a path
                // by calling readlink() on each accumulated component, and
                // the host must see the sandboxed path or it reports ENOENT.
                // Under the sandbox the link itself resolves confined
                // (O_NOFOLLOW) and is read through its O_PATH fd.
                char buf[4096];
                ssize_t n;
                if (yggdrasil::Yggdrasil::path_confined(hfd, path_str)) {
                    ScopedHostFd link(yggdrasil::Yggdrasil::open_confined_path(hfd, path_str, false));
                    n = link.fd < 0 ? -1 : ::readlinkat(link.fd, "", buf, sizeof(buf));
                } else {
                    std::string host_path = yggdrasil::Yggdrasil::remap_path(path_str);
                    n = ::readlinkat(hfd, host_path.c_str(), buf, sizeof(buf));
                }
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
        case 48: { // faccessat(dirfd, path, mode, flags) — AArch64 48
            // BUGFIX: the RAW faccessat syscall takes only 3 arguments
            // (dfd, path, mode); the kernel ignores the 4th (flags).
            // glibc's access()/faccessat() wrappers only set x0-x2+x8, so
            // x3 holds caller garbage. Forwarding it to the host faccessat
            // as `flags` made every access() fail with EINVAL, breaking
            // fontconfig's FcConfigFilename existence checks.
            std::string raw_path = yggdrasil::Yggdrasil::read_path(mem_, a1);
            struct stat device_stat;
            if (yggdrasil::Yggdrasil::binder_device_stat(raw_path, &device_stat) ||
                yggdrasil::Yggdrasil::sysfs_stat(raw_path, &device_stat)) {
                if (a2 & ~uint64_t(7)) { ret_err(EINVAL); return 0; }
                if (a2 & 1) { ret_err(EACCES); return 0; }
                ret_host(0); return 0;
            }
            int hfd = resolve_dirfd(fds_, a0);
            if (hfd == -1 && static_cast<int64_t>(a0) != -100) {
                ret_err(EBADF); return 0;
            }
            int r = access_guest_path(hfd, raw_path, static_cast<int>(a2), 0);
            if (r < 0) { ret_host(r); return 0; }
            ret_host(r);
            return 0;
        }
        case 50: { // fchdir(fd) — AArch64 50
            // BUGFIX: previously called ::fchdir(guest_fd, ...) directly,
            // bypassing FdTable. Resolve via FdTable so virtual fds work.
            // return EBADF instead of passing the guest fd index to the host.
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) { ret_err(EBADF); return 0; }
            int hfd = node->host_fd();
            if (hfd < 0) { ret_err(EBADF); return 0; }
            // Confine like chdir(49): a smuggled outside dirfd (opened via
            // a legacy AT_FDCWD-relative open while the host cwd was
            // outside the root) must not move the host cwd out of the
            // sandbox. Save cwd, verify after, roll back on escape.
            int saved = -1;
            bool check = yggdrasil::Yggdrasil::sandbox_inside_root();
            if (check)
                saved = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            int r = ::fchdir(hfd);
            if (r < 0) {
                if (saved >= 0) ::close(saved);
                ret_errno(); return 0;
            }
            if (check) {
                char cwd[PATH_MAX];
                bool verified =
                    ::getcwd(cwd, sizeof(cwd)) &&
                    yggdrasil::Yggdrasil::path_inside_root(cwd);
                if (!verified) {
                    if (saved >= 0) {
                        ::fchdir(saved);
                        ::close(saved);
                    }
                    ret_err(EACCES);
                    return 0;
                }
                if (saved >= 0) ::close(saved);
            }
            char buf[PATH_MAX];
            if (::getcwd(buf, sizeof(buf))) {
                emu.vfs().apply_chdir(std::string(buf));
            }
            ret_host(r);
            return 0;
        }
        case 49: { // chdir(path) — AArch64 49
            std::string guest_path = yggdrasil::Yggdrasil::read_path(mem_, a0);
            // Confine via open+fchdir: a raw chdir() follows symlinks, so
            // chdir("$ROOT/link-to-/tmp") moved the HOST cwd outside the
            // sandbox and every later AT_FDCWD-relative open escaped with
            // it. open_host_dir resolves under RESOLVE_IN_ROOT when a
            // sandbox is active.
            int dfd = yggdrasil::Yggdrasil::open_host_dir(guest_path);
            if (dfd < 0) { ret_errno(); return 0; }
            // Save the current dir so a failed (escaping) chdir restores
            // it: POSIX leaves cwd unchanged when chdir fails, and the
            // pre-existing rollback-to-root broke that (cwd jumped from
            // /sub to / after a denied chdir).
            int saved = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            int r = ::fchdir(dfd);
            int se = errno;
            ::close(dfd);
            if (r < 0) {
                if (saved >= 0) ::close(saved);
                errno = se; ret_errno(); return 0;
            }
            // Relative paths resolve against the host cwd, which may live
            // outside the sandbox — verify containment afterwards and roll
            // back when the new cwd escaped. (fchdir(fd) itself is left
            // alone: chroot-parity for smuggled fds, and host-cwd compat.)
            if (yggdrasil::Yggdrasil::sandbox_inside_root()) {
                char cwd[PATH_MAX];
                // getcwd failure = unverifiable cwd: fail CLOSED (roll
                // back if possible) rather than accepting blindly. A
                // failed rollback is best-effort only (nothing left to
                // restore with); the EACCES still stops the caller from
                // assuming the chdir landed.
                bool verified = ::getcwd(cwd, sizeof(cwd)) &&
                                yggdrasil::Yggdrasil::path_inside_root(cwd);
                if (!verified) {
                    if (saved >= 0) {
                        ::fchdir(saved);
                        ::close(saved);
                    }
                    ret_err(EACCES);
                    return 0;
                }
            }
            if (saved >= 0) ::close(saved);
            emu.vfs().apply_chdir(guest_path);
            ret_host(r);
            return 0;
        }
        case 17: { // getcwd(buf, size) — AArch64 syscall 17
            // BUGFIX: the old code always returned "/" regardless of
            // chdir() calls. Now we track the guest cwd in the VFS
            // (updated by chdir/fchdir) and return the real path here.
            // The host cwd is meaningless because BIFROST_ROOT sandboxing
            // decouples them.
            std::string cwd = emu.vfs().get_cwd();
            size_t need = cwd.size() + 1;  // include NUL terminator
            if (a1 < need) { ret_err(ERANGE); return 0; }
            try {
                mem_.write(a0, cwd.data(), need);
            } catch (...) { ret_err(EFAULT); return 0; }
            // The Linux getcwd syscall reports the buffer length including
            // the terminating NUL written above.
            ret_host(static_cast<uint64_t>(need));
            return 0;
        }
        case 51: { // chroot(path) — AArch64 51
            // BUGFIX: was previously labeled "fchmod" but fchmod is at 52
            // (handled in FileSystem.cpp). The real syscall at 51 is chroot. We
            // don't support chroot; return -EPERM (requires CAP_SYS_CHROOT).
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
            return 0;
        }
        case 45: { // truncate(path, length) — AArch64 45
            std::string guest = yggdrasil::Yggdrasil::read_path(mem_, a0);
            int r;
            if (yggdrasil::Yggdrasil::path_confined(AT_FDCWD, guest)) {
                ScopedHostFd target(yggdrasil::Yggdrasil::open_confined_path(AT_FDCWD, guest, true));
                r = target.fd < 0 ? -1
                    : ::truncate(yggdrasil::Yggdrasil::proc_fd_path(target.fd).c_str(), (off_t)a1);
            } else {
                std::string path = yggdrasil::Yggdrasil::remap_path(guest);
                r = ::truncate(path.c_str(), (off_t)a1);
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 40: { // mount(source, target, fstype, flags, data) — AArch64 40
            // BUGFIX: previously implemented as sendfile (which is at 71,
            // already handled in Miscellaneous.cpp). The old code dereferenced
            // `fstype` (a string pointer) as an `off_t*` and passed
            // `source`/`target` (string pointers) as fds to ::sendfile.
            // We don't support mount; return -ENOSYS (or -EPERM).
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
            return 0;
        }
        case 39: { // umount2(target, flags) — AArch64 39
            // AArch64 syscall 39 is umount2, NOT symlink (symlinkat is 36).
            // Mounts belong to the host, not the guest: like mount and
            // pivot_root, refuse (EPERM, as without CAP_SYS_ADMIN) instead
            // of unmounting a host filesystem when the emulator runs as root.
            cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EPERM));
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
        case 42: { // nfsservctl — AArch64 42 (unimplemented, returns ENOSYS)
            // AArch64 syscall 42 is nfsservctl, NOT link. The old code
            // dispatched 42 to link(), but musl's link() wrapper calls
            // syscall 37 (linkat). linkat is now correctly handled at
            // syscall 37 in FileSystem.cpp.
            ret_err(ENOSYS);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
