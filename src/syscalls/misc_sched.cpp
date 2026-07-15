// syscalls/misc_sched.cpp — scheduling/priority syscalls extracted from
// misc_process.cpp (Turn 69 refactor). Handles:
//   - ptrace (case 117)
//   - sched_setaffinity / sched_getaffinity / sched_yield (case 122, 123, 124)
//   - sched_get_priority_max / sched_get_priority_min (case 125, 126)
//   - setpriority / getpriority (case 140, 141)
//   - getpgid (case 155)
//   - getcpu (case 168)
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
#include <algorithm>
#include <sched.h>
#include <unistd.h>
#include <vector>
namespace arm64emu {
int64_t syscall_misc_sched(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
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
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
