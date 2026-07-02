// vfs/vfs.h — Virtual File System abstraction for bifrost-emu.
//
// Replaces the inline /proc//dev/ else-if chains that used to live in
// syscalls.cpp's openat handler. Adding a new virtual file or device
// now means registering a handler in vfs.cpp (or procfs.cpp / devfs.cpp)
// — syscalls.cpp stays untouched.
//
// ── Architecture ───────────────────────────────────────────────────────
//
//   Guest path ──► VFS::open() ──► VNode* ──► FdTable::allocate()
//                                              │
//                                              ▼
//                                          guest fd
//
// VFS::open() consults, in order:
//   1. ProcFS  — /proc/self/{exe,cmdline,maps,status,auxv,environ},
//                /proc/{meminfo,cpuinfo,version}, /proc/sys/kernel/osrelease
//   2. DevFS   — /dev/{null,zero,urandom,random,tty,fb0,stdin,stdout,stderr}
//   3. PathRemap — BIFROST_ROOT sandbox (if set)
//   4. Host passthrough — openat() on the (possibly remapped) host path
//
// Each VNode subclass implements the relevant subset of operations:
//   HostVNode     — wraps a host fd (read/write/lseek/fstat via host syscalls)
//   MemfdVNode    — wraps a memfd_create'd fd (for synthetic /proc content)
//   FbVNode       — wraps the GraphicsBackend's /dev/fb0 fd
//   StdioVNode    — wraps stdin/stdout/stderr via dup()
//
// The FdTable owns the VNode* for each guest fd. close() destroys it;
// dup()/dup2() create new fds pointing to the same VNode (shared ownership
// via shared_ptr).
#pragma once

#include "bifrost/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu {

class GraphicsBackend;
class Audio;
class Memory;

// ── VNode: abstract file handle ────────────────────────────────────────
class VNode {
public:
    virtual ~VNode() = default;

    // Read up to `n` bytes at `off` into `buf`. Returns bytes read, or
    // -errno on failure. If `off` is -1, reads from the current file
    // position (which the VNode tracks internally).
    virtual ssize_t read(uint64_t off, void* buf, size_t n) = 0;

    // Write `n` bytes from `buf` at `off`. Returns bytes written, or
    // -errno on failure. If `off` is -1, writes at the current position.
    virtual ssize_t write(uint64_t off, const void* buf, size_t n) = 0;

    // Seek to `off` per `whence` (SEEK_SET/SEEK_CUR/SEEK_END). Returns
    // the new offset, or -errno on failure.
    virtual ssize_t lseek(int64_t off, int whence) = 0;

    // Fill in a struct stat. Returns 0 on success, -errno on failure.
    virtual int fstat(struct stat* st) = 0;

    // Whether this VNode supports seeking (pipes/sockets don't).
    virtual bool seekable() const { return true; }

    // Hint for fcntl F_GETFL — default to O_RDWR.
    virtual int flags() const { return O_RDWR; }

    // Return the host file descriptor for passthrough operations
    // (getdents64, etc.). Returns -1 if this VNode has no host fd.
    virtual int host_fd() const { return -1; }
};

// ── VFS: path → VNode resolver ─────────────────────────────────────────
class VFS {
public:
    VFS();

    // Resolve a guest path to a VNode. Returns nullptr on failure (with
    // errno set via the `*err_out` parameter).
    //
    // Resolution order:
    //   1. ProcFS  (/proc/*)
    //   2. DevFS   (/dev/*)
    //   3. PathRemap (BIFROST_ROOT sandbox)
    //   4. Host passthrough (openat on the remapped path)
    std::unique_ptr<VNode> open(const std::string& guest_path,
                                int flags, mode_t mode, int* err_out);

    // Read a NUL-terminated path string from guest memory. Reads at most
    // 4096 bytes to prevent runaway reads from bad pointers.
    static std::string read_path(Memory& mem, uint64_t addr);

    // Remap a guest path to a host path using BIFROST_ROOT.
    // (Defined in vfs_host.cpp — exposed here so syscall handlers can
    // call it for path-based syscalls like unlinkat/renameat/etc.)
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
    void set_graphics(GraphicsBackend* gfx) { gfx_ = gfx; }

    // Wire up the audio backend (for /dev/dsp, /dev/snd). May be null.
    void set_audio(Audio* audio) { audio_ = audio; }

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
    GraphicsBackend* gfx_ = nullptr;
    Audio* audio_ = nullptr;
    std::function<std::vector<MapEntry>()> maps_provider_;
    std::function<std::string()> cwd_getter_;
    std::function<bool(const std::string&)> cwd_setter_;
    std::string guest_cwd_ = "/";

    // Sub-resolvers (defined in vfs_procfs.cpp / vfs_devfs.cpp /
    // vfs_host.cpp). Each returns nullptr if it doesn't handle the path.
    std::unique_ptr<VNode> open_procfs(const std::string& path,
                                       int flags, mode_t mode, int* err_out);
    std::unique_ptr<VNode> open_devfs(const std::string& path,
                                      int flags, mode_t mode, int* err_out);
    std::unique_ptr<VNode> open_host(const std::string& path,
                                     int flags, mode_t mode, int* err_out);
};

// ── FdTable: guest fd → VNode mapping ──────────────────────────────────
class FdTable {
public:
    FdTable();

    // Allocate a new guest fd holding `node`. Returns the fd, or -errno.
    int allocate(std::shared_ptr<VNode> node);

    // Look up the VNode for `fd`. Returns nullptr if fd is not open.
    std::shared_ptr<VNode> get(int fd);

    // Close `fd`. Returns 0 on success, -errno on failure.
    int close(int fd);

    // Duplicate `fd` to the lowest available fd. Returns new fd or -errno.
    int dup(int fd);

    // Duplicate `fd` to `new_fd` (closing `new_fd` if open). Returns
    // `new_fd` on success or -errno.
    int dup2(int fd, int new_fd);

    // True if `fd` is open.
    bool is_open(int fd) const { return table_.count(fd) != 0; }

    // Number of open fds.
    size_t size() const { return table_.size(); }

private:
    // guest fd → shared VNode. shared_ptr so dup()/dup2() can alias.
    std::unordered_map<int, std::shared_ptr<VNode>> table_;

    int next_fd_ = 3;  // 0/1/2 reserved for stdin/stdout/stderr
};

} // namespace arm64emu
