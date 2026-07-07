// syscalls/misc_wait.cpp — wait/times syscalls extracted from
// misc_process.cpp (Turn 69 refactor). Handles:
//   - times (case 153)
//   - waitpid (case 247)
//   - wait4 (case 260)
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
#include <cstring>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace arm64emu {

int64_t syscall_misc_wait(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3];
    auto& mem_ = emu.mem();

    switch (num) {
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

        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}

} // namespace arm64emu
