// yggdrasil/terminal_ioctls.hpp — shared terminal ioctl dispatch.
//
// Both HostNode and StdioNode implement ioctl() by forwarding terminal
// ioctls (TIOCGWINSZ, TCGETS, TCSETS/TCSETSW/TCSETSF, FIONREAD) to the
// underlying host fd, marshalling the struct through the guest Memory.
// The implementations were duplicated verbatim — Turn 37 extracts them
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

#include "bifrost/types.hpp"
#include "core/memory.h"  // full Memory definition (for read/write/store)

#include <cstdint>
#include <cstdio>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

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
    constexpr uint32_t REQ_TIOCSCTTY  = 0x540E;  // set controlling tty
    constexpr uint32_t REQ_TIOCGPGRP  = 0x540F;  // get foreground pgrp
    constexpr uint32_t REQ_TIOCSPGRP  = 0x5410;  // set foreground pgrp
    constexpr uint32_t REQ_TIOCGWINSZ = 0x5413;  // get window size
    constexpr uint32_t REQ_FIONREAD   = 0x541B;  // bytes available to read
    constexpr uint32_t REQ_FIONBIO    = 0x5421;  // set/clear non-blocking I/O
    constexpr uint32_t REQ_TIOCNOTTY  = 0x5422;  // detach controlling tty
    constexpr uint32_t REQ_TIOCGSID   = 0x5429;  // get session id
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
// drift (which was the original Turn 35 bug — TIOCGWINSZ worked on
// host fds but not on stdin/stdout/stderr).
inline int dispatch_terminal_ioctl(int host_fd, uint32_t request,
                                    uint64_t argp, Memory& mem) {
    if (argp == 0) return -EFAULT;

    switch (request) {
        case ioctl_num::REQ_TIOCGWINSZ: {
            struct winsize ws;
            int r = ::ioctl(host_fd, TIOCGWINSZ, &ws);
            if (r < 0) return -errno;
            mem.write(argp, &ws, sizeof(ws));
            return 0;
        }
        case ioctl_num::REQ_TCGETS: {
            struct termios t;
            int r = ::ioctl(host_fd, TCGETS, &t);
            if (r < 0) return -errno;
            mem.write(argp, &t, sizeof(t));
            return 0;
        }
        case ioctl_num::REQ_TCSETS:
        case ioctl_num::REQ_TCSETSW:
        case ioctl_num::REQ_TCSETSF: {
            struct termios t;
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
// For ioctls we don't recognize, pass them straight to the host fd.
// The argp is interpreted as a raw pointer; for most ioctls this works
// because the guest's argp is a guest virtual address that maps to a
// host address via the emulator's Memory direct window (low 4 GiB).
//
// For ioctls whose argp lives above the 4 GiB window, this would need
// to marshal through Memory — but those are rare enough (mostly DRM/
// KMS ioctls on modern Linux) that we accept the limitation rather
// than complicate the common path.
inline int pass_through_ioctl(int host_fd, uint32_t request, uint64_t argp) {
    int r = ::ioctl(host_fd, static_cast<unsigned long>(request),
                    reinterpret_cast<void*>(argp));
    if (r < 0) return -errno;
    return r;
}

}  // namespace arm64emu::yggdrasil
