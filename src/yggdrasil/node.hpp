// yggdrasil/node.hpp — abstract file handle (the "node" of the world-tree).
//
// Yggdrasil is the VFS subsystem of bifrost-emu. In Norse cosmology,
// Yggdrasil is the world-tree that connects the nine realms; here it
// connects the guest's path namespace to the host filesystem, procfs,
// devfs, and the BIFROST_ROOT sandbox.
//
// A Node is the abstract base for every open file in the guest. Each
// concrete subclass implements the relevant subset of operations:
//   HostNode     — wraps a host fd (read/write/lseek/fstat via host syscalls)
//   MemfdNode    — wraps a memfd_create'd fd (for synthetic /proc content)
//   FbNode       — wraps the GraphicsBackend's /dev/fb0 fd
//   StdioNode    — wraps stdin/stdout/stderr via dup()
//   AudioNode    — wraps the Audio backend (/dev/dsp, /dev/snd)
//   DirNode      — synthetic directory entry (for /proc and /dev listings)
//
// The FdTable owns the shared_ptr<Node> for each guest fd. close()
// drops the refcount; dup()/dup2() create new fds pointing to the
// same Node (shared ownership).
#pragma once
#include "bifrost/types.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
// Forward-declare arm64emu::Memory (used by the ioctl() virtual method).
// The real definition lives in arm64emu:: (core/memory.h).
namespace arm64emu {
    class Memory;
}
namespace arm64emu::yggdrasil {
// ── Node: abstract file handle ────────────────────────────────────────
class Node {
public:
    virtual ~Node() = default;
    // Read up to `n` bytes at `off` into `buf`. Returns bytes read, or
    // -errno on failure. If `off` is -1, reads from the current file
    // position (which the Node tracks internally).
    virtual ssize_t read(uint64_t off, void* buf, size_t n) = 0;
    // Write `n` bytes from `buf` at `off`. Returns bytes written, or
    // -errno on failure. If `off` is -1, writes at the current position.
    virtual ssize_t write(uint64_t off, const void* buf, size_t n) = 0;
    // Seek to `off` per `whence` (SEEK_SET/SEEK_CUR/SEEK_END). Returns
    // the new offset, or -errno on failure.
    virtual ssize_t lseek(int64_t off, int whence) = 0;
    // Fill in a struct stat. Returns 0 on success, -errno on failure.
    virtual int fstat(struct stat* st) = 0;
    // Dispatch an ioctl. Returns 0 on success, -errno on failure, or
    // INT_MIN if this Node does not handle the request (so the caller
    // can fall back to host passthrough or return -ENOTTY). Default
    // implementation returns INT_MIN (not handled). The `argp` is a
    // guest virtual address; the Node implementation is responsible
    // for marshalling through the Memory if it needs to read/write
    // the argp struct.
    //
    // Added in v1.4.5-alpha (Yggdrasil rename). Previously ioctls.cpp
    // dispatched on the request code with a big if-else chain and
    // guessed the fd type (framebuffer? tty? something else?). Now
    // each Node subclass owns its own ioctl handling: FbNode handles
    // FBIOGET_*, StdioNode/HostNode handle TCGETS/FIONREAD/etc., and
    // the syscall layer just calls node->ioctl().
    static constexpr int IOCTL_NOT_HANDLED = -0x7fffffff;
    virtual int ioctl(uint32_t /*request*/, uint64_t /*argp*/,
                      Memory& /*mem*/) { return IOCTL_NOT_HANDLED; }
    // Whether this Node supports seeking (pipes/sockets don't).
    virtual bool seekable() const { return true; }
    // Hint for fcntl F_GETFL — default to O_RDWR.
    virtual int flags() const { return O_RDWR; }
    // Return the host file descriptor for passthrough operations
    // (getdents64, etc.). Returns -1 if this Node has no host fd.
    virtual int host_fd() const { return -1; }
    // Whether this Node represents a directory (for getdents64 dispatch).
    // DirNode overrides to return true; everything else returns false.
    // Added in v1.4.5-alpha so getdents64 can call into the Node instead
    // of requiring a host fd (synthetic /proc and /dev listings work
    // without a host fd).
    virtual bool is_dir() const { return false; }
    // Read directory entries (only valid when is_dir() returns true).
    // Writes linux_dirent64 records into `buf`, returns total bytes
    // written, 0 at end-of-directory, or -errno on failure. `off` is
    // the seek offset (cookie) returned by the previous getdents call,
    // or 0 for the first call.
    //
    // Default implementation returns -ENOTDIR (caller should have
    // checked is_dir() first).
    virtual ssize_t getdents(uint64_t /*off*/, void* /*buf*/, size_t /*n*/) {
        return -ENOTDIR;
    }
};
} // namespace arm64emu::yggdrasil
