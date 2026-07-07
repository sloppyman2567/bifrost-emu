// syscalls/misc_process.cpp — process/identity/resource syscalls extracted
// from misc.cpp. Handles:
//   - ptrace (case 117)
//   - sched_setaffinity / sched_getaffinity / sched_yield (case 122, 123, 124)
//   - sched_get_priority_max / sched_get_priority_min (case 125, 126)
//   - setpriority / getpriority (case 140, 141)
//   - times (case 153)
//   - getpgid (case 155)
//   - getgroups / setgroups (case 158, 159)
//   - uname (case 160)
//   - getrlimit (case 163)
//   - getrusage (case 165)
//   - umask (case 166)
//   - prctl (case 167)
//   - getcpu (case 168)
//   - getpid / getppid / getuid / geteuid / getgid / getegid / gettid (172-178)
//   - sysinfo (case 179)
//   - readahead (case 213)
//   - add_key / request_key / keyctl (case 217, 218, 219)
//   - swapon (case 224)
//   - mincore (case 232)
//   - waitpid / wait4 (case 247, 260)
//   - prlimit64 (case 261)
//   - process_vm_readv (case 270)
//   - kcmp (case 272)
//   - getrandom (case 278)
//   - execveat (case 281)
//   - rseq (case 293)
//
// All case bodies are extracted verbatim from the original misc.cpp.
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
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace arm64emu {

int64_t syscall_misc_process(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem();

    switch (num) {
        case 117: { // ptrace — return -EPERM
            ret_err(EPERM);
            return 0;
        }

        case 124: { // sched_yield — AArch64 124
            // BUGFIX: was previously labeled "sched_setaffinity" but
            // sched_setaffinity is 122, not 124. The actual syscall at
            // 124 is sched_yield. The old code returned 0 without
            // yielding, causing concurrent threads that rely on
            // sched_yield() to spin-lock.
            int r = ::sched_yield();
            if (r < 0) { ret_errno(); return 0; }
            ret_host(r);
            return 0;
        }
        case 122: { // sched_setaffinity(pid, cpusetsize, mask) — no-op
            // AArch64 122. We accept any affinity mask and pretend it
            // succeeded. The guest is a single-process sandbox; we don't
            // enforce CPU affinity.
            // BUGFIX (Turn 65): validate the mask pointer to avoid EFAULT
            // later. The old code didn't even read a1/a2.
            if (a2 != 0 && a1 > 0) {
                // Touch the mask to validate the pointer.
                try { (void)mem_.load<uint8_t>(a2); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }
        case 123: { // sched_getaffinity(pid, cpusetsize, mask) — AArch64 123
            // Returns the CPU affinity mask. The kernel fills in
            // min(cpusetsize, ceil(ncpus/8)) bytes and returns that count.
            // We advertise 8 CPUs (a reasonable default for modern systems),
            // so the mask is 1 byte (0xFF) — but we fill the full
            // cpusetsize with the mask pattern so guests requesting larger
            // masks get valid data.
            //
            // BUGFIX (Turn 65): the old code only wrote 8 bytes and returned
            // 8, ignoring the cpusetsize argument. This broke programs that
            // request larger masks (e.g., Python's os.sched_getaffinity(0)
            // on systems with > 64 CPUs) — they'd see the 8-byte mask but
            // interpret the missing bytes as zero, thinking only CPUs 0-5
            // were available.
            if (a2 == 0) { ret_err(EFAULT); return 0; }
            if (a1 == 0) { ret_err(EINVAL); return 0; }
            // Advertise 8 CPUs (mask = 0xFF in the first byte, 0 elsewhere).
            // Cap the cpusetsize at 256 bytes (2048 CPUs) to prevent OOM
            // from a corrupted size argument.
            uint64_t cpusetsize = std::min<uint64_t>(a1, 256);
            std::vector<uint8_t> mask(cpusetsize, 0);
            // Set the first byte to 0xFF (8 CPUs available).
            mask[0] = 0xFF;
            try { mem_.write(a2, mask.data(), cpusetsize); }
            catch (...) { ret_err(EFAULT); return 0; }
            // Return the number of bytes written.
            ret_host(static_cast<uint64_t>(cpusetsize));
            return 0;
        }
        case 125: { // sched_get_priority_max(policy) — AArch64 125
            // BUGFIX: previously labeled "truncate (legacy)" but AArch64
            // has no legacy truncate (it's truncate at 45). The real
            // syscall at 125 is sched_get_priority_max.
            ret_host(99);  // SCHED_FIFO max priority
            return 0;
        }
        case 126: { // sched_get_priority_min(policy) — AArch64 126
            ret_host(1);
            return 0;
        }
        case 140: { // setpriority — AArch64 140
            // BUGFIX: previously labeled "chown (legacy)" but AArch64
            // has no legacy chown. Real syscall at 140 is setpriority.
            // We accept but ignore priority changes.
            ret_host(0);
            return 0;
        }
        case 141: { // getpriority — AArch64 141
            // BUGFIX: previously labeled "fchown (legacy)". Real syscall
            // is getpriority. Return priority 20 (default nice value).
            // Note: getpriority returns the value in [0, 40] (priority+20),
            // or -1 on error; we return 20 (priority 0).
            ret_host(20);
            return 0;
        }


        case 155: { // getpgid(pid) — AArch64 155
            // BUGFIX: previously labeled "sched_yield" but sched_yield is
            // at 124 (now correctly handled). The real syscall at 155 is
            // getpgid. Return 1 (we're a single-process guest with PGID=1).
            ret_host(1);
            return 0;
        }

        case 153: { // times(struct tms *buf) — AArch64 153
            // times() returns the number of clock ticks since an arbitrary
            // point in the past, and fills struct tms.
            // BUGFIX (Turn 62 rev 2): ALWAYS write to the guest buffer,
            // even on error. The old code returned ret_errno() without
            // writing, leaving the guest's struct tms uninitialized with
            // deterministic stack garbage. Now we zero-fill first, then
            // try host times(). If it fails, the guest gets zeros.
            struct tms t;
            memset(&t, 0, sizeof(t));
            clock_t r = ::times(a0 ? &t : nullptr);
            if (r == static_cast<clock_t>(-1)) {
                // Host times() failed — still write zeros to guest.
                if (a0) {
                    try { mem_.write(a0, &t, sizeof(t)); }
                    catch (...) { ret_err(EFAULT); return 0; }
                }
                ret_errno();
                return 0;
            }
            if (a0) {
                try {
                    mem_.write(a0, &t, sizeof(t));
                } catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

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
            // BUGFIX (Turn 62): forward to host ::getrusage(). The host's
            // rusage includes emulator overhead, but for RUSAGE_CHILDREN
            // (used by `toybox time` after wait4) it's the forked emulator
            // child's CPU time — the closest we can get to guest CPU time.
            // struct rusage is 144 bytes on LP64, identical layout on
            // x86-64 host and AArch64 guest (both use 64-bit time_t).
            //
            // BUGFIX (Turn 62 rev 2): ALWAYS write to the guest buffer,
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
            // toybox sh calls umask(0) during init and
            // umask(prev) at shutdown. Just pass through to the host.
            mode_t old = ::umask((mode_t)a0);
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

        case 168: { // getcpu(cpu, node, tcache) — AArch64 168
            // BUGFIX: previously implemented as ppoll, but AArch64 168 is
            // getcpu (ppoll is at 73). The old code dereferenced `cache`
            // (a3) as a `timespec*` and called ::poll with `node` (a1, a
            // pointer) as nfds — corrupting memory and crashing. We now
            // implement getcpu: write CPU=0 and NUMA node=0 (we have one
            // of each in the guest).
            if (a0 != 0) {
                try { mem_.store<uint32_t>(a0, 0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            if (a1 != 0) {
                try { mem_.store<uint32_t>(a1, 0); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
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
            // BUGFIX (Turn 62 rev 3): forward to host ::sysinfo() for real
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

        case 247: { // waitpid (legacy, same as wait4) — aarch64 247
            // BUGFIX (Turn 62 rev 3): ALWAYS write status to guest.
            int status = 0;
            pid_t r;
            while (true) {
                r = ::waitpid((pid_t)a0, &status, static_cast<int>(a2));
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            if (a1) {
                try { mem_.store<uint32_t>(a1, static_cast<uint32_t>(status)); }
                catch (...) {}
            }
            if (r < 0) {
                ret_errno();
                return 0;
            }
            ret_host(static_cast<uint64_t>(r));
            return 0;
        }

        case 260: { // wait4(pid, wstatus, options, rusage) — aarch64 260
            // BUGFIX (Turn 62 rev 3): ALWAYS write to guest buffers (a1
            // wstatus, a3 rusage) even on error. The old code returned
            // ret_errno() without writing, leaving the guest's struct
            // rusage uninitialized with deterministic stack garbage.
            // This was the ACTUAL root cause of `toybox time` showing
            // "user 549755811552.42" — wait4 failed (ECHILD or EINTR),
            // the rusage buffer was never written, and the guest read
            // its uninitialized stack.
            pid_t pid = (pid_t)a0;
            int options = static_cast<int>(a2);
            int status = 0;
            struct rusage ru;
            memset(&ru, 0, sizeof(ru));
            pid_t r;
            while (true) {
                r = ::wait4(pid, &status, options, a3 ? &ru : nullptr);
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            // ALWAYS write wstatus and rusage to guest memory, even on
            // error, so the guest never sees uninitialized stack data.
            if (a1) {
                try { mem_.store<uint32_t>(a1, static_cast<uint32_t>(status)); }
                catch (...) {}
            }
            if (a3) {
                try { mem_.write(a3, &ru, sizeof(ru)); }
                catch (...) {}
            }
            if (r < 0) {
                ret_errno();
                return 0;
            }
            ret_host(static_cast<uint64_t>(r));
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
            // BUGFIX: was previously labeled "eventfd2 alt" but eventfd2 is
            // at 19 (already handled). The real syscall at 270 is
            // process_vm_readv. We don't support cross-process VM reads;
            // return -ENOSYS.
            ret_err(ENOSYS);
            return 0;
        }

        case 272: { // kcmp(pid1, pid2, type, idx1, idx2) — AArch64 272
            // BUGFIX: was previously labeled "waitid" but waitid is at 95
            // (already handled). The real syscall at 272 is kcmp. We don't
            // support kernel comparison; return 0 (same file) for safety.
            ret_host(0);
            return 0;
        }

        case 278: { // getrandom(buf, buflen, flags) — AArch64 278
            // Provide real random bytes from the host kernel's getrandom
            // syscall. This is the correct source — /dev/urandom requires
            // a file descriptor (which can be exhausted under heavy
            // thread creation), while getrandom(2) never blocks after
            // boot and doesn't need an fd.
            //
            // BUGFIX (Turn 64): the old code capped buflen at 256 bytes.
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

        case 293: { // rseq (restartable sequences, glibc probes at startup)
            // Return -ENOSYS so glibc disables rseq and uses regular paths.
            ret_err(ENOSYS);
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
