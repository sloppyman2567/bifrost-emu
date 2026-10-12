// Transfer.cpp — kernel-side data movement between descriptors:
// vmsplice/splice/tee and sendfile, passed through to the host after
// resolving guest descriptors via the FdTable.
//
// Dispatched from syscall_fs() (FileSystem.cpp).
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Syscalls/Files/FilesInternal.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>
namespace arm64emu {
int64_t syscall_fs_transfer(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    auto& mem_ = emu.mem();
    auto& fds_ = emu.fds();
    switch (num) {
        // ── vmsplice / splice / tee (syscalls 75/76/77) ──────────────
        // These are real AArch64 syscalls that move data through pipes.
        // We implement them as host passthroughs, resolving guest fds via
        // FdTable and marshaling iovec arrays from guest memory.
        case 75: { // vmsplice(fd, iov, nr_segs, flags)
            // Resolve guest fd to host fd via FdTable.
            auto node = fds_.get(static_cast<int>(a0));
            int hfd = node ? node->host_fd() : static_cast<int>(a0);
            if (hfd < 0) { ret_err(EBADF); return 0; }
            // Marshal iovec array from guest memory.
            // Guest iovec: { void* iov_base; size_t iov_len; } — 16 bytes each.
            uint64_t iov_g = a1;
            uint64_t nr_segs = a2;
            if (nr_segs > 1024) nr_segs = 1024;  // sanity cap
            std::vector<struct iovec> iovs(nr_segs);
            std::vector<std::vector<uint8_t>> iov_bufs(nr_segs);
            for (uint64_t i = 0; i < nr_segs; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_g + i * 16);
                uint64_t len = mem_.load<uint64_t>(iov_g + i * 16 + 8);
                if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
                iov_bufs[i].resize(len);
                if (len) mem_.read(base, iov_bufs[i].data(), len);
                iovs[i].iov_base = iov_bufs[i].data();
                iovs[i].iov_len = len;
            }
            ssize_t r = ::vmsplice(hfd, iovs.data(), nr_segs,
                                   static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 76: { // splice(fd_in, off_in, fd_out, off_out, len, flags)
            // Resolve guest fds to host fds via FdTable.
            auto node_in = fds_.get(static_cast<int>(a0));
            int hfd_in = node_in ? node_in->host_fd() : static_cast<int>(a0);
            if (hfd_in < 0) { ret_err(EBADF); return 0; }
            auto node_out = fds_.get(static_cast<int>(a2));
            int hfd_out = node_out ? node_out->host_fd() : static_cast<int>(a2);
            if (hfd_out < 0) { ret_err(EBADF); return 0; }
            // off_in/off_out are pointers to loff_t (64-bit) in guest memory.
            // They can be NULL (for pipes). Read them into host loff_t.
            loff_t off_in = 0, off_out = 0;
            loff_t* poff_in = nullptr;
            loff_t* poff_out = nullptr;
            if (a1) {
                off_in = static_cast<loff_t>(mem_.load<uint64_t>(a1));
                poff_in = &off_in;
            }
            if (a3) {
                off_out = static_cast<loff_t>(mem_.load<uint64_t>(a3));
                poff_out = &off_out;
            }
            ssize_t r = ::splice(hfd_in, poff_in, hfd_out, poff_out,
                                 static_cast<size_t>(a4),
                                 static_cast<int>(a5));
            if (r < 0) { ret_errno(); return 0; }
            // Write back offsets if the guest provided pointers.
            if (a1) mem_.store<uint64_t>(a1, static_cast<uint64_t>(off_in));
            if (a3) mem_.store<uint64_t>(a3, static_cast<uint64_t>(off_out));
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 77: { // tee(fd_in, fd_out, len, flags)
            // Both fds must be pipes. Resolve via FdTable.
            auto node_in = fds_.get(static_cast<int>(a0));
            int hfd_in = node_in ? node_in->host_fd() : static_cast<int>(a0);
            if (hfd_in < 0) { ret_err(EBADF); return 0; }
            auto node_out = fds_.get(static_cast<int>(a1));
            int hfd_out = node_out ? node_out->host_fd() : static_cast<int>(a1);
            if (hfd_out < 0) { ret_err(EBADF); return 0; }
            ssize_t r = ::tee(hfd_in, hfd_out,
                              static_cast<size_t>(a2),
                              static_cast<int>(a3));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 71: { // sendfile(out_fd, in_fd, offset, count) — AArch64 71
            // BUGFIX: previously called ::sendfile(guest_fd, guest_fd, ...)
            // directly, bypassing FdTable. Resolve both fds via FdTable so
            // virtual fds (memfd-backed /proc/*, /dev/fb0) work.
            auto out_node = fds_.get(static_cast<int>(a0));
            auto in_node  = fds_.get(static_cast<int>(a1));
            if (!out_node || !in_node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF));
                return 0;
            }
            int out_hfd = out_node->host_fd();
            int in_hfd  = in_node->host_fd();
            if (out_hfd < 0 || in_hfd < 0) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF));
                return 0;
            }
            off_t off = 0;
            off_t *offp = nullptr;
            if (a2 != 0) {
                try { off = mem_.load<off_t>(a2); offp = &off; } catch (...) {}
            }
            ssize_t r = ::sendfile(out_hfd, in_hfd, offp, a3);
            if (r < 0) { ret_errno(); return 0; }
            if (offp && a2) { try { mem_.store<off_t>(a2, off); } catch (...) {} }
            ret_host(static_cast<uint64_t>(r)); return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
