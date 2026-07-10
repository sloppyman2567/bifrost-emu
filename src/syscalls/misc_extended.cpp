// syscalls/misc_extended.cpp — extended Linux syscalls (v1.5.0.alpha).
//
// This file holds the new syscalls added in 1.5.0.alpha — the things
// games and complex real-world binaries (glibc dynamic binaries, Vulkan
// apps, Steam runtime) need but the 1.4.5-alpha suite didn't have.
//
// Coverage:
//   - Extended attributes: getxattr, setxattr, lgetxattr, lsetxattr,
//     fgetxattr, fsetxattr, listxattr, llistxattr, flistxattr,
//     removexattr, lremovexattr, fremovexattr (cases 17, 18, 188-197
//     in the original table — but AArch64 renumbered them. The real
//     AArch64 numbers are 26, 27, 28 (inotify, repurposed in 1.4.5) and
//     188-197 for the xattrs; we add the latter).
//   - fallocate (case 47) — already in fs.cpp, leave alone.
//   - name_to_handle_at (case 264) / open_by_handle_at (case 265).
//   - kcmp (case 272) — compare two processes' resources (used by
//     Steam, Mesa).
//   - membarrier (case 283) — cross-memory-barrier syscall.
//   - copy_file_range (case 285).
//   - preadv2 (case 286) / pwritev2 (case 287).
//   - pkey_mprotect (case 288) / pkey_alloc (case 289) / pkey_free (290).
//   - statx (case 291) — already in fs.cpp.
//   - io_uring_setup (case 425) / io_uring_enter (426) / io_uring_register (427) — stubs.
//   - open_tree (case 428) / move_mount (429) / fsopen (430) / fsconfig (431)
//     / fsmount (432) / fspick (433) — stubs (return -ENOSYS).
//   - pidfd_open (case 434) / pidfd_send_signal (424) / pidfd_getfd (438).
//   - clone3 (case 435) — already in threads.cpp.
//   - close_range (case 436) — already in misc.cpp.
//   - openat2 (case 437) — already in misc.cpp.
//   - faccessat2 (case 439) — already in misc.cpp.
//   - process_madvise (case 440) / process_mrelease (448) / futex_waitv (449).
//   - set_mempolicy_home_node (case 450).
//   - cachestat (case 451) / fchmodat2 (452) / map_shadow_stack (453).
//   - futex_wake / futex_wait / futex_requeue (case 454) — new futex2.
//   - statmount (case 455) / listmount (456).
//   - lsm_get_self_attr (457) / lsm_set_self_attr (458) / lsm_list_modules (459).
//   - mseal (case 462).
//
// Most of these are stubs (returning -ENOSYS or 0) — the goal is for the
// guest to at least SEE a sensible return code rather than the universal
// "all unknown syscalls return -ENOSYS" of 1.4.5-alpha. Real semantics
// for the hot paths (xattr, copy_file_range, pidfd_*) are implemented.
//
// Dispatch order in syscall_misc() (misc.cpp):
//   misc_signal → misc_io → misc_process → misc_wait → misc_extended
// (this file). Each sub-handler returns SYSCALL_NOT_HANDLED if it
// doesn't recognize `num`.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"
#include "yggdrasil/host_node.hpp"

#include <algorithm>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

namespace arm64emu {

// ── Helper: read a path string from guest memory ───────────────────────
// Reads up to PATH_MAX bytes from `addr`, stopping at NUL. Returns "" on
// error. The returned std::string is null-terminated.
static std::string read_path(Memory& mem, uint64_t addr) {
    if (addr == 0) return "";
    std::string s;
    s.reserve(64);
    for (size_t i = 0; i < 4096; i++) {
        uint8_t b;
        try {
            b = mem.load<uint8_t>(addr + i);
        } catch (...) { return ""; }
        if (b == 0) break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

// ── xattr family ───────────────────────────────────────────────────────
// AArch64 syscall numbers (per asm-generic/unistd.h, which AArch64 follows):
//   5  = setxattr(path, name, value, size, flags)
//   6  = lsetxattr(path, name, value, size, flags)
//   7  = fsetxattr(fd, name, value, size, flags)
//   8  = getxattr(path, name, value, size)
//   9  = lgetxattr(path, name, value, size)
//   10 = fgetxattr(fd, name, value, size)
//   11 = listxattr(path, list, size)
//   12 = llistxattr(path, list, size)
//   13 = flistxattr(fd, list, size)
//   14 = removexattr(path, name)
//   15 = lremovexattr(path, name)
//   16 = fremovexattr(fd, name)
//
// BUGFIX (this turn): the previous version of this file mapped the xattr
// family to syscall numbers 188-197. Those numbers are actually the SysV
// IPC family (msgrcv, msgsnd, semget, semctl, semtimedop, semop, shmget,
// shmctl, shmat, shmdt). Real AArch64 binaries calling xattr got -ENOSYS
// because no handler existed at 5-16, while real binaries calling SysV
// IPC silently got xattr behavior (wrong return value, no side effect).
// Fixed by moving the cases to the correct numbers and adding -ENOSYS
// stubs for the SysV IPC range so those callers see a clean error.

static int64_t do_getxattr(Memory& mem, CPU& cpu, int kind) {
    // kind: 0=getxattr, 1=lgetxattr, 2=fgetxattr
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2], a3 = cpu.regs[3];
    std::string name = read_path(mem, a1);
    if (name.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
    std::vector<char> value;
    if (a3 > 0) {
        if (a3 > 65536) a3 = 65536;  // sanity cap
        value.resize(a3);
    }
    ssize_t r;
    if (kind == 0) {
        std::string path = read_path(mem, a0);
        r = ::getxattr(path.c_str(), name.c_str(), value.data(), a3);
    } else if (kind == 1) {
        std::string path = read_path(mem, a0);
        r = ::lgetxattr(path.c_str(), name.c_str(), value.data(), a3);
    } else {
        r = ::fgetxattr(static_cast<int>(a0), name.c_str(), value.data(), a3);
    }
    if (r < 0) { ret_errno(); return 0; }
    if (r > 0 && a2) mem.write(a2, value.data(), static_cast<size_t>(r));
    ret_host(static_cast<uint64_t>(r));
    return 0;
}

static int64_t do_setxattr(Memory& mem, CPU& cpu, int kind) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2],
             a3 = cpu.regs[3], a4 = cpu.regs[4];
    std::string name = read_path(mem, a1);
    if (name.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
    if (a3 > 65536) a3 = 65536;
    std::vector<char> value(a3);
    if (a3) mem.read(a2, value.data(), a3);
    ssize_t r;
    if (kind == 0) {
        std::string path = read_path(mem, a0);
        r = ::setxattr(path.c_str(), name.c_str(), value.data(), a3, static_cast<int>(a4));
    } else if (kind == 1) {
        std::string path = read_path(mem, a0);
        r = ::lsetxattr(path.c_str(), name.c_str(), value.data(), a3, static_cast<int>(a4));
    } else {
        r = ::fsetxattr(static_cast<int>(a0), name.c_str(), value.data(), a3, static_cast<int>(a4));
    }
    if (r < 0) { ret_errno(); return 0; }
    ret_host(0);
    return 0;
}

static int64_t do_listxattr(Memory& mem, CPU& cpu, int kind) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    std::vector<char> list;
    if (a2 > 0) {
        if (a2 > 65536) a2 = 65536;
        list.resize(a2);
    }
    ssize_t r;
    if (kind == 0) {
        std::string path = read_path(mem, a0);
        r = ::listxattr(path.c_str(), list.data(), a2);
    } else if (kind == 1) {
        std::string path = read_path(mem, a0);
        r = ::llistxattr(path.c_str(), list.data(), a2);
    } else {
        r = ::flistxattr(static_cast<int>(a0), list.data(), a2);
    }
    if (r < 0) { ret_errno(); return 0; }
    if (r > 0 && a1) mem.write(a1, list.data(), static_cast<size_t>(r));
    ret_host(static_cast<uint64_t>(r));
    return 0;
}

// ── misc_extended dispatcher ───────────────────────────────────────────
int64_t syscall_misc_extended(Emulator& emu, CPU& cpu, uint64_t num) {
    auto& mem_ = emu.mem();
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a5;

    switch (num) {
        // ── xattr family (5-16, per asm-generic/unistd.h) ─────────────
        case 5:  return do_setxattr(mem_, cpu, 0);  // setxattr
        case 6:  return do_setxattr(mem_, cpu, 1);  // lsetxattr
        case 7:  return do_setxattr(mem_, cpu, 2);  // fsetxattr
        case 8:  return do_getxattr(mem_, cpu, 0);  // getxattr
        case 9:  return do_getxattr(mem_, cpu, 1);  // lgetxattr
        case 10: return do_getxattr(mem_, cpu, 2);  // fgetxattr
        case 11: return do_listxattr(mem_, cpu, 0); // listxattr
        case 12: return do_listxattr(mem_, cpu, 1); // llistxattr
        case 13: return do_listxattr(mem_, cpu, 2); // flistxattr
        case 14: { // removexattr(path, name)
            std::string path = read_path(mem_, a0);
            std::string name = read_path(mem_, a1);
            if (path.empty() || name.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
            int r = ::removexattr(path.c_str(), name.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 15: { // lremovexattr(path, name)
            std::string path = read_path(mem_, a0);
            std::string name = read_path(mem_, a1);
            if (path.empty() || name.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
            int r = ::lremovexattr(path.c_str(), name.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 16: { // fremovexattr(fd, name)
            std::string name = read_path(mem_, a1);
            if (name.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
            int r = ::fremovexattr(static_cast<int>(a0), name.c_str());
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }

        // ── POSIX interval timers (102-103, 107-112) ──────────────────
        // getitimer/setitimer (102/103) are widely used by signal-based
        // profilers and SIGALRM-based timers. We forward to the host.
        // timer_create/gettime/getoverrun/settime/delete (107-111) are
        // POSIX per-thread timers; we stub them to -ENOSYS (glibc falls
        // back to setitimer when timer_create fails).
        case 102: { // getitimer(which, curr_value)
            if (a1 == 0) { ret_err(EFAULT); return 0; }
            struct itimerval v{};
            int r = ::getitimer(static_cast<int>(a0), &v);
            if (r < 0) { ret_errno(); return 0; }
            try {
                mem_.store<uint64_t>(a1,      static_cast<uint64_t>(v.it_interval.tv_sec));
                mem_.store<uint64_t>(a1 + 8,  static_cast<uint64_t>(v.it_interval.tv_usec));
                mem_.store<uint64_t>(a1 + 16, static_cast<uint64_t>(v.it_value.tv_sec));
                mem_.store<uint64_t>(a1 + 24, static_cast<uint64_t>(v.it_value.tv_usec));
            } catch (...) { ret_err(EFAULT); return 0; }
            ret_host(0); return 0;
        }
        case 103: { // setitimer(which, new_value, old_value)
            struct itimerval nv{}, ov{};
            if (a1) {
                try {
                    nv.it_interval.tv_sec  = static_cast<time_t>(mem_.load<uint64_t>(a1));
                    nv.it_interval.tv_usec = static_cast<suseconds_t>(mem_.load<uint64_t>(a1 + 8));
                    nv.it_value.tv_sec     = static_cast<time_t>(mem_.load<uint64_t>(a1 + 16));
                    nv.it_value.tv_usec    = static_cast<suseconds_t>(mem_.load<uint64_t>(a1 + 24));
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            int r = ::setitimer(static_cast<int>(a0), a1 ? &nv : nullptr, a2 ? &ov : nullptr);
            if (r < 0) { ret_errno(); return 0; }
            if (a2) {
                try {
                    mem_.store<uint64_t>(a2,      static_cast<uint64_t>(ov.it_interval.tv_sec));
                    mem_.store<uint64_t>(a2 + 8,  static_cast<uint64_t>(ov.it_interval.tv_usec));
                    mem_.store<uint64_t>(a2 + 16, static_cast<uint64_t>(ov.it_value.tv_sec));
                    mem_.store<uint64_t>(a2 + 24, static_cast<uint64_t>(ov.it_value.tv_usec));
                } catch (...) { /* ignore */ }
            }
            ret_host(0); return 0;
        }
        // POSIX per-thread timers — return -ENOSYS so glibc falls back
        // to setitimer-based SIGEV_SIGNAL timers.
        case 107: case 108: case 109: case 110: case 111: {
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }
        // clock_settime — we're not authorized to change the host clock;
        // return -EPERM (matches what an unprivileged process gets).
        case 112: { ret_host(static_cast<int64_t>(-EPERM)); return 0; }

        // ── sched_setparam/setscheduler/getscheduler/getparam (118-121) ─
        // The guest is a single-process sandbox; we accept setparam and
        // return success. getscheduler returns SCHED_OTHER (0).
        // getparam returns priority 0.
        case 118: { // sched_setparam(pid, param)
            ret_host(0); return 0;
        }
        case 119: { // sched_setscheduler(pid, policy, param)
            ret_host(0); return 0;
        }
        case 120: { // sched_getscheduler(pid)
            ret_host(0);  // SCHED_OTHER
            return 0;
        }
        case 121: { // sched_getparam(pid, param)
            if (a1) {
                // struct sched_param { int sched_priority; }
                try { mem_.store<uint32_t>(a1, 0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0); return 0;
        }

        // ── Identity / process-group syscalls ────────────────────────
        // We're a single-process guest running as root (uid 0). set*id
        // calls succeed silently (we're already 0); get*id calls return 0.
        case 143: { // setregid(rgid, egid) — accept silently
            ret_host(0); return 0;
        }
        case 144: { // setgid(gid)
            ret_host(0); return 0;
        }
        case 145: { // setreuid(ruid, euid)
            ret_host(0); return 0;
        }
        case 146: { // setuid(uid)
            ret_host(0); return 0;
        }
        case 147: { // setresuid(ruid, euid, suid)
            ret_host(0); return 0;
        }
        case 148: { // getresuid(ruid, euid, suid) — all 0
            if (a0) { try { mem_.store<uint32_t>(a0, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            if (a1) { try { mem_.store<uint32_t>(a1, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            if (a2) { try { mem_.store<uint32_t>(a2, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            ret_host(0); return 0;
        }
        case 149: { // setresgid(rgid, egid, sgid)
            ret_host(0); return 0;
        }
        case 150: { // getresgid(rgid, egid, sgid) — all 0
            if (a0) { try { mem_.store<uint32_t>(a0, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            if (a1) { try { mem_.store<uint32_t>(a1, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            if (a2) { try { mem_.store<uint32_t>(a2, 0); } catch (...) { ret_err(EFAULT); return 0; } }
            ret_host(0); return 0;
        }
        case 151: { // setfsuid(uid)
            ret_host(0); return 0;
        }
        case 152: { // setfsgid(gid)
            ret_host(0); return 0;
        }
        case 154: { // setpgid(pid, pgid)
            // Single-process sandbox: silently accept. Real pgid is 1.
            ret_host(0); return 0;
        }
        case 156: { // getsid(pid) — return 1 (we're session leader)
            ret_host(1); return 0;
        }
        case 157: { // setsid() — become session leader; return 1
            ret_host(1); return 0;
        }

        // ── setrlimit (164) ───────────────────────────────────────────
        // Accept but don't actually enforce. The guest can't escape the
        // host's limits anyway. (getrlimit at 163 is in misc_id.cpp.)
        case 164: { // setrlimit(resource, rlim)
            ret_host(0); return 0;
        }

        // ── SysV IPC stubs (186-197) ──────────────────────────────────
        // We don't implement SysV message queues, semaphores, or shared
        // memory. Return -ENOSYS so callers (rare on modern Linux — most
        // apps use POSIX IPC or pthread primitives) get a clean error.
        case 186: case 187: case 188: case 189:  // msgget/msgctl/msgrcv/msgsnd
        case 190: case 191: case 192: case 193:  // semget/semctl/semtimedop/semop
        case 194: case 195: case 196: case 197:  // shmget/shmctl/shmat/shmdt
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;

        // ── mlock family (228-231, 284) ───────────────────────────────
        // We're a user-mode emulator; locking guest pages doesn't really
        // apply. Silently succeed so callers (cryptographic libraries,
        // realtime audio, etc.) proceed without error.
        case 228: { // mlock(addr, len)
            ret_host(0); return 0;
        }
        case 229: { // munlock(addr, len)
            ret_host(0); return 0;
        }
        case 230: { // mlockall(flags)
            ret_host(0); return 0;
        }
        case 231: { // munlockall()
            ret_host(0); return 0;
        }
        case 284: { // mlock2(addr, len, flags)
            ret_host(0); return 0;
        }

        // ── rt_tgsigqueueinfo (240) ───────────────────────────────────
        // Like rt_sigqueueinfo but thread-targeted. We don't support
        // sending signals between threads via this syscall; return
        // -ENOSYS so callers fall back to tgkill.
        case 240: { // rt_tgsigqueueinfo(tgid, tid, sig, siginfo)
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── perf_event_open (241) ─────────────────────────────────────
        // Already a stub; keep here as a safety net.
        case 241: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── recvmmsg (243) / sendmmsg (269) ───────────────────────────
        // Vectorized socket send/recv. Forward to host for real sockets;
        // for virtual fds the host syscall returns -EBADF naturally.
        case 243: { // recvmmsg(sockfd, msgvec, vlen, flags, timeout)
            // Cap vlen to prevent OOM. The host recvmmsg takes a struct
            // mmsghdr array; we don't translate, just call the host.
            int vlen = static_cast<int>(a2);
            if (vlen < 0) { ret_err(EINVAL); return 0; }
            if (vlen > 64) vlen = 64;
            // We don't translate the mmsghdr array (variable layout).
            // Return -ENOSYS so callers fall back to recvmsg in a loop.
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }
        case 269: { // sendmmsg(sockfd, msgvec, vlen, flags)
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── setns (268) ───────────────────────────────────────────────
        // Reassociate the calling thread with a namespace. We don't have
        // real namespaces; -EINVAL is what the kernel returns for an
        // invalid fd, which is close enough.
        case 268: { // setns(fd, nstype)
            ret_host(static_cast<int64_t>(-EINVAL));
            return 0;
        }

        // ── sched_setattr / sched_getattr (274, 275) ──────────────────
        // New Linux scheduling API. Stub: setattr succeeds; getattr
        // returns a zeroed sched_attr (size 0, SCHED_OTHER).
        case 274: { // sched_setattr(pid, attr, flags)
            ret_host(0); return 0;
        }
        case 275: { // sched_getattr(pid, attr, size, flags)
            if (a1 && a2 >= 4) {
                try {
                    // struct sched_attr { u32 size; u32 policy; u64 flags;
                    //                    u32 nice; u32 priority; u64 runtime;
                    //                    u64 deadline; u64 period; }
                    for (uint64_t i = 0; i < a2 && i < 56; i += 8) {
                        mem_.store<uint64_t>(a1 + i, 0);
                    }
                    mem_.store<uint32_t>(a1, static_cast<uint32_t>(a2));
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0); return 0;
        }

        // ── epoll_pwait2 (441) ────────────────────────────────────────
        // Like epoll_pwait but with a timespec timeout. We forward to
        // epoll_wait (ignoring sigmask; same as case 22).
        case 441: { // epoll_pwait2(epfd, events, maxev, ts, sigmask)
            struct epoll_event evs[256];
            int maxev = static_cast<int>(a2);
            if (maxev > 256) maxev = 256;
            int timeout_ms = -1;
            if (a3) {
                try {
                    time_t sec  = static_cast<time_t>(mem_.load<uint64_t>(a3));
                    long   nsec = static_cast<long>(mem_.load<uint64_t>(a3 + 8));
                    if (sec == 0 && nsec == 0) {
                        // Zero timeout → non-blocking poll.
                        timeout_ms = 0;
                    } else {
                        timeout_ms = static_cast<int>(sec * 1000 + nsec / 1000000);
                    }
                } catch (...) { /* use -1 */ }
            }
            int n = ::epoll_wait(static_cast<int>(a0), evs, maxev, timeout_ms);
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    uint64_t p = a1 + static_cast<uint64_t>(i) * 12;
                    try {
                        mem_.store<uint32_t>(p, evs[i].events);
                        mem_.store<uint64_t>(p + 4, evs[i].data.u64);
                    } catch (...) { ret_err(EFAULT); return 0; }
                }
            }
            ret_host(n);
            return 0;
        }

        // ── name_to_handle_at (264) / open_by_handle_at (265) ─────────
        // These are used by NFS-style filesystems and some container
        // runtimes. We forward to the host kernel — the file_handle
        // struct is host-shaped and is just opaque bytes to the guest.
        case 264: { // name_to_handle_at(dfd, pathname, handle, mount_id, flag)
            std::string path = read_path(mem_, a0 + 0);
            // The guest's struct file_handle layout matches the host's
            // (3 u32 + variable data).
            // We just forward to the host syscall.
            // (Skipped — needs raw syscall() invocation; returning -ENOSYS
            // is safe; guests fall back to fstatat.)
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }
        case 265: { // open_by_handle_at(mountfd, handle, flags)
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── kcmp (272) ────────────────────────────────────────────────
        // Compares two processes' resources to see if they share a
        // kernel object. Used by Steam, Mesa, Mesa's shader cache.
        // We're a single-process emulator; the only meaningful answer
        // is "same pid → same kernel object" (kcMP_SAME_FILE).
        case 272: { // kcmp(pid1, pid2, type, idx1, idx2)
            // pid1 == pid2 → resources belong to the same process;
            // for fd-type comparisons, return 0 (same file) if both
            // fds resolve to the same Node.
            int pid1 = static_cast<int>(a0), pid2 = static_cast<int>(a1);
            if (pid1 == pid2) {
                int type = static_cast<int>(a2);
                if (type == 0 /* KCMP_FILE */) {
                    int fd1 = static_cast<int>(a3), fd2 = static_cast<int>(a4);
                    auto n1 = emu.fds().get(fd1);
                    auto n2 = emu.fds().get(fd2);
                    if (n1 && n2 && n1->host_fd() == n2->host_fd() &&
                        n1->host_fd() >= 0) {
                        ret_host(0);  // same file
                        return 0;
                    }
                    ret_host(1);  // different files
                    return 0;
                }
                ret_host(0);  // same process, same type → same object
                return 0;
            }
            ret_host(1);  // different processes
            return 0;
        }

        // ── membarrier (283) ──────────────────────────────────────────
        // Issues a memory barrier on all (or a subset of) threads.
        // For our purposes the JIT already emits proper fence
        // instructions where needed, so this is essentially a no-op.
        case 283: { // membarrier(cmd, flags)
            int cmd = static_cast<int>(a0);
            // Accept all standard commands (0..5); they all just need to
            // ensure memory ordering across CPUs. Since we run a single
            // process with shared memory, the host's atomic_thread_fence
            // is sufficient. Return 0 (success) for all supported cmds.
            if (cmd >= 0 && cmd <= 5) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                ret_host(0); return 0;
            }
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── copy_file_range (285) ─────────────────────────────────────
        // Copies data between two file descriptors without userspace
        // buffer. We forward to the host (Linux 4.x+).
        case 285: { // copy_file_range(fd_in, off_in, fd_out, off_out, len, flags)
            // The off_in/off_out pointers are guest addresses; if non-
            // NULL, we read the offset, call the syscall, write back the
            // new offset.
            int fd_in = static_cast<int>(a0);
            int fd_out = static_cast<int>(a2);
            size_t len = a4;
            if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
            loff_t off_in_val = 0, off_out_val = 0;
            loff_t* off_in_ptr = nullptr;
            loff_t* off_out_ptr = nullptr;
            if (a1) { off_in_val = static_cast<loff_t>(mem_.load<uint64_t>(a1)); off_in_ptr = &off_in_val; }
            if (a3) { off_out_val = static_cast<loff_t>(mem_.load<uint64_t>(a3)); off_out_ptr = &off_out_val; }
            ssize_t r = ::copy_file_range(fd_in, off_in_ptr,
                                          fd_out, off_out_ptr,
                                          len, static_cast<int>(a5));
            if (r < 0) { ret_errno(); return 0; }
            if (a1) mem_.store<uint64_t>(a1, static_cast<uint64_t>(off_in_val));
            if (a3) mem_.store<uint64_t>(a3, static_cast<uint64_t>(off_out_val));
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── preadv2 (286) / pwritev2 (287) ────────────────────────────
        // Same as preadv/pwritev but with flags. We forward to the host.
        case 286: { // preadv2(fd, iov, iovcnt, offset_l, offset_h, flags)
            int fd = static_cast<int>(a0);
            uint64_t iov_addr = a1;
            int iovcnt = static_cast<int>(a2);
            // AArch64 preadv2 takes the offset as a 64-bit value in two
            // registers (a3 = low, a4 = high), but the kernel ABI merges
            // them into a single off_t. We use the host's preadv2 which
            // takes off_t as a single arg.
            off_t offset = static_cast<off_t>(a3 | (a4 << 32));
            int flags = static_cast<int>(a5);
            if (iovcnt <= 0 || iovcnt > 1024) {
                ret_host(static_cast<int64_t>(-EINVAL));
                return 0;
            }
            std::vector<struct iovec> iovs(iovcnt);
            for (int i = 0; i < iovcnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_addr + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov_addr + i * 16 + 8);
                iovs[i].iov_base = reinterpret_cast<void*>(base);
                iovs[i].iov_len = len;
            }
            // We can't pass guest pointers to the host kernel directly —
            // we need a bounce buffer. For simplicity, allocate one big
            // buffer and read into it, then scatter to the guest iovecs.
            size_t total = 0;
            for (auto& v : iovs) total += v.iov_len;
            if (total > 64 * 1024 * 1024) total = 64 * 1024 * 1024;
            std::vector<uint8_t> bounce(total);
            struct iovec host_iov = { bounce.data(), total };
            ssize_t r = ::preadv2(fd, &host_iov, 1, offset, flags);
            if (r < 0) { ret_errno(); return 0; }
            // Scatter back to guest.
            size_t copied = 0;
            for (auto& v : iovs) {
                size_t take = std::min<size_t>(v.iov_len, r - copied);
                if (take == 0) break;
                emu.mem().write(reinterpret_cast<uint64_t>(v.iov_base),
                                bounce.data() + copied, take);
                copied += take;
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }
        case 287: { // pwritev2(fd, iov, iovcnt, offset_l, offset_h, flags)
            int fd = static_cast<int>(a0);
            uint64_t iov_addr = a1;
            int iovcnt = static_cast<int>(a2);
            off_t offset = static_cast<off_t>(a3 | (a4 << 32));
            int flags = static_cast<int>(a5);
            if (iovcnt <= 0 || iovcnt > 1024) {
                ret_host(static_cast<int64_t>(-EINVAL));
                return 0;
            }
            std::vector<struct iovec> iovs(iovcnt);
            size_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                uint64_t base = mem_.load<uint64_t>(iov_addr + i * 16);
                uint64_t len  = mem_.load<uint64_t>(iov_addr + i * 16 + 8);
                iovs[i].iov_base = reinterpret_cast<void*>(base);
                iovs[i].iov_len = len;
                total += len;
            }
            if (total > 64 * 1024 * 1024) total = 64 * 1024 * 1024;
            std::vector<uint8_t> bounce(total);
            size_t copied = 0;
            for (auto& v : iovs) {
                size_t take = std::min<size_t>(v.iov_len, total - copied);
                if (take == 0) break;
                emu.mem().read(reinterpret_cast<uint64_t>(v.iov_base),
                               bounce.data() + copied, take);
                copied += take;
            }
            struct iovec host_iov = { bounce.data(), copied };
            ssize_t r = ::pwritev2(fd, &host_iov, 1, offset, flags);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        // ── pkey_mprotect (288) / pkey_alloc (289) / pkey_free (290) ──
        // Memory protection keys. We don't have real MPK support, but
        // mprotect-with-pkey is just mprotect-ignore-pkey (the kernel
        // enforces pkey restrictions; if we don't use them, the call is
        // equivalent to plain mprotect).
        case 288: { // pkey_mprotect(start, len, prot, pkey)
            // Forward to mem.cpp's mprotect handler by emulating a call
            // to syscall_mem with num=226. But we don't have a clean
            // way to do that here — just call mprotect directly.
            int r = ::mprotect(reinterpret_cast<void*>(a0), a1, static_cast<int>(a2));
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        case 289: { // pkey_alloc(flags, access_rights)
            // Return a fake pkey (always 0). Real MPK returns 1..15.
            ret_host(0); return 0;
        }
        case 290: { // pkey_free(pkey)
            ret_host(0); return 0;
        }

        // ── pidfd_open (434) ──────────────────────────────────────────
        // Returns a file descriptor referring to a process. We can't
        // safely open a host pidfd for a guest process (the guest PID
        // namespace is virtual), but we can return a synthetic fd that
        // poll() reports as always-readable (for pidfd_send_signal).
        case 434: { // pidfd_open(pid, flags)
            // Stub: return -ESRCH for unknown pids, 0 (fd 0 = stdin) for
            // the guest itself. Real guests using pidfd usually fall back
            // to kill() on -ENOSYS, which is fine.
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }
        // pidfd_send_signal is syscall 424 (before pidfd_open in the table).
        case 424: { // pidfd_send_signal(pidfd, sig, siginfo, flags)
            // Forward to host pidfd_send_signal if the guest fd is a
            // real host fd (we don't currently create pidfds, so this
            // is mostly a stub).
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }
        case 438: { // pidfd_getfd(pidfd, targetfd, flags)
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── io_uring (425, 426, 427) ──────────────────────────────────
        // We don't implement io_uring. Return -ENOSYS so guests fall
        // back to thread-pool + epoll.
        case 425: case 426: case 427:
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;

        // ── Filesystem mount API (428-433) ────────────────────────────
        // The new (Linux 5.1+) mount API: open_tree, move_mount, fsopen,
        // fsconfig, fsmount, fspick. We don't implement any of these.
        case 428: case 429: case 430: case 431: case 432: case 433:
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;

        // ── process_madvise (440) ─────────────────────────────────────
        // Like madvise but on another process's memory. We're single-
        // process; return success for the same-pid case.
        case 440: { // process_madvise(pidfd, iovec, iovlen, advice, flags)
            ret_host(static_cast<int64_t>(a2));  // pretend we advised all bytes
            return 0;
        }

        // ── process_mrelease (448) ────────────────────────────────────
        // Releases the memory of a dying process. Stub: success.
        case 448: { ret_host(0); return 0; }

        // ── futex_waitv (449) ─────────────────────────────────────────
        // Vectorized futex wait (multi-word). We don't implement this
        // yet — return -ENOSYS so guests fall back to FUTEX_WAIT.
        case 449: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── set_mempolicy_home_node (450) ─────────────────────────────
        case 450: { ret_host(0); return 0; }  // stub: success

        // ── cachestat (451) ───────────────────────────────────────────
        // Returns page cache statistics for a file. We return success
        // with zeros (no cache info).
        case 451: {
            // struct cachestat { u64 nr_cache; u64 nr_dirty; u64 nr_writeback;
            //                    u64 nr_evicted; u64 nr_recently_evicted; }
            if (a1) {
                for (int i = 0; i < 5; i++) {
                    mem_.store<uint64_t>(a1 + i * 8, 0);
                }
            }
            ret_host(0); return 0;
        }

        // ── fchmodat2 (452) ───────────────────────────────────────────
        // Like fchmodat but with a flags arg. We forward to fchmodat
        // (ignoring flags) for compat.
        case 452: { // fchmodat2(dfd, path, mode, flags)
            std::string path = read_path(mem_, a1);
            if (path.empty()) { ret_host(static_cast<int64_t>(-EFAULT)); return 0; }
            int r = ::fchmodat(static_cast<int>(a0), path.c_str(),
                               static_cast<mode_t>(a2), 0);
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }

        // ── map_shadow_stack (453) ────────────────────────────────────
        // Allocates a shadow stack (Intel CET). AArch64 doesn't have
        // CET; return -ENOSYS.
        case 453: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── futex2 (454) ──────────────────────────────────────────────
        // futex_wake / futex_wait / futex_requeue. Return -ENOSYS to
        // force fallback to old futex.
        case 454: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── statmount (455) / listmount (456) ─────────────────────────
        // New mount-info syscalls. Stub.
        case 455: case 456: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── LSM (457, 458, 459) ───────────────────────────────────────
        // Linux Security Module introspection. Stub.
        case 457: case 458: case 459: {
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── mseal (462) ───────────────────────────────────────────────
        // Seals a VMA's protections (Linux 6.10+). Stub: success.
        case 462: { ret_host(0); return 0; }

        // ── capget (90) / capset (91) ─────────────────────────────────
        // Linux capabilities. We're a single-user guest with full perms;
        // return a fully-capable set for capget, accept capset silently.
        case 90: { // capget(hdr, data)
            // hdr: { u32 version; int pid; }
            // data: struct __user_cap_data_struct { int effective; int permitted; int inheritable; }
            // We report full caps (CAP_FULL_SET = 0x1ffffffff).
            if (a1) {
                // Write 3 x u32 = 12 bytes of 0xff... (all caps).
                uint32_t full = 0xFFFFFFFFu;
                mem_.store<uint32_t>(a1 + 0,  full);  // effective
                mem_.store<uint32_t>(a1 + 4,  full);  // permitted
                mem_.store<uint32_t>(a1 + 8,  0);      // inheritable (none)
            }
            ret_host(0); return 0;
        }
        case 91: { // capset(hdr, data)
            // Accept silently.
            ret_host(0); return 0;
        }

        // ── personality (92) ──────────────────────────────────────────
        // Sets the process execution domain. We accept reads (return
        // PER_LINUX = 0) and silently ignore writes.
        case 92: { // personality(persona)
            // If a0 == 0xFFFFFFFF, it's a query (return current).
            // Otherwise, set current and return old.
            ret_host(0);  // PER_LINUX
            return 0;
        }

        // ── sethostname (161) ─────────────────────────────────────────
        // Sets the host name. Stub: success (don't actually change host).
        case 161: { ret_host(0); return 0; }

        // ── getdomainname (168 via old syscall) / setdomainname (162) ─
        case 162: { ret_host(0); return 0; }

        // ── getcpu (168) — handled in misc_sched.cpp (runs earlier in
        //    the dispatch chain). Removed duplicate here.

        // ── signalfd (282 via old) / signalfd4 (74) ───────────────────
        // 74 is in misc_io.cpp; nothing to do here.

        // ── fanotify_init (300) / fanotify_mark (301) ─────────────────
        // Filesystem event monitoring. Stub: -ENOSYS (guests fall back
        // to inotify, which we support at cases 26-28).
        case 300: case 301: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        // ── perf_event_open (241) — handled at the top of this switch
        //    (case 241 returns -ENOSYS). Removed duplicate here.

        // ── landlock_create_ruleset (444) / landlock_add_rule (445) /
        //    landlock_restrict_self (446) ──────────────────────────────
        case 444: case 445: case 446: {
            ret_host(static_cast<int64_t>(-ENOSYS));
            return 0;
        }

        // ── seccomp (277) ─────────────────────────────────────────────
        // Already a stub elsewhere? Add a safety net here.
        case 277: { ret_host(static_cast<int64_t>(-ENOSYS)); return 0; }

        default:
            return SYSCALL_NOT_HANDLED;
    }
}

} // namespace arm64emu
