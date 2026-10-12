// Ioctl.cpp — ioctl() syscall.
//
// rewritten to dispatch via Node::ioctl(). Previously
// this file had a big if-else chain on the request code (TIOCGWINSZ,
// FBIOGET_*, TCGETS, etc.) and guessed the fd type. Now each Node
// subclass owns its ioctls:
//   - FbNode::ioctl()      handles FBIOGET_VSCREENINFO/FSCREENINFO
//   - HostNode::ioctl()    handles TIOCGWINSZ/TCGETS/TCSETS/FIONREAD +
//                          pass-through for anything else
//   - StdioNode::ioctl()   same as HostNode (delegates to fd 0/1/2)
//   - MemfdNode/DirNode/etc. return IOCTL_NOT_HANDLED → -ENOTTY
//
// The syscall layer just calls node->ioctl(request, argp, mem). If it
// returns Node::IOCTL_NOT_HANDLED, we return -ENOTTY to the guest.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/Signals.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/Node.hpp"
#include "System/Graphics/Display/Graphics.hpp"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
namespace arm64emu {
int64_t syscall_ioctls(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    auto& mem_ = emu.mem();
    auto& fds_ = emu.fds();
    switch (num) {
        case 29: { // ioctl(fd, request, argp) — AArch64 syscall 29
            auto node = fds_.get(static_cast<int>(a0));
            if (!node) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EBADF));
                return 0;
            }
            // Dispatch to the Node. Each Node subclass implements
            // ioctl() for the requests it understands; everything else
            // returns Node::IOCTL_NOT_HANDLED, which we translate to
            // -ENOTTY (the POSIX "Inappropriate ioctl for device" error).
            int r = node->ioctl_thread(static_cast<uint32_t>(a1), a2, mem_, cpu, fds_);
            if (r == yggdrasil::Node::IOCTL_NOT_HANDLED) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-ENOTTY));
            } else if (r < 0) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
            } else {
                cpu.regs[0] = static_cast<uint64_t>(r);
            }
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
