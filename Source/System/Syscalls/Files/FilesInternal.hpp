// FilesInternal.hpp — helpers and family handlers shared by the file,
// descriptor and socket syscall sources. Include only from
// Source/System/Syscalls/Files/.
#pragma once
#include "System/Core/Emulator.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
namespace arm64emu {
// ── File syscall families (dispatched in this order by syscall_fs) ──────
int64_t syscall_fs_descriptors(Emulator& emu, CPU& cpu, uint64_t num); // Descriptors.cpp
int64_t syscall_fs_paths(Emulator& emu, CPU& cpu, uint64_t num);       // Paths.cpp
int64_t syscall_fs_metadata(Emulator& emu, CPU& cpu, uint64_t num);    // Metadata.cpp
int64_t syscall_fs_transfer(Emulator& emu, CPU& cpu, uint64_t num);    // Transfer.cpp
// ── Shared helpers (FileSystem.cpp) ─────────────────────────────────────
// AArch64 open flags ↔ host open flags; used by openat, pipe2 and fcntl.
int open_flags_to_host(int guest);
int open_flags_to_guest(int host);
// Append one record to the BIFROST_XSEQLOG byte-sequence log.
void xseq_append(char dir, const uint8_t* p, size_t n);
// Resolve a guest dirfd for host *at calls: AT_FDCWD passes through, -1
// means invalid or virtual (no host fd).
int resolve_dirfd(FdTable& fds, uint64_t guest_dirfd);
// Guest view of /proc/<pid>/exe and /proc/<pid>/fd/<n>.
bool is_proc_exe_path(const std::string& p);
int proc_link_fd(const std::string& path, std::string* rest = nullptr);
void proc_exe_stat(const std::string& elf_path, bool nofollow, struct stat* st);
// ── Socket-style descriptor helpers (EventIO.cpp, Sockets.cpp) ─────────
// Returns the host fd backing a guest fd, or -1 if the fd is invalid or
// not backed by a host fd. For fds that were never registered in the
// FdTable (e.g. stdin/stdout/stderr which are HostNode-wrapped but owned
// by StdioNode), fall back to using the guest fd as the host fd. This
// preserves backward compatibility for any code path that somehow
// obtained a raw host fd.
inline int resolve_sock_fd(Emulator& emu, uint64_t guest_fd) {
    int gfd = static_cast<int>(static_cast<int64_t>(guest_fd));
    if (gfd < 0) return -1;
    auto node = emu.fds().get(gfd);
    if (node) {
        int hfd = node->host_fd();
        return hfd;  // may be -1 for virtual nodes
    }
    return gfd;
}
// Register a freshly-created host fd in the FdTable and return the guest
// fd. Uses O_RDWR as the default flags (sockets don't have a meaningful
// "flags" field at creation time — they're full-duplex).
inline int register_host_fd(Emulator& emu, int hfd, int flags = O_RDWR) {
    if (hfd < 0) return hfd;
    return emu.fds().allocate(std::make_shared<yggdrasil::HostNode>(hfd, flags));
}
} // namespace arm64emu
