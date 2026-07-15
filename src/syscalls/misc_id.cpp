// syscalls/misc_id.cpp — identity/system-info syscalls extracted from
// misc_process.cpp (Turn 69 refactor). Handles:
//   - getgroups / setgroups (case 158, 159)
//   - uname (case 160)
//   - getrlimit (case 163)
//   - getrusage (case 165)
//   - umask (case 166)
//   - prctl (case 167)
//   - getpid / getppid / getuid / geteuid / getgid / getegid / gettid (172-178)
//   - sysinfo (case 179)
//   - readahead (case 213)
//   - add_key / request_key / keyctl (case 217, 218, 219)
//   - swapon (case 224)
//   - mincore (case 232)
//   - prlimit64 (case 261)
//   - process_vm_readv (case 270)
//   - kcmp (case 272)
//   - getrandom (case 278)
//   - execveat (case 281)
//   - rseq (case 293)
//
// All case bodies are extracted verbatim from the original misc_process.cpp.
//
// NOTE: this file is NOT a friend of Emulator (unlike misc.cpp). It accesses
// private state via the public accessors emu.mem(), emu.fds(), etc.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "syscalls/syscalls.h"
#include <errno.h>
#include <fcntl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <syscall.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
namespace arm64emu {
int64_t syscall_misc_id(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem();
    switch (num) {
        case 158: { // getgroups(size, gid_t list[]) — AArch64 syscall 158
            // Return just the effective GID (0 = root) in the supplied list.
            // Most guests call getgroups(0, NULL) first to get the count, then
            // allocate and call again. We return 1 group (GID 0).
            if (a0 == 0) { ret_host(1); return 0; }  // query count
            if (a1 == 0) { ret_err(EFAULT); return 0; }
            try {
                mem_.store<uint32_t>(a1, 0);  // GID 0 (root)
            } catch (...) { ret_err(EFAULT); return 0; }
            ret_host(1);
            return 0;
        }
        case 159: { // setgroups(size, list[]) — AArch64 syscall 159
            // No-op for emulation. The guest is a single-user sandbox; we
            // accept any setgroups() call and pretend it succeeded.
            ret_host(0);
            return 0;
        }
        case 160: { // uname
            // struct utsname (Linux): 6 fields of 65 bytes each
            //   sysname, nodename, release, version, machine, domainname
            // glibc checks the release string to decide which features
            // (VDSO, futex flags, etc.) are available. We advertise a
            // reasonably modern kernel so glibc takes the fast paths.
            const char* fields[] = {
                "Linux",                       // sysname
                "arm64-emu",                   // nodename
                "6.5.0",                       // release (glibc wants >= 3.2 for most things)
                "#1 SMP PREEMPT Dynamic arm64-emu", // version
                "aarch64",                     // machine
                "(none)",                      // domainname
            };
            uint64_t off = a0;
            for (auto s : fields) {
                char buf[65] = {0};
                strncpy(buf, s, 64);
                mem_.write(off, buf, 65);
                off += 65;
            }
            ret_host(0);
            return 0;
        }
        case 163: { // getrlimit(resource, rlim) — AArch64 syscall 163
            // Return generous infinite limits so libc doesn't choke.
            // struct rlimit { uint64_t rlim_cur; uint64_t rlim_max; }
            uint64_t rlim[2] = { static_cast<uint64_t>(-1ULL), static_cast<uint64_t>(-1ULL) };
            mem_.write(a1, rlim, sizeof(rlim));
            ret_host(0);
            return 0;
        }
        case 165: { // getrusage(who, usage) — AArch64 syscall 165
            // rusage includes emulator overhead, but for RUSAGE_CHILDREN
            // (used by `toybox time` after wait4) it's the forked emulator
            // child's CPU time — the closest we can get to guest CPU time.
            // struct rusage is 144 bytes on LP64, identical layout on
            // x86-64 host and AArch64 guest (both use 64-bit time_t).
            //
            // even on error. The old code returned ret_errno() without
            // writing, leaving the guest's stack buffer uninitialized
            // with deterministic garbage (e.g. "user 549755811552.42").
            // Now we zero-fill first, then try host getrusage. If it
            // fails, the guest gets zeros (not garbage).
            int who = static_cast<int>(a0);
            if (a1 == 0) { ret_err(EFAULT); return 0; }
            // Zero-fill first so the guest NEVER sees uninitialized data.
            struct rusage ru;
            memset(&ru, 0, sizeof(ru));
            int r = ::getrusage(who, &ru);
            if (r < 0) {
                // Host getrusage failed — write zeros (not garbage).
                if (getenv("BIFROST_TRACE_RUSAGE")) {
                    fprintf(stderr, "[getrusage] who=%d FAILED errno=%d — "
                            "writing zeros\n", who, errno);
                }
            } else {
                if (getenv("BIFROST_TRACE_RUSAGE")) {
                    fprintf(stderr, "[getrusage] who=%d OK: utime=%ld.%06ld "
                            "stime=%ld.%06ld\n", who,
                            (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
                            (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec);
                }
            }
            try { mem_.write(a1, &ru, sizeof(ru)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_host(r < 0 ? static_cast<uint64_t>(static_cast<int64_t>(-errno)) : 0);
            return 0;
        }
        case 166: { // umask(new_mask) — aarch64 166
            // Track guest umask separately from host. The old code called
            // ::umask() which modifies the host process's umask — this
            // caused JIT verify-mode false-positives (stateful syscall run
            // twice gives different results) and broke sandbox isolation.
            mode_t old = emu.guest_umask();
            emu.set_guest_umask(static_cast<mode_t>(a0));
            ret_host(static_cast<uint64_t>(old));
            return 0;
        }
        case 167: { // prctl — process/thread control
            // Implemented the most common prctl options. Unknown options
            // return -EINVAL (matching kernel behavior). Previously ALL
            // options silently returned 0, which broke PR_GET_NAME (returned
            // garbage), PR_SET_PDEATHSIG (silently ignored), etc.
            //
            // PR_* constants per <linux/prctl.h>. We define them locally
            // (not via #include <sys/prctl.h>) because the host's glibc
            // header may not have all the recent ones.
            enum {
                PR_SET_PDEATHSIG   = 1,
                PR_GET_PDEATHSIG   = 2,
                PR_GET_DUMPABLE    = 3,
                PR_SET_DUMPABLE    = 4,
                PR_GET_KEEPCAPS    = 7,
                PR_SET_KEEPCAPS    = 8,
                PR_GET_TIMING      = 13,
                PR_SET_TIMING      = 14,
                PR_SET_NAME        = 15,
                PR_GET_NAME        = 16,
                PR_GET_SECCOMP     = 21,
                PR_SET_SECCOMP     = 22,
                PR_CAPBSET_READ    = 23,
                PR_CAPBSET_DROP    = 24,
                PR_GET_TSC         = 25,
                PR_SET_TSC         = 26,
                PR_GET_SECUREBITS  = 27,
                PR_SET_SECUREBITS  = 28,
                PR_SET_TIMERSLACK  = 29,
                PR_GET_TIMERSLACK  = 30,
                PR_TASK_PERF_EVENTS_DISABLE = 31,
                PR_TASK_PERF_EVENTS_ENABLE  = 32,
                PR_MCE_KILL        = 33,
                PR_MCE_KILL_GET    = 34,
                PR_SET_MM          = 35,
                PR_SET_PTRACER     = 0x59616d61,
                PR_SET_CHILD_SUBREAPER = 36,
                PR_GET_CHILD_SUBREAPER = 37,
                PR_SET_NO_NEW_PRIVS    = 38,
                PR_GET_NO_NEW_PRIVS    = 39,
                PR_GET_TID_ADDRESS     = 40,
                PR_SET_THP_DISABLE     = 41,
                PR_GET_THP_DISABLE     = 42,
                PR_CAP_AMBIENT         = 47,
            };
            uint32_t option = static_cast<uint32_t>(a0);
            switch (option) {
                case PR_SET_NAME: {  // 15 — set process name (comm)
                    // Read the name from guest memory (a1). Max 16 bytes
                    // (15 + NUL). The kernel truncates, doesn't error.
                    char name[17] = {0};
                    try {
                        for (size_t i = 0; i < 16; i++) {
                            uint8_t c = mem_.load<uint8_t>(a1 + i);
                            if (c == 0) break;
                            name[i] = static_cast<char>(c);
                        }
                        name[16] = '\0';
                    } catch (...) {
                        ret_err(EFAULT);
                        return 0;
                    }
                    emu.set_guest_comm(name);
                    ret_host(0);
                    return 0;
                }
                case PR_GET_NAME: {  // 16 — get process name (comm)
                    // Write the name to guest memory (a1). Always 16 bytes
                    // (NUL-padded).
                    if (a1 == 0) { ret_err(EFAULT); return 0; }
                    char buf[16] = {0};
                    const std::string& comm = emu.guest_comm();
                    size_t n = comm.size() > 15 ? 15 : comm.size();
                    memcpy(buf, comm.data(), n);
                    try { mem_.write(a1, buf, 16); }
                    catch (...) { ret_err(EFAULT); return 0; }
                    ret_host(0);
                    return 0;
                }
                case PR_SET_PDEATHSIG:  // 1 — set parent-death signal
                case PR_SET_DUMPABLE:   // 4
                case PR_SET_KEEPCAPS:   // 8
                case PR_SET_TIMING:     // 14
                case PR_SET_SECCOMP:    // 22 — we don't implement seccomp
                case PR_CAPBSET_DROP:   // 24
                case PR_SET_TSC:        // 26
                case PR_SET_SECUREBITS: // 28
                case PR_SET_TIMERSLACK: // 29
                case PR_TASK_PERF_EVENTS_DISABLE:
                case PR_TASK_PERF_EVENTS_ENABLE:
                case PR_MCE_KILL:       // 33
                case PR_SET_CHILD_SUBREAPER: // 36
                case PR_SET_NO_NEW_PRIVS:   // 38
                case PR_SET_THP_DISABLE:    // 41
                case PR_SET_PTRACER:        // 0x59616d61
                    // Accept and ignore — we don't enforce these in the
                    // sandbox. Returning 0 (success) is safe.
                    ret_host(0);
                    return 0;
                case PR_GET_PDEATHSIG: {  // 2 — return parent-death signal
                    // Write 0 (no signal) to the guest pointer in a1.
                    if (a1) {
                        try { mem_.store<uint32_t>(a1, 0); }
                        catch (...) { ret_err(EFAULT); return 0; }
                    }
                    ret_host(0);
                    return 0;
                }
                case PR_GET_DUMPABLE:    // 3
                    ret_host(1);  // SUID_DUMP_USER
                    return 0;
                case PR_GET_KEEPCAPS:    // 7
                    ret_host(0);
                    return 0;
                case PR_GET_TIMING:      // 13
                    ret_host(0);  // PR_TIMING_STATISTICAL
                    return 0;
                case PR_GET_SECCOMP:     // 21
                    ret_host(0);  // SECCOMP_MODE_DISABLED
                    return 0;
                case PR_CAPBSET_READ:    // 23 — capability bounding set
                    ret_host(1);  // capability is in bounding set
                    return 0;
                case PR_GET_TSC:         // 25
                    ret_host(0);  // PR_TSC_ENABLE
                    return 0;
                case PR_GET_SECUREBITS:  // 27
                    ret_host(0);
                    return 0;
                case PR_GET_TIMERSLACK:  // 30
                    ret_host(50000);  // 50 us default
                    return 0;
                case PR_MCE_KILL_GET:    // 34
                    ret_host(0);  // PR_MCE_KILL_DEFAULT
                    return 0;
                case PR_GET_CHILD_SUBREAPER: // 37
                    if (a1) {
                        try { mem_.store<uint32_t>(a1, 0); }
                        catch (...) { ret_err(EFAULT); return 0; }
                    }
                    ret_host(0);
                    return 0;
                case PR_GET_NO_NEW_PRIVS:    // 39
                    ret_host(0);  // not set
                    return 0;
                case PR_GET_TID_ADDRESS: {  // 40
                    // Write the address of the TID field to a1.
                    if (a1) {
                        try { mem_.store<uint64_t>(a1, cpu.set_tid_address_ptr); }
                        catch (...) { ret_err(EFAULT); return 0; }
                    }
                    ret_host(0);
                    return 0;
                }
                case PR_GET_THP_DISABLE:  // 42
                    ret_host(0);
                    return 0;
                case PR_CAP_AMBIENT:  // 47 — sub-operations via a1
                    // PR_CAP_AMBIENT_IS_SET/RAISE/etc. Return 0.
                    ret_host(0);
                    return 0;
                default:
                    // Unknown prctl option — return -EINVAL (matches kernel).
                    ret_err(EINVAL);
                    return 0;
            }
        }
        case 172: { // getpid
            // CLONE_THREAD semantics: all threads in the same thread
            // group see the same PID (= the main thread's TID, which is
            // 1 for the guest process). Forked children (clone without
            // CLONE_VM) are separate processes — they must report a
            // DIFFERENT PID so that getpid() != parent_pid, which lets
            // the child detect it's a forked process and reset signal
            // handlers to SIG_DFL (e.g., toybox sh line 2711:
            // "if (getpid() != TT.pid) signal(SIGINT, SIG_DFL)").
            // Without this, the forked child inherits SIG_IGN from the
            // parent and Ctrl+C can't interrupt foreground commands.
            if (cpu.is_fork_process) {
                ret_host(static_cast<uint64_t>(cpu.tid));
            } else {
                ret_host(1);
            }
            return 0;
        }
        case 173: { // getppid
            // BUGFIX: was returning the emulator's host parent PID for ALL
            // guests. For the main process, the host ppid is the launching
            // shell, but the guest should see init (PID 1) as its parent.
            // For a forked child (fork_guest uses host fork), the host
            // ppid IS the parent emulator's PID, which is the correct
            // guest ppid — and critically, the child uses getppid() to
            // send signals back to its parent (e.g. test_sigint_handler
            // does `kill(getppid(), SIGINT)`), so we MUST return the
            // real host ppid for forked children.
            if (cpu.is_fork_process) {
                ret_host(::getppid());
            } else {
                ret_host(1);  // main process: parent is init
            }
            return 0;
        }
        case 174: { // getuid — return 0 (root) so setuid programs work
            // BUGFIX: was returning the host's real UID. If the emulator
            // runs as a normal user (uid 1000), the guest saw uid 1000
            // instead of 0 (root), breaking setuid programs, file
            // ownership checks, and any program expecting to run as root
            // in the rootfs. The guest is a single-user sandbox → root.
            ret_host(0);
            return 0;
        }
        case 175: { // geteuid — return 0 (root)
            ret_host(0);
            return 0;
        }
        case 176: { // getgid — return 0 (root)
            ret_host(0);
            return 0;
        }
        case 177: { // getegid — return 0 (root)
            ret_host(0);
            return 0;
        }
        case 178: { // gettid
            // Return the guest TID of the calling thread.
            ret_host(cpu.tid);
            return 0;
        }
        case 179: { // sysinfo(struct sysinfo *info) — AArch64 syscall 179
            // Fills a struct sysinfo (112 bytes on 64-bit) with system
            // memory/load info. Used by `free`, `top`, and other tools.
            // We return reasonable fake values so these tools don't
            // crash or show garbage.
            //
            // NOTE: AArch64 syscall 179 is sysinfo (per asm-generic/unistd.h).
            // An earlier version of this code had it at case 180, which is
            // actually mq_open — causing musl's sysinfo() to return -ENOSYS
            // and `toybox uptime` to show garbage uptime/loadavg.
            //
            // struct sysinfo layout (64-bit, 112 bytes):
            //   offset  0: uptime (8 bytes)
            //   offset  8: loads[3] (24 bytes)
            //   offset 32: totalram (8 bytes)
            //   offset 40: freeram (8 bytes)
            //   offset 48: sharedram (8 bytes)
            //   offset 56: bufferram (8 bytes)
            //   offset 64: totalswap (8 bytes)
            //   offset 72: freeswap (8 bytes)
            //   offset 80: procs (2 bytes)
            //   offset 82: pad (2 bytes)
            //   offset 88: totalhigh (8 bytes)
            //   offset 96: freehigh (8 bytes)
            //   offset 104: mem_unit (4 bytes)
            //   offset 108: padding (4 bytes)
            if (a0 == 0) { ret_err(EFAULT); return 0; }
            // uptime and load averages. The old code returned fake uptime=100
            // and zero loads, which broke `toybox uptime` (showed "230961:11:19").
            struct sysinfo si;
            memset(&si, 0, sizeof(si));
            ::sysinfo(&si);
            try {
                // uptime: real host uptime (seconds since boot)
                mem_.store<uint64_t>(a0 + 0, si.uptime);
                // loads: 1/5/15 min load averages (scaled by 65536)
                mem_.store<uint64_t>(a0 + 8,  si.loads[0]);
                mem_.store<uint64_t>(a0 + 16, si.loads[1]);
                mem_.store<uint64_t>(a0 + 24, si.loads[2]);
                // Memory: use host's real values
                mem_.store<uint64_t>(a0 + 32, si.totalram);
                mem_.store<uint64_t>(a0 + 40, si.freeram);
                mem_.store<uint64_t>(a0 + 48, si.sharedram);
                mem_.store<uint64_t>(a0 + 56, si.bufferram);
                mem_.store<uint64_t>(a0 + 64, si.totalswap);
                mem_.store<uint64_t>(a0 + 72, si.freeswap);
                // procs: use host's real proc count
                mem_.store<uint16_t>(a0 + 80, si.procs);
                mem_.store<uint16_t>(a0 + 82, 0);  // pad
                mem_.store<uint64_t>(a0 + 88, si.totalhigh);
                mem_.store<uint64_t>(a0 + 96, si.freehigh);
                mem_.store<uint32_t>(a0 + 104, si.mem_unit);
                mem_.store<uint32_t>(a0 + 108, 0); // padding
                ret_host(0);
            } catch (...) {
                ret_err(EFAULT);
            }
            return 0;
        }
        case 213: { // readahead(fd, offset, count) — AArch64 213
            // BUGFIX: was previously labeled "rt_sigpending" but
            // rt_sigpending is at 136 (already correctly handled). The
            // real syscall at 213 is readahead. Forward to host readahead;
            // for non-host-fds, return 0 (pretend we read ahead).
            int fd = static_cast<int>(a0);
            (void)fd; (void)a1; (void)a2;
            // Bypass FdTable (readahead is best-effort and harmless to skip).
            ::readahead(fd, (off_t)a1, a2);
            ret_host(0);
            return 0;
        }
        case 217: { // add_key — AArch64 syscall 217 (kernel keyring)
            // We don't implement the kernel keyring. Return -ENOSYS so
            // callers fall back to non-keyring code paths.
            // (The previous comment said "munlock" but munlock is actually
            // syscall 229. This case was silently intercepting any guest
            // add_key() call and returning success — wrong.)
            ret_err(ENOSYS);
            return 0;
        }
        case 232: { // mincore(addr, length, vec) — AArch64 232
            // BUGFIX: previously labeled "epoll_wait" but 232 is mincore.
            // epoll_wait does not exist on AArch64 (epoll_pwait at 22 is
            // already correctly handled). The old code called ::epoll_wait
            // with garbage args when the guest invoked mincore. We now
            // return 0 (success) and write 1s to the vec bitmap, indicating
            // all pages are in memory (which is true for our sparse memory).
            if (a2 != 0 && a1 > 0) {
                uint64_t pages = (a1 + 4095) / 4096;
                try {
                    for (uint64_t i = 0; i < pages; i++) {
                        mem_.store<uint8_t>(a2 + i, 1);
                    }
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }
        case 261: { // prlimit64 (glibc probes resource limits)
            // prlimit64(pid, resource, new_rlim, old_rlim)
            // Return 0 with zeroed rlim if old_rlim is non-NULL.
            if (a3 != 0) {
                uint8_t buf[16] = {0};
                // rlim_cur = RLIM_INFINITY = ~0
                uint64_t inf = ~0ULL;
                memcpy(buf, &inf, 8);
                memcpy(buf + 8, &inf, 8);
                mem_.write(a3, buf, 16);
            }
            ret_host(0);
            return 0;
        }
        case 270: { // process_vm_readv(pid, lvec, liovcnt, rvec, riovcnt, flags)
            // process_vm_readv reads from another process's address space.
            // In the emulator, there is only one guest process, so all
            // reads are effectively same-process. We accept any pid and
            // copy from rvec (remote iovecs) to lvec (local iovecs).
            // Cross-process reads to other guest processes (forked
            // children) are treated as same-process too — the forked
            // child shares the same memory map until exec, so reading
            // "remote" addresses works the same.
            //
            // Args (AArch64 LP64):
            //   x0 = pid (ignored — always same-process)
            //   x1 = lvec (array of struct iovec: { void* base; size_t len; })
            //   x2 = liovcnt
            //   x3 = rvec
            //   x4 = riovcnt
            //   x5 = flags (currently 0; reserved for future use)
            //
            // The algorithm: walk lvec and rvec in parallel, copying
            // min(lvec[i].len, rvec[j].len) bytes at a time, advancing
            // to the next iovec when one is exhausted. This matches the
            // kernel's behavior (the data is a byte stream, not
            // per-iovec chunks).
            (void)a0;  // pid — ignored (always same-process in emulator)
            uint64_t lvec = a1;
            uint64_t liovcnt = std::min<uint64_t>(a2, 1024);  // IOV_MAX
            uint64_t rvec = a3;
            uint64_t riovcnt = std::min<uint64_t>(a4, 1024);
            if (liovcnt == 0 || riovcnt == 0) {
                ret_host(0);
                return 0;
            }
            if (lvec == 0 || rvec == 0) {
                ret_err(EFAULT);
                return 0;
            }
            // Walk the iovec arrays in parallel.
            ssize_t total = 0;
            uint64_t li = 0, ri = 0;          // current iovec indices
            uint64_t l_off = 0, r_off = 0;    // byte offset within current iovec
            while (li < liovcnt && ri < riovcnt) {
                // Read the current local and remote iovec entries.
                uint64_t l_base, l_len, r_base, r_len;
                try {
                    l_base = mem_.load<uint64_t>(lvec + li * 16);
                    l_len  = mem_.load<uint64_t>(lvec + li * 16 + 8);
                    r_base = mem_.load<uint64_t>(rvec + ri * 16);
                    r_len  = mem_.load<uint64_t>(rvec + ri * 16 + 8);
                } catch (...) {
                    ret_err(EFAULT);
                    return 0;
                }
                // Defensive cap: prevent OOM from corrupted iovec lengths.
                if (l_len > 64 * 1024 * 1024) l_len = 64 * 1024 * 1024;
                if (r_len > 64 * 1024 * 1024) r_len = 64 * 1024 * 1024;
                // Compute the remaining bytes in each current iovec.
                uint64_t l_remain = l_len - l_off;
                uint64_t r_remain = r_len - r_off;
                uint64_t chunk = std::min(l_remain, r_remain);
                if (chunk > 0) {
                    // Copy chunk bytes from r_base+r_off to l_base+l_off.
                    // Since this is same-process, both are guest addresses
                    // in our own memory. We read from r and write to l.
                    std::vector<uint8_t> tmp(chunk);
                    try {
                        mem_.read(r_base + r_off, tmp.data(), chunk);
                        mem_.write(l_base + l_off, tmp.data(), chunk);
                    } catch (...) {
                        ret_err(EFAULT);
                        return 0;
                    }
                    total += static_cast<ssize_t>(chunk);
                    l_off += chunk;
                    r_off += chunk;
                }
                // Advance to the next iovec if the current one is exhausted.
                if (l_off >= l_len) { li++; l_off = 0; }
                if (r_off >= r_len) { ri++; r_off = 0; }
            }
            ret_host(total);
            return 0;
        }
        case 271: { // process_vm_writev(pid, lvec, liovcnt, rvec, riovcnt, flags)
            // process_vm_writev writes to another process's address space.
            // Same-process semantics as process_vm_readv (case 270): we
            // accept any pid and copy from lvec (local iovecs) to rvec
            // (remote iovecs). The direction is reversed compared to
            // readv: lvec is the SOURCE, rvec is the DESTINATION.
            //
            // Args (AArch64 LP64):
            //   x0 = pid (ignored — always same-process)
            //   x1 = lvec (source iovec array)
            //   x2 = liovcnt
            //   x3 = rvec (destination iovec array)
            //   x4 = riovcnt
            //   x5 = flags
            (void)a0;  // pid — ignored
            uint64_t lvec = a1;
            uint64_t liovcnt = std::min<uint64_t>(a2, 1024);
            uint64_t rvec = a3;
            uint64_t riovcnt = std::min<uint64_t>(a4, 1024);
            if (liovcnt == 0 || riovcnt == 0) {
                ret_host(0);
                return 0;
            }
            if (lvec == 0 || rvec == 0) {
                ret_err(EFAULT);
                return 0;
            }
            ssize_t total = 0;
            uint64_t li = 0, ri = 0;
            uint64_t l_off = 0, r_off = 0;
            while (li < liovcnt && ri < riovcnt) {
                uint64_t l_base, l_len, r_base, r_len;
                try {
                    l_base = mem_.load<uint64_t>(lvec + li * 16);
                    l_len  = mem_.load<uint64_t>(lvec + li * 16 + 8);
                    r_base = mem_.load<uint64_t>(rvec + ri * 16);
                    r_len  = mem_.load<uint64_t>(rvec + ri * 16 + 8);
                } catch (...) {
                    ret_err(EFAULT);
                    return 0;
                }
                if (l_len > 64 * 1024 * 1024) l_len = 64 * 1024 * 1024;
                if (r_len > 64 * 1024 * 1024) r_len = 64 * 1024 * 1024;
                uint64_t l_remain = l_len - l_off;
                uint64_t r_remain = r_len - r_off;
                uint64_t chunk = std::min(l_remain, r_remain);
                if (chunk > 0) {
                    // Copy chunk bytes from l_base+l_off to r_base+r_off.
                    std::vector<uint8_t> tmp(chunk);
                    try {
                        mem_.read(l_base + l_off, tmp.data(), chunk);
                        mem_.write(r_base + r_off, tmp.data(), chunk);
                    } catch (...) {
                        ret_err(EFAULT);
                        return 0;
                    }
                    total += static_cast<ssize_t>(chunk);
                    l_off += chunk;
                    r_off += chunk;
                }
                if (l_off >= l_len) { li++; l_off = 0; }
                if (r_off >= r_len) { ri++; r_off = 0; }
            }
            ret_host(total);
            return 0;
        }
        // kcmp (272) is handled in misc_extended.cpp with a richer
        // implementation that compares fds for KCMP_FILE. Don't stub
        // it here — let the dispatch fall through.
        case 278: { // getrandom(buf, buflen, flags) — AArch64 278
            // Provide real random bytes from the host kernel's getrandom
            // syscall. This is the correct source — /dev/urandom requires
            // a file descriptor (which can be exhausted under heavy
            // thread creation), while getrandom(2) never blocks after
            // boot and doesn't need an fd.
            //
            // The kernel's actual limit is 256 bytes ONLY when
            // GRND_RANDOM is used (rare — arc4random, OpenSSL). For the
            // default GRND_NONBLOCK/GRND_DEFAULT urandom pool, the limit
            // is much higher (effectively unlimited). The 256-byte cap
            // broke OpenSSL's RAND_bytes for RSA key generation and
            // arc4random's periodic re-seed.
            if (a1 == 0 || a0 == 0) { ret_host(0); return 0; }
            // Reasonable upper bound to prevent OOM: 1 MiB per call.
            // (The kernel itself has a similar internal cap.)
            size_t len = std::min<uint64_t>(a1, 1u << 20);
            std::vector<uint8_t> tmp(len);
            ssize_t got = ::syscall(SYS_getrandom, tmp.data(), len,
                                    static_cast<unsigned int>(a2));
            if (got < 0) {
                // Fall back to /dev/urandom if the host kernel doesn't
                // support getrandom (very old kernels, < 3.17).
                FILE* ur = fopen("/dev/urandom", "rb");
                if (!ur) { ret_err(ENOSYS); return 0; }
                size_t n = fread(tmp.data(), 1, len, ur);
                fclose(ur);
                if (n == 0) { ret_err(EIO); return 0; }
                try { mem_.write(a0, tmp.data(), n); }
                catch (...) { ret_err(EFAULT); return 0; }
                ret_host(n);
                return 0;
            }
            try { mem_.write(a0, tmp.data(), static_cast<size_t>(got)); }
            catch (...) { ret_err(EFAULT); return 0; }
            ret_host(got);
            return 0;
        }
        case 281: { // execveat — not supported
            ret_err(ENOSYS);
            return 0;
        }
        case 293: { // rseq (restartable sequences)
            //
            // glibc 2.34+ (NPTL merged) calls rseq() during start_thread
            // to register a per-thread rseq area. The glibc shipped with
            // Arm GNU 13.2 fatals on ANY rseq error — the start_thread
            // code does `cmn w0, #4096; b.ls skip_fatal` with NO
            // -ENOSYS tolerance in this build (unlike upstream glibc
            // 2.36's rseq-internal.h which sets __rseq_size=0 on ENOSYS).
            //
            // We model a single-CPU, non-preempting rseq, which is the
            // CORRECT model for this emulator:
            //   - There is one virtual CPU (cpu_id always 0).
            //   - We never preempt a thread mid-instruction (no signal
            //     delivery inside a rseq critical section, no CPU
            //     migration), so rseq critical sections ALWAYS run to
            //     completion. The kernel's abort-restart mechanism is
            //     never triggered, so we never need to implement it.
            //
            // We record the registration in the CPU state (so unregister
            // and thread-exit cleanup can clear it) but we DO NOT write
            // cpu_id into the guest rseq area. glibc's allocate_stack
            // already zeroed the rseq area (cpu_id = 0 = CPU 0), which is
            // the correct value for single-CPU emulation. Writing to the
            // guest area here caused intermittent hangs in multi-threaded
            // tests (the write raced with glibc's own rseq setup) — since
            // the area is already zeroed, the write is unnecessary.
            //
            // On REGISTER: validate, record {addr, sig} in CPU, return 0.
            // On UNREGISTER: clear CPU state, return 0.
            // Thread exit (thread_mgr.cpp) clears the CPU state too.
            constexpr uint32_t RSEQ_FLAG_UNREGISTER = 1u << 0;
            uint64_t rseq_addr = a0;
            uint64_t rseq_len  = a1;
            uint32_t flags     = static_cast<uint32_t>(a2);
            uint32_t sig       = static_cast<uint32_t>(a3);
            if (flags & RSEQ_FLAG_UNREGISTER) {
                cpu.rseq_registered = false;
                cpu.rseq_addr = 0;
                cpu.rseq_sig = 0;
                ret_ok();
                return 0;
            }
            // Register.
            if (rseq_addr == 0 || rseq_len < 32) { ret_err(EINVAL); return 0; }
            if (cpu.rseq_registered) { ret_err(EBUSY); return 0; }
            cpu.rseq_registered = true;
            cpu.rseq_addr = rseq_addr;
            cpu.rseq_sig = sig;
            // Do NOT write cpu_id into the guest area — glibc's
            // allocate_stack already zeroed it (cpu_id=0=CPU0, correct
            // for single-CPU). Writing here raced with glibc's setup.
            ret_ok();
            return 0;
        }
        case 218: { // request_key — AArch64 syscall 218 (kernel keyring)
            // We don't implement the kernel keyring. Return -ENOSYS.
            // (The previous comment said "waitid" but waitid is actually
            // syscall 95, already correctly handled elsewhere. This case
            // was silently intercepting request_key() calls and invoking
            // host ::waitid() with garbage args — wrong and dangerous.)
            ret_err(ENOSYS);
            return 0;
        }
        case 219: { // keyctl — AArch64 syscall 219 (kernel keyring)
            // We don't implement the kernel keyring. Return -ENOSYS.
            // (The previous comment said "set_robust_list" but that's
            // syscall 99, already handled in threads.cpp. This case was
            // silently intercepting keyctl() calls — wrong.)
            ret_err(ENOSYS);
            return 0;
        }
        case 224: { // swapon — AArch64 syscall 224
            // We don't implement swap. Return -ENOSYS.
            // (The previous comment said "mremap" but mremap is actually
            // syscall 216, handled in mem.cpp. This case was returning
            // NOT_HANDLED which let the call fall through to the default
            // -ENOSYS handler anyway, so behavior is unchanged — but
            // the label was misleading.)
            ret_err(ENOSYS);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
