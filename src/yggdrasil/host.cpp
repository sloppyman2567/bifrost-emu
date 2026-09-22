// yggdrasil/host.cpp — host passthrough + BIFROST_ROOT path remap.
//
// If BIFROST_ROOT is set in the environment, guest absolute paths
// starting with "/" (except /proc and /dev which are virtual) are
// remapped to "$BIFROST_ROOT/<path>". This lets the user sandbox guest
// file I/O to a specific directory.
//
// If BIFROST_ROOT is not set, the path is passed through to the host
// openat() unchanged.
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/host_node.hpp"
#include "yggdrasil/terminal_ioctls.hpp"  // shared ioctl dispatch
#include "core/memory.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/openat2.h>
#endif
namespace arm64emu::yggdrasil {
// File-local helpers defined below (order matters for single-pass use).
static std::string sandbox_root_str();
static bool have_openat2();
static bool dirfd_confined(int host_dirfd);
// ── Sandboxed open (openat2 RESOLVE_IN_ROOT) ──────────────────────────
// The lexical `..` normalization in map_guest_path stops "../.." walks,
// but host openat() still FOLLOWS symlinks: a guest symlink inside
// BIFROST_ROOT pointing at /etc/passwd escaped the sandbox (read, write,
// and execve). openat2(RESOLVE_IN_ROOT) confines the ENTIRE resolution —
// "..", absolute symlinks, and absolute components — under a root fd,
// which is strictly stronger than chroot (chroot does not confine
// symlink targets either). Verified: escaping trailing symlink fails
// with ENOENT under IN_ROOT (ELOOP with O_NOFOLLOW).
#ifndef SYS_openat2
// Ancient libc without the openat2 number: every confined open falls
// back to plain openat (documented degradation, same as before).
static constexpr long kNoOpenat2 = -1;
#else
static constexpr long kNoOpenat2 = 0;  // (unused; keeps -Wunused off)
#endif
static bool have_openat2() {
#ifdef SYS_openat2
    // Probe once, race-free: openat2 exists since Linux 5.6; old kernels
    // return ENOSYS. (A plain static here would be a data race across
    // syscall threads.)
    static std::once_flag flag;
    static bool avail = false;
    std::call_once(flag, [] {
        int se = errno;  // preserve: probe clobbers errno either way
        struct open_how probe {};
        probe.flags = O_RDONLY;
        probe.resolve = RESOLVE_IN_ROOT;
        long r = syscall(SYS_openat2, AT_FDCWD, ".", &probe, sizeof(probe));
        if (r >= 0) {
            ::close(static_cast<int>(r));
            avail = true;
        } else {
            // ENOSYS = no openat2. Any other outcome (or success) means
            // the number exists (a lookup error just reflects host cwd).
            avail = (errno != ENOSYS);
        }
        errno = se;
    });
    return avail;
#else
    (void)kNoOpenat2;
    return false;
#endif
}
// Cached O_RDONLY|O_DIRECTORY fd for a sandbox root, so confined opens
// don't pay open+close per call. Reopened if the root changes (tests
// setenv a scratch root). Mutex-guarded: syscalls run on threads.
// Takes the wanted root explicitly so callers snapshot getenv() once.
static int sandbox_root_fd(const std::string& want) {
    static std::mutex mu;
    static std::string cached_path;
    static int cached_fd = -1;
    std::lock_guard<std::mutex> g(mu);
    if (!want.empty() && want == cached_path) return cached_fd;
    // Root changed (or first use): intentionally LEAK the old fd instead
    // of closing it. A concurrent in-flight openat2 may still be using
    // it; closing under it would recycle the number into a wrong-root
    // operation (or EBADF). Roots change ~never outside tests, so one
    // leaked fd per change. Failures are NOT cached: a transient error
    // retries next call instead of sticking permanently.
    if (!want.empty()) {
        int fd = ::open(want.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            cached_path = want;
            cached_fd = fd;
            return fd;
        }
    }
    return -1;
}
// Canonical (symlink-free) form of the cached root for prefix checks.
// Cached under the same mutex; empty when unresolvable.
static std::string sandbox_root_canonical() {
    static std::mutex mu;
    static std::string cached_path;
    static std::string cached_canon;
    std::string want = sandbox_root_str();
    std::lock_guard<std::mutex> g(mu);
    if (want == cached_path) return cached_canon;
    cached_path = want;
    cached_canon.clear();
    if (!want.empty()) {
        char resolved[PATH_MAX];
        if (::realpath(want.c_str(), resolved)) cached_canon = resolved;
    }
    return cached_canon;
}
// True when `guest_path` is subject to sandbox confinement under `root`:
// the path is absolute and not one of the passthrough carve-outs (/proc,
// /dev, exact-$XAUTHORITY). Must stay in sync with map_guest_path below.
// Takes the root explicitly so callers snapshot getenv() once.
static bool uses_sandbox(const std::string& root,
                         const std::string& guest_path) {
    if (root.empty()) return false;
    if (guest_path.empty() || guest_path[0] != '/') return false;
    auto is_under = [](const std::string& p, const char* base) {
        size_t n = std::strlen(base);
        return p.compare(0, n, base) == 0 &&
               (p.size() == n || p[n] == '/');
    };
    if (is_under(guest_path, "/proc")) return false;
    if (is_under(guest_path, "/dev")) return false;
    const char* xauth = getenv("XAUTHORITY");
    if (xauth && xauth[0] == '/' && guest_path == xauth) return false;
    return true;
}
// Confined open of an already-remapped absolute host path that is known
// to sit under `root` (the BIFROST_ROOT value the caller snapshotted).
// Strips the root prefix and resolves the remainder under the cached
// root fd with RESOLVE_IN_ROOT. Falls back to plain openat when openat2
// or the root fd is unavailable.
static int open_rooted(const std::string& root, const std::string& host_path,
                       int flags, mode_t mode) {
    std::string rel = host_path.size() > root.size()
                          ? host_path.substr(root.size())
                          : std::string();
    // host_path = root + norm, norm starts with '/' (or is empty for "/").
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty()) rel = ".";
    int rootfd = sandbox_root_fd(root);
    if (rootfd >= 0 && have_openat2()) {
#ifdef SYS_openat2
        struct open_how how {};
        how.flags = static_cast<__u64>(static_cast<uint64_t>(flags));
        how.mode = static_cast<__u64>(mode);
        how.resolve = RESOLVE_IN_ROOT;
        int fd = static_cast<int>(
            syscall(SYS_openat2, rootfd, rel.c_str(), &how, sizeof(how)));
        if (fd >= 0 || errno != ENOSYS) return fd;
#endif
    }
    if (rootfd < 0) {
        // Root configured but unopenable (deleted? perms?): fail CLOSED.
        // Falling back to plain openat here would resolve the remapped
        // absolute path with symlink-following and no confinement.
        errno = ENOENT;
        return -1;
    }
    // No openat2 on this kernel: textual confinement only (symlinks may
    // still escape — documented degradation, same as before this change).
    return ::openat(AT_FDCWD, host_path.c_str(), flags, mode);
}
// Snapshot $BIFROST_ROOT with trailing slashes stripped ("" when unset).
// Callers snapshot once per syscall so the root can't change mid-call.
static std::string sandbox_root_str() {
    const char* r = getenv("BIFROST_ROOT");
    if (!r || !r[0]) return std::string();
    std::string root(r);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root;
}
// True for /proc/<digits>... (any numeric PID dir, including bare
// "/proc/1234"). The guest's getpid() returns 1, so /proc/1/* means the
// guest itself — but procfs only serves /proc/self/*; anything else fell
// through to the HOST proc table (other PIDs' environ/maps/mem!).
static bool is_numeric_proc_pid(const std::string& p) {
    if (p.compare(0, 6, "/proc/") != 0) return false;
    size_t i = 6, n = p.size();
    if (i >= n) return false;
    while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    if (i == 6) return false;  // "self", "sys", ... — not numeric
    return i == n || p[i] == '/';
}
// Helper: remap a guest path to a host path using BIFROST_ROOT.
// `root_override` ("" = read env): lets callers that already snapshotted
// the root pass it explicitly so map and open can't disagree under a
// concurrent host setenv (which in practice never happens — guest setenv
// is invisible and the suite sets up-front — but the plumbing is free).
std::string map_guest_path(const std::string& guest_path,
                           const char* root_override = nullptr) {
    const char* bifrost_root =
        root_override ? root_override : getenv("BIFROST_ROOT");
    if (!bifrost_root || !bifrost_root[0]) return guest_path;
    if (guest_path.empty() || guest_path[0] != '/') return guest_path;
    // Virtual filesystems are not remapped. Match on a path boundary so
    // "/procedure" and "/device" are NOT mistaken for /proc and /dev
    // (a substr prefix test used to let those bypass the sandbox).
    auto is_under = [](const std::string& p, const char* base) {
        size_t n = std::strlen(base);
        return p.compare(0, n, base) == 0 &&
               (p.size() == n || p[n] == '/');
    };
    // Sandbox denial: numeric-PID proc paths (/proc/1234/...) and
    // /proc/self/mem never reach the host proc table. Unserved, they fell
    // through to HOST /proc — other processes' environ/maps/mem and the
    // emulator's own memory. "" turns into ENOENT naturally downstream
    // (openat("") fails), so no call-site changes are needed. /proc/self/*
    // otherwise keeps host passthrough (compat: task/fd/mountinfo).
    if (is_numeric_proc_pid(guest_path) || guest_path == "/proc/self/mem" ||
        guest_path.compare(0, 15, "/proc/self/mem/") == 0)
        return std::string();
    if (is_under(guest_path, "/proc")) return guest_path;
    if (is_under(guest_path, "/dev")) return guest_path;
    // X11 clients need to read the host X auth cookie. If the guest opens
    // the exact path the host XAUTHORITY points at, pass it through
    // unchanged — it lives in the host session dir (e.g. /run/user/...),
    // outside the BIFROST_ROOT sandbox. Without this, xcb connects to the
    // X socket but the server rejects it with "Authorization required".
    const char* xauth = getenv("XAUTHORITY");
    if (xauth && xauth[0] == '/' && guest_path == xauth) return guest_path;
    std::string root(bifrost_root);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    // Lexically normalize "." and ".." before remapping (chroot-style:
    // ".." at the root stays at the root). Without this, a guest path of
    // "/../../etc/passwd" produced "$ROOT/../../etc/passwd" and read a
    // host file outside the sandbox.
    std::string norm;
    size_t i = 0;
    while (i < guest_path.size()) {
        size_t j = guest_path.find('/', i);
        if (j == std::string::npos) j = guest_path.size();
        std::string comp = guest_path.substr(i, j - i);
        if (comp.empty() || comp == ".") {
            // no-op
        } else if (comp == "..") {
            size_t slash = norm.rfind('/');
            if (slash == std::string::npos) norm.clear();
            else norm.erase(slash);
        } else {
            norm += '/';
            norm += comp;
        }
        i = j + 1;
    }
    return root + norm;  // norm is empty for "/" (or all-"." paths)
}
// Static method exposed via Yggdrasil so syscall handlers don't need to
// friend host.cpp's free function.
std::string Yggdrasil::remap_path(const std::string& guest_path) {
    return map_guest_path(guest_path);
}
std::unique_ptr<Node> Yggdrasil::open_host(const std::string& guest_path,
                                           int flags, mode_t mode, int* err_out) {
    // Route the raw open through the confined helper so symlink/.. escapes
    // and denied proc paths share one policy. Empty (denied) paths fail
    // inside with ENOENT instead of reaching the host.
    int fd = open_host_fd(guest_path, flags, mode);
    if (fd < 0) {
        *err_out = -errno;
        return nullptr;
    }
    return std::make_unique<HostNode>(fd, flags);
}
// ── Confined raw opens ──────────────────────────────────────────────
// Central choke point for every guest-controlled host open (open/openat,
// execve, chdir). Absolute sandboxed paths resolve under the cached root
// fd with RESOLVE_IN_ROOT; everything else keeps legacy behavior.
// Returns a host fd or -1 with errno set (O_DIRECT retry included).
int Yggdrasil::open_host_fd(const std::string& guest_path, int flags,
                            mode_t mode) {
    std::string root = sandbox_root_str();
    std::string host_path = map_guest_path(guest_path, root.c_str());
    if (host_path.empty()) { errno = ENOENT; return -1; }  // denied proc path
    int fd = -1;
    if (uses_sandbox(root, guest_path)) {
        fd = open_rooted(root, host_path, flags, mode);
    } else {
        fd = ::openat(AT_FDCWD, host_path.c_str(), flags, mode);
    }
    // on x86_64 hosts (the kernel rejects it). But on real AArch64 Linux,
    // O_DIRECT is silently ignored for directories. BusyBox's `ls` opens
    // directories with O_RDONLY|O_DIRECTORY|O_NONBLOCK|O_CLOEXEC (and
    // sometimes O_DIRECT from certain code paths), so we need to handle
    // this difference. If the open fails with EINVAL and O_DIRECT is set,
    // retry without O_DIRECT.
    if (fd < 0 && errno == EINVAL && (flags & O_DIRECT)) {
        if (uses_sandbox(root, guest_path)) {
            fd = open_rooted(root, host_path, flags & ~O_DIRECT, mode);
        } else {
            fd = ::openat(AT_FDCWD, host_path.c_str(), flags & ~O_DIRECT,
                          mode);
        }
    }
    return fd;
}
// Confined open of a path RELATIVE to an already-open host dir fd
// (explicit-dirfd openat). With a sandbox active the lookup stays under
// that dir via RESOLVE_IN_ROOT; without one it is plain openat.
int Yggdrasil::open_host_at(int host_dirfd, const std::string& relpath,
                            int flags, mode_t mode) {
    // AT_FDCWD keeps legacy host-cwd behavior verbatim (the host cwd may
    // live outside the sandbox and games depend on it) — callers only
    // pass real dirfds here.
    if (host_dirfd == AT_FDCWD)
        return ::openat(AT_FDCWD, relpath.c_str(), flags, mode);
    if (dirfd_confined(host_dirfd) && have_openat2()) {
#ifdef SYS_openat2
        struct open_how how {};
        how.flags = static_cast<__u64>(static_cast<uint64_t>(flags));
        how.mode = static_cast<__u64>(mode);
        how.resolve = RESOLVE_IN_ROOT;
        int fd = static_cast<int>(
            syscall(SYS_openat2, host_dirfd, relpath.c_str(), &how,
                    sizeof(how)));
        if (fd >= 0 || errno != ENOSYS) return fd;
#endif
    }
    return ::openat(host_dirfd, relpath.c_str(), flags, mode);
}
// Confined open of a directory for chdir/fchdir preparation. Always
// O_RDONLY|O_DIRECTORY so the result is fchdir-able.
int Yggdrasil::open_host_dir(const std::string& guest_path) {
    return open_host_fd(guest_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
}
// ── Confined parent resolution for path syscalls ────────────────────
// Splits (dirfd, guest_path) into a confined parent dir fd + a single
// basename with no '/'. The parent traversal (the part that can walk
// through symlinks and "..") goes through openat2 RESOLVE_IN_ROOT
// whenever confinement applies; the basename is then passed to the
// caller's *at() verbatim. A trailing symlink in the basename behaves
// chroot-like (followed as-is: mkdir→EEXIST, unlink/rename→operate on
// the link itself, stat→reads target metadata).
//
// Confinement applies when: (a) the guest path is absolute and a sandbox
// root is set (parent opened under the root fd), or (b) the path is
// relative and host_dirfd itself resolves inside the root. Otherwise the
// parent opens exactly as before (legacy behavior, unchanged).
Yggdrasil::ParentDir Yggdrasil::open_parent(int host_dirfd,
                                            const std::string& guest_path) {
    ParentDir pd;
    if (guest_path.empty()) { errno = ENOENT; return pd; }
    std::string root = sandbox_root_str();
    std::string dir, base;
    bool confine = false;
    if (!guest_path.empty() && guest_path[0] == '/') {
        std::string host_path = map_guest_path(guest_path, root.c_str());
        if (host_path.empty()) { errno = ENOENT; return pd; }  // denied
        size_t slash = host_path.rfind('/');
        std::string parent = host_path.substr(0, slash);
        base = host_path.substr(slash + 1);
        if (!root.empty() && uses_sandbox(root, guest_path)) {
            if (host_path.size() == root.size()) {
                // Guest "/" itself (map yields exactly root): operate on
                // the root dir itself via base="." so mkdir("/") gives
                // EEXIST etc. instead of mis-targeting $ROOT/<rootname>.
                int rootfd = sandbox_root_fd(root);
                if (rootfd < 0) { errno = ENOENT; return pd; }
                pd.fd = rootfd;
                pd.owned = false;
                pd.base = ".";
                return pd;
            }
            // Strip the root prefix for the rootfd-relative open.
            std::string rel = parent.size() > root.size()
                                  ? parent.substr(root.size())
                                  : std::string();
            while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
            if (rel.empty()) {
                // Parent IS the root: borrow the cached rootfd (never
                // closed — leak-on-evict keeps it valid; do not close).
                // Unopenable root fails CLOSED (ENOENT) rather than
                // falling back to an unconfined open.
                int rootfd = sandbox_root_fd(root);
                if (rootfd < 0) { errno = ENOENT; return pd; }
                pd.fd = rootfd;
                pd.owned = false;
                pd.base = base;
                return pd;
            }
            int rootfd = sandbox_root_fd(root);
            if (rootfd < 0) { errno = ENOENT; return pd; }  // fail closed
            int pfd = -1;
            if (have_openat2()) {
#ifdef SYS_openat2
                struct open_how how {};
                how.flags = O_RDONLY | O_DIRECTORY;
                how.resolve = RESOLVE_IN_ROOT;
                pfd = static_cast<int>(syscall(SYS_openat2, rootfd,
                                               rel.c_str(), &how, sizeof(how)));
                if (pfd < 0 && errno != ENOSYS) return pd;  // errno set
#endif
            }
            if (pfd < 0) {
                // No openat2 (or ENOSYS): textual confinement only.
                pfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (pfd < 0) return pd;  // errno set
            }
            pd.fd = pfd;
            pd.owned = true;
            pd.base = base;
            return pd;
        }
        // No sandbox: plain parent open (identical to before).
        dir = parent;
    } else {
        // Relative path: split guest-side, resolve parent under dirfd.
        size_t slash = guest_path.rfind('/');
        std::string reldir = (slash == std::string::npos)
                                 ? std::string()
                                 : guest_path.substr(0, slash);
        base = (slash == std::string::npos) ? guest_path
                                            : guest_path.substr(slash + 1);
        // Strip trailing slashes from the basename side is wrong;
        // trailing slashes mean "must be a dir" — but the basename must
        // not contain '/'. A trailing slash would make base empty:
        if (base.empty()) { errno = ENOENT; return pd; }
        if (reldir.empty()) {
            // Direct child of dirfd: no parent open needed; the dirfd
            // itself is the parent (borrowed, owned by FdTable/the caller).
            if (host_dirfd == AT_FDCWD) {
                pd.fd = AT_FDCWD;
            } else {
                if (host_dirfd < 0) { errno = EBADF; return pd; }
                pd.fd = host_dirfd;
            }
            pd.owned = false;
            pd.base = base;
            return pd;
        }
        confine = dirfd_confined(host_dirfd);
        int pfd = -1;
        if (confine && have_openat2()) {
#ifdef SYS_openat2
            struct open_how how {};
            how.flags = O_RDONLY | O_DIRECTORY;
            how.resolve = RESOLVE_IN_ROOT;
            pfd = static_cast<int>(syscall(SYS_openat2, host_dirfd,
                                           reldir.c_str(), &how, sizeof(how)));
            if (pfd < 0 && errno != ENOSYS) return pd;  // errno set
#endif
        }
        if (pfd < 0) {
            pfd = ::openat(host_dirfd, reldir.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (pfd < 0) return pd;  // errno set
        }
        pd.fd = pfd;
        pd.owned = true;
        pd.base = base;
        return pd;
    }
    // Absolute path, no sandbox: plain parent open.
    int pfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) return pd;  // errno set
    pd.fd = pfd;
    pd.owned = true;
    pd.base = base;
    return pd;
}
bool Yggdrasil::sandbox_inside_root() {
    return !sandbox_root_str().empty();
}
bool Yggdrasil::path_inside_root(const char* path) {
    if (!path || !path[0]) return false;
    std::string canon = sandbox_root_canonical();
    if (canon.empty()) return false;
    // Degenerate root "/": everything is inside.
    if (canon == "/") return path[0] == '/';
    size_t n = canon.size();
    if (std::strncmp(path, canon.c_str(), n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}
// ── dirfd confinement decision ────────────────────────────────────
// True when `host_dirfd` resolves inside the sandbox root, meaning
// RESOLVE_IN_ROOT lookups under it stay confined. AT_FDCWD is judged by
// the host cwd; real fds via /proc/self/fd/N (canonical kernel paths,
// so the prefix check is exact). Anything unresolvable → false, i.e.
// legacy plain-openat behavior identical to before this change.
static bool dirfd_confined(int host_dirfd) {
    std::string root = sandbox_root_str();
    if (root.empty()) return false;
    char target[PATH_MAX];
    if (host_dirfd == AT_FDCWD) {
        if (!::getcwd(target, sizeof(target))) return false;
    } else {
        char proc[64];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", host_dirfd);
        ssize_t n = ::readlink(proc, target, sizeof(target) - 1);
        if (n <= 0) return false;
        target[n] = '\0';
    }
    return Yggdrasil::path_inside_root(target);
}
// ── HostNode method implementations ───────────────────────────────────
// host_fd() is inline in host_node.hpp
ssize_t HostNode::read(uint64_t off, void* buf, size_t n) {
    // pread when positioned: lseek+read races on shared fds (two guest
    // threads sharing an fd would scramble each other's file offset).
    if (off != UINT64_MAX) {
        ssize_t r = ::pread(fd_, buf, n, static_cast<off_t>(off));
        return r < 0 ? -errno : r;
    }
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t HostNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) {
        ssize_t r = ::pwrite(fd_, buf, n, static_cast<off_t>(off));
        return r < 0 ? -errno : r;
    }
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t HostNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}
int HostNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}
// HostNode::ioctl — handle terminal and FIONREAD ioctls by forwarding
// to the host fd. v1.4.5-alpha: moved here from ioctls.cpp's heuristic
// dispatch. refactored to use the shared
// dispatch_terminal_ioctl() helper (was duplicated in StdioNode).
int HostNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    int r = dispatch_terminal_ioctl(fd_, request, argp, mem);
    if (r != Node::IOCTL_NOT_HANDLED) return r;
    // Anything else: pass through to the host. The host will return
    // -ENOTTY for unrecognized ioctls, which is what we want.
    return pass_through_ioctl(fd_, request, argp);
}
} // namespace arm64emu::yggdrasil
