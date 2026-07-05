// yggdrasil/yggdrasil.hpp — the world-tree: guest path → Node resolver.
//
// Yggdrasil is the VFS subsystem of bifrost-emu. In Norse cosmology,
// Yggdrasil is the world-tree that connects the nine realms; here it
// connects the guest's path namespace to four "worlds":
//
//   1. ProcFS    — /proc/self/{exe,cmdline,maps,status,auxv,environ},
//                  /proc/{meminfo,cpuinfo,version}, /proc/sys/kernel/osrelease
//   2. DevFS     — /dev/{null,zero,urandom,random,tty,fb0,stdin,stdout,stderr,
//                  dsp,snd,ptmx,pts/*}
//   3. PathRemap — BIFROST_ROOT sandbox (if set)
//   4. Host      — openat() passthrough on the (possibly remapped) host path
//
//   Guest path ──► Yggdrasil::open() ──► Node* ──► FdTable::allocate()
//                                                    │
//                                                    ▼
//                                                guest fd
//
// The FdTable owns the shared_ptr<Node> for each guest fd. close() drops
// the refcount; dup()/dup2() create new fds pointing to the same Node.
//
// v1.4.5-alpha: renamed from `VFS` to `Yggdrasil` to give the subsystem
// an identity (matching the Norse theme of bifrost + FrostJIT). The
// class names `VFS`, `VNode`, `HostVNode`, etc. were renamed to
// `Yggdrasil`, `Node`, `HostNode`, etc. The file layout was split
// from `vfs/vfs.{h,cpp}` + `vfs/vfs_table.{h,cpp}` into per-concern
// files under `yggdrasil/`. No behavior change beyond the
// improvements documented in CHANGELOG.md.
#pragma once

#include "bifrost/types.hpp"
#include "yggdrasil/node.hpp"

#include <cstdint>
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
// live in arm64emu:: — graphics.hpp and audio/audio.h). These must be
// declared at the arm64emu scope so the yggdrasil namespace can refer
// to them without qualification.
namespace arm64emu {
    class FrostGraphics;
    class Audio;
    class Memory;
}

namespace arm64emu::yggdrasil {

// ── Yggdrasil: path → Node resolver ───────────────────────────────────
class Yggdrasil {
public:
    Yggdrasil();

    // Resolve a guest path to a Node. Returns nullptr on failure (with
    // errno set via the `*err_out` parameter).
    //
    // Resolution order:
    //   1. ProcFS  (/proc/*)
    //   2. DevFS   (/dev/*)
    //   3. PathRemap (BIFROST_ROOT sandbox)
    //   4. Host passthrough (openat on the remapped path)
    std::unique_ptr<Node> open(const std::string& guest_path,
                               int flags, mode_t mode, int* err_out);

    // Read a NUL-terminated path string from guest memory. Reads at most
    // 4096 bytes to prevent runaway reads from bad pointers.
    static std::string read_path(::arm64emu::Memory& mem, uint64_t addr);

    // Remap a guest path to a host path using BIFROST_ROOT.
    // (Defined in yggdrasil/host.cpp — exposed here so syscall handlers
    // can call it for path-based syscalls like unlinkat/renameat/etc.)
    static std::string remap_path(const std::string& guest_path);

    // Set the ELF path (used by /proc/self/exe). Called once from
    // Emulator::load_elf_file().
    void set_elf_path(const std::string& path) { elf_path_ = path; }
    const std::string& elf_path() const { return elf_path_; }

    // Set the argv vector (used by /proc/self/cmdline). Called once
    // from Emulator::load_elf_file().
    void set_argv(const std::vector<std::string>& argv) { argv_ = argv; }

    // Wire up the graphics backend (for /dev/fb0). May be null if the
    // emulator was built without SDL2 and the guest never opens /dev/fb0.
    void set_graphics(::arm64emu::FrostGraphics* gfx) { gfx_ = gfx; }

    // Wire up the audio backend (for /dev/dsp, /dev/snd). May be null.
    void set_audio(::arm64emu::Audio* audio) { audio_ = audio; }

    // ── Live memory layout for /proc/self/maps ─────────────────────
    // The Emulator registers a callback that returns the current
    // allocations + brk range so /proc/self/maps can show a real layout
    // instead of the old hardcoded 5-line string. The callback returns
    // a vector of (start, size, label) tuples; label is "rwxp" style
    // permissions + optional [stack]/[heap]/[anon] annotation.
    struct MapEntry {
        uint64_t    start;
        uint64_t    end;
        char        perms[5];  // "rwxp\0"
        std::string label;     // "" or "[stack]"/"[heap]"/etc.
    };
    void set_maps_provider(std::function<std::vector<MapEntry>()> cb) {
        maps_provider_ = std::move(cb);
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
    std::string elf_path_;
    std::vector<std::string> argv_;
    ::arm64emu::FrostGraphics* gfx_ = nullptr;
    ::arm64emu::Audio* audio_ = nullptr;
    std::function<std::vector<MapEntry>()> maps_provider_;
    std::function<std::string()> cwd_getter_;
    std::function<bool(const std::string&)> cwd_setter_;
    std::string guest_cwd_ = "/";

    // Sub-resolvers (defined in yggdrasil/procfs.cpp / devfs.cpp /
    // host.cpp). Each returns nullptr if it doesn't handle the path.
    std::unique_ptr<Node> open_procfs(const std::string& path,
                                      int flags, mode_t mode, int* err_out);
    std::unique_ptr<Node> open_devfs(const std::string& path,
                                     int flags, mode_t mode, int* err_out);
    std::unique_ptr<Node> open_host(const std::string& path,
                                    int flags, mode_t mode, int* err_out);
};

// ── FdTable: guest fd → Node mapping ──────────────────────────────────
//
// BUGFIX (v1.4.5-alpha): added a mutex to protect `table_`. Multiple guest
// threads (each on its own host thread) call openat/close/read/write/dup
// concurrently, all touching `table_`. Without a lock, concurrent insert +
// erase + find on std::unordered_map is undefined behavior — the map can
// be corrupted, causing crashes, wrong fd lookups, or use-after-free of
// Node pointers. All public methods now take `mu_` internally.
class FdTable {
public:
    FdTable();

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

private:
    // guest fd → shared Node. shared_ptr so dup()/dup2() can alias.
    mutable std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<Node>> table_;
};

} // namespace arm64emu::yggdrasil
