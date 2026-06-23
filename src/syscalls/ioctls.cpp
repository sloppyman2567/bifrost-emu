// syscalls/ioctls.cpp — ioctl() syscall.
//
// Handles FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO (for /dev/fb0),
// terminal ioctls (TCGETS/TCSETS/etc.), and various stubs.
//
// Extracted verbatim from the original syscalls.cpp.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "syscalls/syscalls.h"
#include "syscalls/syscalls.h"
#include "graphics.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace arm64emu {

int64_t syscall_ioctls(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& graphics_ = emu.graphics_;
    auto ret_host = [&](int64_t r) { cpu.regs[0] = (uint64_t)r; };

    switch (num) {
        case 29: { // ioctl(fd, request, argp) — AArch64 syscall 29
            // TIOCGWINSZ: query terminal window size. musl's isatty()
            // uses this as a fast-path probe — if it succeeds, the fd
            // is considered a tty. We MUST forward to the host so
            // isatty() correctly returns false for pipes/files.
            // (Previously we always returned success with a fake
            // 24x80 size, which made isatty() always return true —
            // breaking musl's stdio buffering decisions.)
            if (a1 == 0x5413 /*TIOCGWINSZ*/) {
                struct winsize ws;
                int r = ::ioctl((int)a0, TIOCGWINSZ, &ws);
                if (r < 0) {
                    ret_host((uint64_t)(int64_t)-errno);
                } else {
                    mem_.write(a2, &ws, sizeof(ws));
                    ret_host(0);
                }
                return 0;
            }
            // Framebuffer ioctls: route to the graphics backend.
            // FBIOGET_VSCREENINFO (0x4600) and FBIOGET_FSCREENINFO
            // (0x4602) are the two that real fb programs query at
            // startup to learn the mode. We allocate a host-side
            // struct, let GraphicsBackend::ioctl fill it, then copy
            // it to the guest buffer.
            if (a1 == FBIOGET_VSCREENINFO || a1 == FBIOGET_FSCREENINFO) {
                if (!graphics_.ready()) {
                    cpu.regs[0] = (uint64_t)(int64_t)-ENODEV;
                    return 0;
                }
                // The two structs are different sizes; we use the
                // larger one as the host buffer to be safe.
                char host_buf[192];  // ample for either struct
                memset(host_buf, 0, sizeof(host_buf));
                int r = graphics_.ioctl((uint32_t)a1, host_buf);
                if (r < 0) {
                    cpu.regs[0] = (uint64_t)(int64_t)r;
                } else {
                    // sizeof(struct fb_var_screeninfo) = 160
                    // sizeof(struct fb_fix_screeninfo) = 80 (LP64)
                    size_t out_sz = (a1 == FBIOGET_VSCREENINFO) ? 160 : 80;
                    mem_.write(a2, host_buf, out_sz);
                    ret_host(0);
                }
                return 0;
            }
            // Terminal attribute ioctls (TCGETS, TCSETS, etc.).
            // Forward to the host so isatty() works correctly: musl's
            // isatty() calls ioctl(fd, TCGETS, &termios) and considers
            // the fd a tty iff that returns 0. Previously we returned
            // 0 for EVERY unknown ioctl, which made isatty() always
            // return true — even for pipes and regular files. That
            // broke musl's stdio: it would pick line-buffered mode
            // for non-tty stdin, and the interactive shell would
            // hang waiting for keyboard input that never came.
            //
            // TCGETS reads the termios struct; TCSETS/TCSETSW/TCSETSF
            // write it. Both structs are identical layout (struct
            // termios, ~60 bytes on Linux). We marshal through a
            // host-side buffer because a2 is a guest virtual address.
            if (a1 == 0x5401 /*TCGETS*/) {
                struct termios t;
                int r = ::ioctl((int)a0, TCGETS, &t);
                if (r == 0) mem_.write(a2, &t, sizeof(t));
                if (r < 0) ret_host((uint64_t)(int64_t)-errno);
                else       ret_host(r);
                return 0;
            }
            if (a1 == 0x5402 /*TCSETS*/ || a1 == 0x5403 /*TCSETSW*/ ||
                a1 == 0x5404 /*TCSETSF*/) {
                struct termios t;
                mem_.read(a2, &t, sizeof(t));
                int r = ::ioctl((int)a0, (unsigned long)a1, &t);
                if (r < 0) ret_host((uint64_t)(int64_t)-errno);
                else       ret_host(r);
                return 0;
            }
            // FIONREAD (0x541B): how many bytes can be read without
            // blocking. Forward to host so guest select/poll loops
            // work correctly.
            if (a1 == 0x541B /*FIONREAD*/) {
                int n = 0;
                int r = ::ioctl((int)a0, FIONREAD, &n);
                if (r == 0) mem_.store<int32_t>(a2, n);
                if (r < 0) ret_host((uint64_t)(int64_t)-errno);
                else       ret_host(r);
                return 0;
            }
            // TIOCGETD / TIOCSETD / TIOCNOTTY etc. — pass through.
            // Anything we don't recognize: return -ENOTTY so callers
            // (especially isatty()) can correctly distinguish ttys
            // from non-ttys.
            ret_host((uint64_t)(int64_t)-ENOTTY);
            return 0;
        }

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
