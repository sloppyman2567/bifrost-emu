// VFS.hpp — the world-tree: guest path → Node resolver.
//
// Yggdrasil is the VFS subsystem of bifrost-emu. In Norse cosmology,
// Yggdrasil is the world-tree that connects the nine realms; here it
// connects the guest's path namespace to five "worlds":
//
//   1. ProcFS    — /proc/self/{exe,cmdline,maps,status,auxv,environ},
//                  /proc/{meminfo,cpuinfo,version}, /proc/sys/kernel/osrelease
//   2. SysFS     — /sys/devices/system/cpu/* (synthetic CPU
//                  topology, all served from kGuestCpuCount)
//   3. DevFS     — /dev/{null,zero,urandom,random,tty,fb0,stdin,stdout,stderr,
//                  dsp,snd,ptmx,pts/*}
//   4. PathRemap — BIFROST_ROOT sandbox (if set)
//   5. Host      — openat() passthrough on the (possibly remapped) host path
//
//   Guest path ──► Yggdrasil::open() ──► Node* ──► FdTable::allocate()
//                                                    │
//                                                    ▼
//                                                guest fd
//
// The FdTable owns the shared_ptr<Node> for each guest fd. close() drops
// the refcount; dup()/dup2() create new fds pointing to the same Node.
//
// renamed from `VFS` to `Yggdrasil` to give the subsystem
// an identity (matching the Norse theme of bifrost + FrostJIT). The
// class names `VFS`, `VNode`, `HostVNode`, etc. were renamed to
// `Yggdrasil`, `Node`, `HostNode`, etc. The file layout was split
// from `vfs/vfs.{h,cpp}` + `vfs/vfs_table.{h,cpp}` into per-concern
// files under `yggdrasil/`. No behavior change beyond the
// improvements documented in CHANGELOG.md.
#pragma once
#include "Bifrost/Types.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// Forward-declare the arm64emu types we reference (the real definitions
// live in arm64emu:: — Graphics.hpp and audio/audio.h). These must be
// declared at the arm64emu scope so the yggdrasil namespace can refer
// to them without qualification.
namespace arm64emu {
    class FrostGraphics;
    class Audio;
    class Memory;
}
namespace arm64emu::yggdrasil {
struct BinderDomain;
// ── Guest CPU topology: the single source of truth ─────────
// Every guest-visible CPU-count source advertises this many
// CPUs: sched_getaffinity (Syscalls/Process/Scheduling.cpp), the
// synthetic sysfs tree (yggdrasil/sysfs.cpp) and
// /proc/cpuinfo (yggdrasil/procfs.cpp). Bionic's
// sysconf(_SC_NPROCESSORS_ONLN) reads
// /sys/devices/system/cpu/online, so the sysfs tree and the
// affinity mask MUST agree — a mismatch (or a missing sysfs,
// which made sysconf return -1) made libunity's job system
// compute a zero worker count and stall the presentation
// thread. Change the count here only; never per-file.
inline constexpr int kGuestCpuCount = 8;
// ── Yggdrasil: path → Node resolver ───────────────────────────────────
class Yggdrasil {
public:
    Yggdrasil();
    void stop_binder();
    void set_binder_broker(std::string path) { binder_broker_ = std::move(path); }
    // Select before opening Binder devices. Only /dev/binder gets a manager;
    // hardware/vendor domains retain the raw transport contract.
    void enable_android_services();
    // Serve Bionic system properties at /dev/__properties__ (Android runtime).
    void enable_android_properties() { android_properties_ = true; }
    // Fixed credentials for the single installed-app process. Set before load.
    void set_process_identity(uint32_t uid, uint32_t gid) { process_uid_ = uid; process_gid_ = gid; }
    uint32_t process_uid() const { return process_uid_; }
    uint32_t process_gid() const { return process_gid_; }
    static bool binder_device_stat(const std::string& path, struct stat* st);
    // Synthetic stat for the SysFS tree (fstatat/statx/faccessat do
    // not consult Yggdrasil::open — they go straight to the host via
    // remap_path). Mirrors binder_device_stat: fills `st` from the
    // same classifier that serves the open path, so a guest that
    // stats /sys/devices/system/cpu/* before opening sees the same
    // types and sizes (a host /sys would report the HOST's core
    // count and contradict sched_getaffinity). Returns false when
    // `path` is not part of the synthetic tree.
    static bool sysfs_stat(const std::string& path, struct stat* st);
    // Resolve a guest path to a Node. Returns nullptr on failure (with
    // errno set via the `*err_out` parameter).
    //
    // Resolution order:
    //   1. ProcFS  (/proc/*)
    //   2. SysFS   (/sys/* — always synthetic)
    //   3. DevFS   (/dev/*)
    //   4. PathRemap (BIFROST_ROOT sandbox)
    //   5. Host passthrough (openat on the remapped path)
    std::unique_ptr<Node> open(const std::string& guest_path,
                               int flags, mode_t mode, int* err_out);
    // Read a NUL-terminated path string from guest memory. Reads at most
    // 4096 bytes to prevent runaway reads from bad pointers.
    static std::string read_path(::arm64emu::Memory& mem, uint64_t addr);
    // Remap a guest path to a host path using BIFROST_ROOT.
    // (Defined in yggdrasil/host.cpp — exposed here so syscall handlers
    // can call it for path-based syscalls like unlinkat/renameat/etc.)
    static std::string remap_path(const std::string& guest_path);
    // Confined raw host open (fd or -1 with errno). Used by open_host
    // internally and by execve/chdir which need the fd, not a Node.
    static int open_host_fd(const std::string& guest_path, int flags,
                            mode_t mode = 0);
    // Confined open relative to a host directory fd or AT_FDCWD. A cwd
    // outside the configured root retains legacy host-relative access.
    static int open_host_at(int host_dirfd, const std::string& relpath,
                            int flags, mode_t mode = 0);
    // Confined O_RDONLY|O_DIRECTORY open for chdir preparation.
    static int open_host_dir(const std::string& guest_path);
    // Confined parent dir + basename for path syscalls (mkdirat,
    // unlinkat, renameat, fstatat, ...). The parent traversal — the part
    // that can walk through symlinks and ".." — is confined with
    // RESOLVE_IN_ROOT whenever a sandbox (or a confined dirfd) applies;
    // the basename never contains '/'. Callers use pd.fd/pd.base and do
    // nothing else: the destructor closes owned fds (leak-proof on all
    // error paths). Invalid results carry errno (check valid()).
    struct ParentDir {
        int fd = -1;
        bool owned = false;
        std::string base;
        ~ParentDir() { if (owned && fd >= 0) ::close(fd); }
        ParentDir() = default;
        ParentDir(const ParentDir&) = delete;
        ParentDir& operator=(const ParentDir&) = delete;
        ParentDir(ParentDir&& o) noexcept
            : fd(o.fd), owned(o.owned), base(std::move(o.base)) {
            o.fd = -1;
            o.owned = false;
        }
        // AT_FDCWD is a valid parent for a bare relative name.
        bool valid() const { return (fd >= 0 || fd == AT_FDCWD) && !base.empty(); }
    };
    static ParentDir open_parent(int host_dirfd,
                                 const std::string& guest_path);
    // True when BIFROST_ROOT confinement applies to (host_dirfd,
    // guest_path): an absolute sandboxed path, or a relative path under a
    // dirfd/cwd inside the root. Metadata and attribute syscalls then use
    // open_confined_path; otherwise they keep their direct host call.
    static bool path_confined(int host_dirfd, const std::string& guest_path);
    // O_PATH|O_CLOEXEC fd for a confined path, resolved with
    // RESOLVE_IN_ROOT (O_NOFOLLOW unless `follow`), or -1 with errno.
    // Operate on it with AT_EMPTY_PATH calls or through proc_fd_path().
    static int open_confined_path(int host_dirfd, const std::string& guest_path,
                                  bool follow);
    // "/proc/self/fd/N": the kernel magic link to exactly that open file.
    static std::string proc_fd_path(int host_fd);
    // True when a BIFROST_ROOT sandbox is active.
    static bool sandbox_inside_root();
    // Canonical host path of the BIFROST_ROOT sandbox ("" when inactive).
    static std::string sandbox_root_path();
    // True when `path` (a canonical getcwd-style path) lies inside the
    // canonical sandbox root (boundary-aware). False when unresolvable.
    static bool path_inside_root(const char* path);
    // Kernel /proc target for a real host fd, with the canonical sandbox
    // prefix removed so linker/realpath callers see a guest absolute path.
    static int fd_link_target(int host_fd, std::string& target);
    // Guest view of a canonical host path: the sandbox prefix removed.
    static std::string guest_view_of_host_path(std::string path);
    // Explicit-dirfd open: absolute paths behave exactly like open()
    // (dirfd ignored per POSIX); relative paths resolve under host_dirfd
    // (a real host dir fd, or AT_FDCWD with the same confinement policy).
    // ProcFS/DevFS only match absolute paths, so relative opens go
    // straight to the confined host helper.
    std::unique_ptr<Node> open_at(int host_dirfd,
                                  const std::string& guest_path,
                                  int flags, mode_t mode, int* err_out);
    // Set the ELF path (used by /proc/self/exe). Called once from
    // Emulator::load_elf_file().
    void set_elf_path(const std::string& path, const std::string& backing_path = {}) {
        elf_path_ = path;
        elf_backing_path_ = backing_path;
    }
    const std::string& elf_path() const { return elf_path_; }
    void set_initial_stack(uint64_t address) { initial_stack_ = address; }
    // Live /proc/self/cmdline contents, read from the guest's argv block.
    void set_cmdline_provider(std::function<std::string()> cb) {
        cmdline_provider_ = std::move(cb);
    }
    // The guest's own pid as getpid() reports it: 1, or the host pid in a
    // fork child. /proc/<that pid> names the same process as /proc/self.
    void set_self_pid(int pid) { self_pid_ = pid; }
    int self_pid() const { return self_pid_; }
    // The process identity getpid/getppid/getpgid/getsid and /proc report:
    // the PID-1 model of a plain run, or the host process's own identity
    // for fork children and Binder-broker apps (self_pid != 1).
    struct ProcessIds { int pid, ppid, pgid, sid; };
    ProcessIds process_ids() const;
    std::string canonical_proc_path(const std::string& path) const;
    // Wire up the graphics backend (for /dev/fb0). May be null if the
    // emulator was built without SDL2 and the guest never opens /dev/fb0.
    void set_graphics(::arm64emu::FrostGraphics* gfx) { gfx_ = gfx; }
    // Wire up the audio backend (for /dev/dsp, /dev/snd). May be null.
    void set_audio(::arm64emu::Audio* audio) { audio_ = audio; }
    // ── Live memory layout for /proc/self/maps ─────────────────────
    // The Emulator registers a callback that returns every guest mapping
    // in address order, printed in the kernel's /proc/<pid>/maps format.
    struct MapEntry {
        uint64_t    start;
        uint64_t    end;
        char        perms[5];  // "rwxp\0"
        uint64_t    offset = 0;            // file offset of `start`
        uint64_t    dev = 0, inode = 0;    // host st_dev/st_ino of the file
        std::string label;     // guest path, "[stack]"/"[heap]"/"[vdso]" or ""
    };
    void set_maps_provider(std::function<std::vector<MapEntry>()> cb) {
        maps_provider_ = std::move(cb);
    }
    // ── Guest process name (comm) for /proc/self/comm ─────────────
    // The Emulator registers a callback that returns the current
    // guest_comm_ (set by prctl PR_SET_NAME, defaults to ELF basename).
    void set_comm_provider(std::function<std::string()> cb) {
        comm_provider_ = std::move(cb);
    }
    // ── Guest cwd tracking (chdir/getcwd) ──────────────────────────
    // The Emulator registers the guest's current working directory
    // here so the getcwd syscall can return the real path (the host
    // cwd is meaningless because BIFROST_ROOT sandboxing decouples
    // them). Updated by chdir/fchdir; queried by getcwd.
    void set_cwd_provider(std::function<std::string()> getter,
                          std::function<bool(const std::string&)> setter) {
        cwd_getter_ = std::move(getter);
        cwd_setter_ = std::move(setter);
    }
    const std::string& cwd() const { return guest_cwd_; }
    void set_cwd(const std::string& c) { guest_cwd_ = c; }
    bool has_cwd_provider() const { return cwd_setter_ != nullptr; }
    bool apply_chdir(const std::string& path) {
        if (cwd_setter_) return cwd_setter_(path);
        return false;
    }
    // Expose the maps provider for /proc/self/maps.
    const std::function<std::vector<MapEntry>()>& maps_provider() const {
        return maps_provider_;
    }
    // Expose the cwd getter for getcwd syscall.
    bool has_cwd_getter() const { return cwd_getter_ != nullptr; }
    std::string get_cwd() const {
        return cwd_getter_ ? cwd_getter_() : guest_cwd_;
    }
private:
    std::mutex binder_mu_;
    bool android_services_ = false;
    bool android_properties_ = false;
    uint32_t process_uid_ = 0, process_gid_ = 0;
    std::once_flag property_area_once_;
    std::string property_area_;  // built from the root on first open
    std::unordered_map<std::string, std::shared_ptr<BinderDomain>> binder_domains_;
    std::string binder_broker_;
    std::string elf_path_;
    // Exact host file selected by the ELF loader, independent of guest cwd.
    std::string elf_backing_path_;
    uint64_t initial_stack_ = 0;
    std::function<std::string()> cmdline_provider_;
    int self_pid_ = 1;
    ::arm64emu::FrostGraphics* gfx_ = nullptr;
    ::arm64emu::Audio* audio_ = nullptr;
    std::function<std::vector<MapEntry>()> maps_provider_;
    std::function<std::string()> comm_provider_;
    std::function<std::string()> cwd_getter_;
    std::function<bool(const std::string&)> cwd_setter_;
    std::string guest_cwd_ = "/";
    // Sub-resolvers (defined in yggdrasil/procfs.cpp / SysFS.cpp /
    // DevFS.cpp / HostPaths.cpp). Each returns nullptr if it doesn't handle
    // the path.
    std::unique_ptr<Node> open_procfs(const std::string& path,
                                      int flags, mode_t mode, int* err_out);
    std::unique_ptr<Node> open_sysfs(const std::string& path,
                                     int flags, mode_t mode, int* err_out);
    std::unique_ptr<Node> open_devfs(const std::string& path,
                                     int flags, mode_t mode, int* err_out);
    std::unique_ptr<Node> open_host(const std::string& path,
                                    int flags, mode_t mode, int* err_out);
};
// ── FdTable: guest fd → Node mapping ──────────────────────────────────
//
// threads (each on its own host thread) call openat/close/read/write/dup
// concurrently, all touching `table_`. Without a lock, concurrent insert +
// erase + find on std::unordered_map is undefined behavior — the map can
// be corrupted, causing crashes, wrong fd lookups, or use-after-free of
// Node pointers. All public methods now take `mu_` internally.
class FdTable {
public:
    FdTable();
    ~FdTable() { close_range(0, INT32_MAX); }
    // Allocate a new guest fd holding `node`. Returns the fd, or -errno.
    int allocate(std::shared_ptr<Node> node);
    // Look up the Node for `fd`. Returns nullptr if fd is not open.
    std::shared_ptr<Node> get(int fd) const;
    // Close `fd`. Returns 0 on success, -errno on failure.
    int close(int fd);
    // Duplicate `fd` to the lowest available fd. Returns new fd or -errno.
    // If `min_fd` > 0, scan for the lowest free fd >= min_fd (F_DUPFD).
    int dup(int fd, int min_fd = 0);
    // Duplicate `fd` to `new_fd` (closing `new_fd` if open). Returns
    // `new_fd` on success or -errno.
    int dup2(int fd, int new_fd);
    // Close every open fd in [first, last]. Used by close_range(2).
    // Iterates only over the open fds (not every integer in the range),
    // so a huge range with few open fds is O(open_fds), not O(range).
    void close_range(int first, int last);
    // True if `fd` is open.
    bool is_open(int fd) const;
    // Number of open fds.
    size_t size() const;
    void invalidate_device_mappings(uint64_t addr, uint64_t size);
    int protect_device_mappings(uint64_t addr, uint64_t size, uint64_t prot);
private:
    // guest fd → shared Node. shared_ptr so dup()/dup2() can alias.
    mutable std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<Node>> table_;
    std::vector<std::weak_ptr<Node>> mapping_devices_;
    std::atomic<bool> has_mapping_devices_{false};
    std::vector<std::shared_ptr<Node>> mapping_devices_snapshot();
};
} // namespace arm64emu::yggdrasil
