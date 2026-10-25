// TerminalIoctls.hpp — shared terminal ioctl dispatch.
//
// Both HostNode and StdioNode implement ioctl() by forwarding terminal
// ioctls (TIOCGWINSZ, TCGETS, TCSETS/TCSETSW/TCSETSF, FIONREAD) to the
// underlying host fd, marshalling the struct through the guest Memory.
// The implementations were duplicated verbatimextracts them
// into a single shared helper.
//
// This header also defines the AArch64 Linux ioctl request numbers as
// named constants, replacing the magic 0x5413/0x5401/0x541B hex values
// scattered through the codebase. The values are identical on x86-64
// and AArch64 (they're part of the Linux UAPI, not architecture-
// specific).
//
// NOTE: We use the prefix `REQ_` (not just `TIOCGWINSZ` etc.) because
// the system headers `<sys/ioctl.h>` define these names as preprocessor
// macros. A bare `TIOCGWINSZ` would be macro-expanded before the
// compiler sees it, breaking namespace-qualified access
// (`ioctl_num::TIOCGWINSZ` → `ioctl_num::0x5413` → syntax error).
#pragma once
#include "Bifrost/Types.hpp"
#include "System/Memory/Memory.hpp"  // full Memory definition (for read/write/store)
#include <cstdint>
#include <cstdio>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
// Linux asm-generic TCGETS/TCSETS wire layout (also x86-64's kernel ABI).
// libc's struct termios can be larger and must never cross this boundary.
struct KernelTermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line, c_cc[19];
};
static_assert(sizeof(KernelTermios) == 36);
// struct termios2 (TCGETS2/TCSETS2*): the same fields plus the numeric line speeds. glibc 2.42
// and newer read and write the terminal through these (isatty, tcgetattr, tcsetattr), so a guest
// that only gets TCGETS answered sees every terminal as "not a tty".
struct KernelTermios2 {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line, c_cc[19];
    uint32_t c_ispeed, c_ospeed;
};
static_assert(sizeof(KernelTermios2) == 44);
// ── AArch64 Linux ioctl request numbers (from <asm-generic/ioctls.h>) ──
// These match the values the guest AArch64 binary uses when it issues
// an ioctl syscall. We hardcode them (instead of relying on the host's
// <asm/ioctls.h>) because the host is x86-64 and the values happen to
// be identical, but relying on that coincidence is fragile — explicit
// is better.
//
// Reference: Linux UAPI, include/uapi/asm-generic/ioctls.h
namespace ioctl_num {
    constexpr uint32_t REQ_TCGETS     = 0x5401;  // read termios
    constexpr uint32_t REQ_TCSETS     = 0x5402;  // write termios
    constexpr uint32_t REQ_TCSETSW    = 0x5403;  // drain output, then write
    constexpr uint32_t REQ_TCSETSF    = 0x5404;  // drain input, then write
    // _IOR/_IOW('T', 0x2A..0x2D, struct termios2), 44 bytes. The host is x86-64, whose kernel
    // uses the same numbers and layout, so they are forwarded as they are (glibc's <sys/ioctl.h>
    // does not define them).
    constexpr uint32_t REQ_TCGETS2    = 0x802C542A;  // read termios2
    constexpr uint32_t REQ_TCSETS2    = 0x402C542B;  // write termios2
    constexpr uint32_t REQ_TCSETSW2   = 0x402C542C;  // drain output, then write
    constexpr uint32_t REQ_TCSETSF2   = 0x402C542D;  // drain input, then write
    constexpr uint32_t REQ_TCSBRK     = 0x5409;  // tcdrain / tcsendbreak(0): value argument
    constexpr uint32_t REQ_TCXONC     = 0x540A;  // tcflow: value argument
    constexpr uint32_t REQ_TCFLSH     = 0x540B;  // tcflush: value argument
    constexpr uint32_t REQ_TIOCEXCL   = 0x540C;  // exclusive open
    constexpr uint32_t REQ_TIOCNXCL   = 0x540D;  // clear exclusive open
    constexpr uint32_t REQ_TIOCSCTTY  = 0x540E;  // set controlling tty
    constexpr uint32_t REQ_TIOCGPGRP  = 0x540F;  // get foreground pgrp
    constexpr uint32_t REQ_TIOCSPGRP  = 0x5410;  // set foreground pgrp
    constexpr uint32_t REQ_TIOCOUTQ   = 0x5411;  // bytes queued for output
    constexpr uint32_t REQ_TIOCGWINSZ = 0x5413;  // get window size
    constexpr uint32_t REQ_TIOCSWINSZ = 0x5414;  // set window size (the tty then signals SIGWINCH)
    constexpr uint32_t REQ_FIONREAD   = 0x541B;  // bytes available to read
    constexpr uint32_t REQ_TIOCPKT    = 0x5420;  // packet mode on a pty master
    constexpr uint32_t REQ_FIONBIO    = 0x5421;  // set/clear non-blocking I/O
    constexpr uint32_t REQ_TIOCNOTTY  = 0x5422;  // detach controlling tty
    constexpr uint32_t REQ_TCSBRKP    = 0x5425;  // tcsendbreak(n): value argument
    constexpr uint32_t REQ_TIOCGSID   = 0x5429;  // get session id
    // _IOR('T', 0x30, unsigned) and friends: the pty master's setup requests.
    constexpr uint32_t REQ_TIOCGPTN   = 0x80045430;  // slave number of a pty master
    constexpr uint32_t REQ_TIOCSPTLCK = 0x40045431;  // lock (1) or unlock (0) the slave
    constexpr uint32_t REQ_TIOCGPTLCK = 0x80045439;  // is the slave locked
    constexpr uint32_t REQ_TIOCGEXCL  = 0x80045440;  // is the terminal opened exclusively
    constexpr uint32_t REQ_TIOCGPTPEER = 0x5441;     // open the slave of a master: returns an fd
}  // namespace ioctl_num
// ── dispatch_terminal_ioctl — shared ioctl handler ─────────────────────
// Forward terminal-related ioctls to the host fd, marshalling the
// argument struct through the guest Memory. Returns:
//   - 0 on success
//   - -errno on host ioctl failure
//   - Node::IOCTL_NOT_HANDLED if `request` is not a known terminal ioctl
//     (so the caller can fall through to host passthrough or return
//     -ENOTTY).
//
// This helper is used by both HostNode::ioctl() and StdioNode::ioctl().
// Keeping the dispatch logic in one place ensures the two paths can't
// drift (which was theoriginal bug — TIOCGWINSZ worked on
// host fds but not on stdin/stdout/stderr).
inline int dispatch_terminal_ioctl(int host_fd, uint32_t request,
                                    uint64_t argp, Memory& mem) {
    // TCFLSH, TCSBRK, TCXONC, TCSBRKP and the exclusive-mode requests take a value (often 0), not a pointer.
    const bool value_argument =
        request == ioctl_num::REQ_TCSBRK || request == ioctl_num::REQ_TCXONC ||
        request == ioctl_num::REQ_TCFLSH || request == ioctl_num::REQ_TCSBRKP ||
        request == ioctl_num::REQ_TIOCEXCL || request == ioctl_num::REQ_TIOCNXCL ||
        request == ioctl_num::REQ_TIOCSCTTY || request == ioctl_num::REQ_TIOCNOTTY;
    if (argp == 0 && !value_argument) return -EFAULT;
    switch (request) {
        case ioctl_num::REQ_TIOCGWINSZ: {
            struct winsize ws;
            int r = ::ioctl(host_fd, TIOCGWINSZ, &ws);
            if (r < 0) return -errno;
            mem.write(argp, &ws, sizeof(ws));
            return 0;
        }
        case ioctl_num::REQ_TIOCSWINSZ: {
            struct winsize ws{};
            mem.read(argp, &ws, sizeof(ws));  // four 16-bit fields, the same on every architecture
            return ::ioctl(host_fd, TIOCSWINSZ, &ws) < 0 ? -errno : 0;
        }
        // An int the kernel writes (the pty number, lock state, queued bytes, line discipline).
        case ioctl_num::REQ_TIOCGPTN:
        case ioctl_num::REQ_TIOCGPTLCK:
        case ioctl_num::REQ_TIOCGEXCL:
        case ioctl_num::REQ_TIOCOUTQ: {
            uint32_t v = 0;
            if (::ioctl(host_fd, request, &v) < 0) return -errno;
            mem.store<uint32_t>(argp, v);
            return 0;
        }
        // An int the kernel reads (lock/unlock, packet mode).
        case ioctl_num::REQ_TIOCSPTLCK:
        case ioctl_num::REQ_TIOCPKT: {
            int v = mem.load<int32_t>(argp);
            return ::ioctl(host_fd, request, &v) < 0 ? -errno : 0;
        }
        // The argument is the value itself, not a pointer.
        case ioctl_num::REQ_TCSBRK:
        case ioctl_num::REQ_TCXONC:
        case ioctl_num::REQ_TCFLSH:
        case ioctl_num::REQ_TCSBRKP:
            return ::ioctl(host_fd, request, static_cast<unsigned long>(static_cast<int>(argp))) < 0 ? -errno : 0;
        // The guest's session is virtual (plain runs present PID 1 as the session leader, see
        // TIOCGSID below), so acquiring a controlling terminal is the host's answer when the
        // emulator really is a leader, and success when the kernel only says "you are not a
        // session leader". Giving the terminal up is never forwarded: it would detach the
        // emulator's own terminal.
        case ioctl_num::REQ_TIOCSCTTY: {
            if (::ioctl(host_fd, request, static_cast<unsigned long>(static_cast<int>(argp))) < 0 && errno != EPERM)
                return -errno;
            return 0;
        }
        case ioctl_num::REQ_TIOCNOTTY:
            return 0;
        case ioctl_num::REQ_TIOCEXCL:
        case ioctl_num::REQ_TIOCNXCL:
            return ::ioctl(host_fd, request) < 0 ? -errno : 0;
        case ioctl_num::REQ_TCGETS: {
            KernelTermios t{};
            int r = ::ioctl(host_fd, TCGETS, &t);
            if (r < 0) return -errno;
            mem.write(argp, &t, sizeof(t));
            return 0;
        }
        case ioctl_num::REQ_TCSETS:
        case ioctl_num::REQ_TCSETSW:
        case ioctl_num::REQ_TCSETSF: {
            KernelTermios t{};
            mem.read(argp, &t, sizeof(t));
            // Map our generic constants back to the host's <termios.h>
            // values. On AArch64+glibc these are identical, but on
            // musl/x86-64 the constants may differ. Using the host's
            // named constants ensures correctness regardless of host.
            unsigned long host_req;
            switch (request) {
                case ioctl_num::REQ_TCSETS:  host_req = TCSETS;  break;
                case ioctl_num::REQ_TCSETSW: host_req = TCSETSW; break;
                default:                     host_req = TCSETSF; break;
            }
            int r = ::ioctl(host_fd, host_req, &t);
            if (r < 0) return -errno;
            return 0;
        }
        case ioctl_num::REQ_TCGETS2: {
            KernelTermios2 t{};
            int r = ::ioctl(host_fd, ioctl_num::REQ_TCGETS2, &t);
            if (r < 0) return -errno;
            mem.write(argp, &t, sizeof(t));
            return 0;
        }
        case ioctl_num::REQ_TCSETS2:
        case ioctl_num::REQ_TCSETSW2:
        case ioctl_num::REQ_TCSETSF2: {
            KernelTermios2 t{};
            mem.read(argp, &t, sizeof(t));
            int r = ::ioctl(host_fd, request, &t);
            if (r < 0) return -errno;
            return 0;
        }
        case ioctl_num::REQ_FIONREAD: {
            int n = 0;
            int r = ::ioctl(host_fd, FIONREAD, &n);
            if (r < 0) return -errno;
            mem.store<int32_t>(argp, n);
            return 0;
        }
        case ioctl_num::REQ_FIONBIO: {
            // FIONBIO takes an int* (non-zero = non-blocking).
            int n = 0;
            mem.read(argp, &n, sizeof(n));
            int r = ::ioctl(host_fd, FIONBIO, &n);
            if (r < 0) return -errno;
            return 0;
        }
        case ioctl_num::REQ_TIOCGPGRP: {
            // Get the foreground process group of the terminal.
            //
            // The guest is a single-process model: getpid()=1, getpgid()=1.
            // If we forwarded to the host, tcgetpgrp() would return the
            // HOST foreground pgrp (e.g., 12345), which would never match
            // getpgrp()=1 — making the guest think it's a background job.
            // Shells (toybox sh, bash) check tcgetpgrp(0)==getpgrp() to
            // decide if they're interactive; if not, they set SIGINT to
            // SIG_IGN, breaking Ctrl+C. Return 1 (the guest PGID) so the
            // shell sees itself as the foreground process group.
            mem.store<int32_t>(argp, 1);
            return 0;
        }
        case ioctl_num::REQ_TIOCSPGRP: {
            // Set the foreground process group. No-op in the single-process
            // guest model — there's only one process group (PGID=1).
            return 0;
        }
        case ioctl_num::REQ_TIOCGSID: {
            // Get the session ID. Return 1 (guest PID = session leader).
            mem.store<int32_t>(argp, 1);
            return 0;
        }
        default:
            return Node::IOCTL_NOT_HANDLED;
    }
}
// ── pass_through_ioctl — last-resort host fd passthrough ───────────────
// For ioctls we don't recognize, we previously passed them straight to
// the host fd with the guest's argp reinterpreted as a host pointer.
// This was DANGEROUS: the guest's virtual address space is separate
// from the host's, so the host ioctl would dereference garbage memory
// (EFAULT) or, worse, corrupt host memory if the guest address happened
// to map to a valid host region.
//
// of passing them through. This is safer and matches what the kernel
// returns for ioctls the fd doesn't support. Programs that rely on
// exotic ioctls (DRM/KMS, media devices, etc.) will get -ENOTTY and
// can fall back to other code paths.
//
// The ioctls we DO handle (TCGETS, TCSETS, FIONREAD, TIOCGWINSZ, etc.)
// are dispatched by dispatch_terminal_ioctl() above, which correctly
// marshals the argp through the guest's Memory.
inline int pass_through_ioctl(int /*host_fd*/, uint32_t /*request*/, uint64_t /*argp*/) {
    // Return -ENOTTY (Inappropriate ioctl for device). This is what the
    // kernel returns for ioctls the fd doesn't support.
    return -ENOTTY;
}
}  // namespace arm64emu::yggdrasil
