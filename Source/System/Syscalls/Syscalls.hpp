// Syscalls.hpp — internal syscall handler declarations.
//
// Each syscall family exports a handler that takes the syscall number +
// args and returns a status:
//   - 0 if handled (result already in cpu.regs[0])
//   - SYSCALL_NOT_HANDLED if this family doesn't handle `num`
//
// The main dispatcher in Syscalls.cpp calls each handler in turn; the
// first one that handles the call wins. Unhandled syscalls fall through
// to a stub that returns -ENOSYS.
//
// Each family keeps its logic in its own file (Files/, Process/, Memory.cpp,
// HostCalls/, ...) while sharing the dispatcher. Families own disjoint
// syscall numbers, so dispatch order affects only lookup cost.
#pragma once
#include "Bifrost/Types.hpp"
#include <cstdint>
#include <errno.h>
#include <memory>
#include <string>
#include <sys/types.h>
namespace arm64emu {
namespace yggdrasil { class Node; }
// Sentinel return value: this handler didn't recognize `num`.
constexpr int64_t SYSCALL_NOT_HANDLED = INT64_MIN;
// Increment the syscall histogram for `num` (relaxed atomic — hot path).
// Used by Emulator::syscall and the JIT thunk fast path so the
// BIFROST_STATS_PERIOD printout keeps counting thunk calls.
void note_syscall(uint64_t num);
// Short name for a syscall number ("write", "mmap", "thunk", "?" when
// unknown). For crash-report lines. Never returns nullptr.
const char* syscall_name_for_crash(uint64_t num);
// ── Syscall helper macros ─────────────────────────────────────────────
// These reduce the 30+ duplicated `ret_host(static_cast<uint64_t>(static_cast<int64_t>(-X)))`
// patterns across all syscall files. They make the code more readable and
// maintainable.
// Set the return value in cpu.regs[0] to a signed value.
#define ret_host(val) do { cpu.regs[0] = static_cast<uint64_t>(val); } while(0)
// Set the return value to -errno (the most common error pattern).
#define ret_errno() do { ret_host(static_cast<int64_t>(-errno)); } while(0)
// Set the return value to a specific negative error code.
#define ret_err(code) do { ret_host(static_cast<int64_t>(-(code))); } while(0)
// Set the return value to 0 (success).
#define ret_ok() do { ret_host(0); } while(0)
// Each handler takes the Emulator (so it can access mem/fds/vfs/etc.)
// and the CPU (for register access). It reads args from cpu.regs[0..5]
// and writes the return value to cpu.regs[0] when it handles the call.
//
// Returns SYSCALL_NOT_HANDLED if `num` is not in this handler's set.
// Otherwise returns 0 (the return value is already in cpu.regs[0]).
//
// `num` is the syscall number (cpu.regs[8]).
int64_t syscall_fs(Emulator& emu, CPU& cpu, uint64_t num);
int64_t syscall_mem(Emulator& emu, CPU& cpu, uint64_t num);
int64_t syscall_threads(Emulator& emu, CPU& cpu, uint64_t num);
int64_t syscall_time(Emulator& emu, CPU& cpu, uint64_t num);
int64_t syscall_ioctls(Emulator& emu, CPU& cpu, uint64_t num);
// ── vDSO clock fast-path ────────────────────────────
// Reads the host clock directly for clock_gettime(113)/clock_getres(114)/
// gettimeofday(169) and writes the result to guest memory. Used by the
// syscall dispatcher and the JIT native vDSO fast path. Returns true if
// the call was handled (result set in cpu.regs[0]); false otherwise.
bool syscall_vdso_clock(Emulator& emu, CPU& cpu, uint64_t num);
// Syscall histogram (BIFROST_STATS_PERIOD printout). Counts every syscall;
// prints top-N with per-second rates. Defined in Syscalls.cpp.
void dump_syscall_histogram(double dt);
// ── Further Linux syscall families (dispatched after syscall_ioctls) ───
int64_t syscall_signals(Emulator& emu, CPU& cpu, uint64_t num);  // Process/Signals.cpp: sigaction/sigreturn/...
int64_t syscall_event_io(Emulator& emu, CPU& cpu, uint64_t num); // Files/EventIO.cpp: epoll/eventfd/timerfd/poll/inotify
int64_t syscall_sockets(Emulator& emu, CPU& cpu, uint64_t num);  // Files/Sockets.cpp: socket/connect/sendmsg/...
int64_t syscall_process(Emulator& emu, CPU& cpu, uint64_t num);  // Process/Process.cpp: dispatches the four below
// syscall_process() dispatches to these in order. Each returns
// SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
int64_t syscall_scheduling(Emulator& emu, CPU& cpu, uint64_t num); // ptrace/sched_*/priority/getpgid/getcpu
int64_t syscall_identity(Emulator& emu, CPU& cpu, uint64_t num);   // getuid/getpid/uname/sysinfo/getrandom/unshare/etc
int64_t syscall_wait(Emulator& emu, CPU& cpu, uint64_t num);       // times/waitpid/wait4/waitid
int64_t syscall_exit(Emulator& emu, CPU& cpu, uint64_t num);       // exit/exit_group
// Compatibility.cpp — extended syscalls: xattr, kcmp, membarrier,
// copy_file_range, pkey_*, pidfd_*, io_uring stubs, mount-API stubs,
// capget/capset, personality, fanotify stubs, landlock stubs, seccomp stub,
// mseal, etc.
int64_t syscall_compatibility(Emulator& emu, CPU& cpu, uint64_t num);
// ── Bifrost-private host calls (numbers >= 0x1000, HostCalls/) ─────────
int64_t syscall_host_thunks(Emulator& emu, CPU& cpu, uint64_t num);      // Thunks.cpp: 0x1000, 0x100D
int64_t syscall_android_launcher(Emulator& emu, CPU& cpu, uint64_t num); // Android.cpp: 0x100E
int64_t syscall_linker_services(Emulator& emu, CPU& cpu, uint64_t num);  // DynamicLinker.cpp: 0x1001-0x100C
// openat()/openat2() path resolution with GUEST flags and mode: flag
// translation, umask, dirfd, and /proc/{self,thread-self,1}/fd/N[/rest]
// through GUEST fd N; everything else goes through the VFS. Returns
// nullptr with *err set to a negative errno on failure.
std::unique_ptr<yggdrasil::Node> open_guest_at(Emulator& emu, uint64_t guest_dirfd,
    const std::string& path, int guest_flags, mode_t guest_mode, int* err);
} // namespace arm64emu
