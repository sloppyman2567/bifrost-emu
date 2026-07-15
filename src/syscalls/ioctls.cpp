// syscalls/ioctls.cpp — ioctl() syscall.
//
// v1.4.5-alpha: rewritten to dispatch via Node::ioctl(). Previously
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
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/node.hpp"
#include "frost/graphics.hpp"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
namespace arm64emu {
int64_t syscall_ioctls(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    auto& mem_ = emu.mem_;
    auto& fds_ = emu.fds_;
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
            int r = node->ioctl(static_cast<uint32_t>(a1), a2, mem_);
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
