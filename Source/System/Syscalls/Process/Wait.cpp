// Wait.cpp — wait/times syscalls, dispatched from
// syscall_process() in Process.cpp. Handles:
//   - times (case 153)
//   - waitpid (case 247)
//   - wait4 (case 260)
//   - waitid (case 95)
//
// NOTE: this file is not a friend of Emulator. It accesses
// private state via the public accessors emu.mem(), emu.fds(), etc.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include <errno.h>
#include <cstring>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
namespace arm64emu {
static pid_t guest_wait4(Emulator& emu, CPU& cpu, pid_t pid, int* status,
                         int options, struct rusage* usage) {
    if (!emu.debugger().wait_polling()) return ::wait4(pid, status, options, usage);
    for (;;) {
        pid_t result = ::wait4(pid, status, options | WNOHANG, usage);
        if (result > 0) {
            if (WIFEXITED(*status) || WIFSIGNALED(*status)) emu.debugger().reaped(result);
            return result;
        }
        if (result < 0 && errno != ECHILD) return result;
        int tid = 0;
        if (emu.debugger().take_wait(pid, true, true, true, tid, *status)) {
            if (usage) memset(usage, 0, sizeof(*usage));
            return tid;
        }
        if (result < 0 && !emu.debugger().has_tracees(pid)) return result;
        if (options & WNOHANG) return 0;
        if (emu.group_exit_requested() || cpu.has_unblocked_pending_signals()) {
            errno = EINTR; return -1;
        }
        emu.debugger().wait_change();
    }
}
static int guest_waitid(Emulator& emu, CPU& cpu, idtype_t type, id_t id,
                        siginfo_t* info, int options) {
    if (type != P_ALL && type != P_PID && type != P_PGID) return ::waitid(type, id, info, options);
    int selector = type == P_ALL ? -1 : type == P_PID ? static_cast<int>(id) : -static_cast<int>(id);
    if (!emu.debugger().wait_polling()) return ::waitid(type, id, info, options);
    for (;;) {
        int result = ::waitid(type, id, info, options | WNOHANG);
        if (result == 0 && info->si_pid) {
            if (!(options & WNOWAIT) && (info->si_code == CLD_EXITED || info->si_code == CLD_KILLED || info->si_code == CLD_DUMPED))
                emu.debugger().reaped(info->si_pid);
            return 0;
        }
        if (result < 0 && errno != ECHILD) return result;
        int tid = 0, status = 0;
        if (emu.debugger().take_wait(selector, options & WSTOPPED, options & WEXITED,
                                    !(options & WNOWAIT), tid, status)) {
            memset(info, 0, sizeof(*info));
            info->si_signo = SIGCHLD; info->si_pid = tid; info->si_uid = emu.vfs().process_uid();
            info->si_code = WIFSTOPPED(status) ? CLD_TRAPPED : WIFEXITED(status) ? CLD_EXITED : CLD_KILLED;
            info->si_status = WIFSTOPPED(status) ? status >> 8 : WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
            return 0;
        }
        if (result < 0 && !emu.debugger().has_tracees(selector)) return result;
        if (options & WNOHANG) { memset(info, 0, sizeof(*info)); return 0; }
        if (emu.group_exit_requested() || cpu.has_unblocked_pending_signals()) { errno = EINTR; return -1; }
        emu.debugger().wait_change();
    }
}
int64_t syscall_wait(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3];
    auto& mem_ = emu.mem();
    switch (num) {
        case 153: { // times(struct tms *buf) — AArch64 153
            // times() returns the number of clock ticks since an arbitrary
            // point in the past, and fills struct tms.
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
        case 247: { // waitpid (legacy, same as wait4) — aarch64 247
            int status = 0;
            pid_t r;
            while (true) {
                r = guest_wait4(emu, cpu, (pid_t)a0, &status, static_cast<int>(a2), nullptr);
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
                r = guest_wait4(emu, cpu, pid, &status, options, a3 ? &ru : nullptr);
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
        case 95: { // waitid(idtype, id, infop, options) — AArch64 95
            // written at offset 4 (where si_errno belongs) and si_pid at
            // offset 8 (where si_code belongs). The correct AArch64
            // siginfo_t layout is:
            //   offset  0: si_signo  (4 bytes)
            //   offset  4: si_errno  (4 bytes)
            //   offset  8: si_code   (4 bytes)
            //   offset 12: __pad     (4 bytes)
            //   offset 16: _sigchld.si_pid    (4 bytes)
            //   offset 20: _sigchld.si_uid    (4 bytes)
            //   offset 24: _sigchld.si_status (4 bytes)
            //   offset 28: __pad              (4 bytes)
            //   offset 32: _sigchld.si_utime  (8 bytes)
            //   offset 40: _sigchld.si_stime  (8 bytes)
            // This matches build_siginfo() in Source/System/Core/Signals.cpp and the
            // Linux kernel's struct siginfo for AArch64.
            siginfo_t si;
            memset(&si, 0, sizeof(si));
            int r;
            while (true) {
                r = guest_waitid(emu, cpu, static_cast<idtype_t>(a0), static_cast<id_t>(a1), &si, static_cast<int>(a3));
                if (r >= 0 || errno != EINTR) break;
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-EINTR));
                if (emu.handle_eintr(cpu)) return 0;  // handler will run
                break;  // no signal delivered, return -EINTR
            }
            // ALWAYS write siginfo to guest, even on error.
            if (a2) {
                try {
                    mem_.store<uint32_t>(a2 + 0,  static_cast<uint32_t>(si.si_signo));
                    mem_.store<uint32_t>(a2 + 4,  0);  // si_errno (always 0)
                    mem_.store<uint32_t>(a2 + 8,  static_cast<uint32_t>(si.si_code));
                    mem_.store<uint32_t>(a2 + 12, 0);  // __pad
                    mem_.store<uint32_t>(a2 + 16, static_cast<uint32_t>(si.si_pid));
                    mem_.store<uint32_t>(a2 + 20, static_cast<uint32_t>(si.si_uid));
                    mem_.store<uint32_t>(a2 + 24, static_cast<uint32_t>(si.si_status));
                    mem_.store<uint32_t>(a2 + 28, 0);  // __pad
                    // si_utime / si_stime (offset 32/40) — only valid when
                    // si_code == CLD_TRAPPED with WUNTRACED; leave as 0.
                } catch (...) {}
            }
            if (r < 0) { ret_errno(); return 0; }
            ret_host(0); return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
