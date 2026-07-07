// syscalls/syscalls.h — internal syscall handler declarations.
//
// Each subsystem (fs, mem, threads, time, ioctls) exports a handler that
// takes the syscall number + args and returns either:
//   - the syscall return value (already set in cpu.regs[0] if applicable)
//   - SYSCALL_NOT_HANDLED if this subsystem doesn't handle `num`
//
// The main dispatcher in syscalls.cpp calls each handler in turn; the
// first one that handles the call wins. Unhandled syscalls fall through
// to a stub that returns -ENOSYS.
//
// This split keeps each subsystem's logic in its own file (fs.cpp for
// file ops, mem.cpp for mmap/brk, etc.) while sharing the dispatcher.
#pragma once

#include "bifrost/types.hpp"

#include <cstdint>
#include <errno.h>

namespace arm64emu {

// Sentinel return value: this handler didn't recognize `num`.
constexpr int64_t SYSCALL_NOT_HANDLED = INT64_MIN;

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
int64_t syscall_misc(Emulator& emu, CPU& cpu, uint64_t num);

// ── misc.cpp sub-handlers (Turn 68 refactor) ──────────────────────────
// syscall_misc() dispatches to these in order. Each returns
// SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
int64_t syscall_misc_signal(Emulator& emu, CPU& cpu, uint64_t num);  // sigaction/sigreturn/etc
int64_t syscall_misc_io(Emulator& emu, CPU& cpu, uint64_t num);      // epoll/eventfd/timerfd/socket/poll
int64_t syscall_misc_process(Emulator& emu, CPU& cpu, uint64_t num); // getpid/wait/rlimit/prctl/etc

// ── misc_process.cpp sub-handlers (Turn 69 refactor) ──────────────────
// syscall_misc_process() dispatches to these in order. Each returns
// SYSCALL_NOT_HANDLED if it doesn't recognize `num`.
int64_t syscall_misc_sched(Emulator& emu, CPU& cpu, uint64_t num);  // ptrace/sched_*/priority/getpgid/getcpu
int64_t syscall_misc_id(Emulator& emu, CPU& cpu, uint64_t num);     // getuid/getpid/uname/sysinfo/getrandom/etc
int64_t syscall_misc_wait(Emulator& emu, CPU& cpu, uint64_t num);   // times/waitpid/wait4

// ── misc_extended.cpp (v1.5.0.alpha) ────────────────────────────────
// Extended syscalls: xattr, kcmp, membarrier, copy_file_range, pkey_*,
// pidfd_*, io_uring stubs, mount-API stubs, capget/capset, personality,
// fanotify stubs, landlock stubs, seccomp stub, mseal, etc.
int64_t syscall_misc_extended(Emulator& emu, CPU& cpu, uint64_t num);

} // namespace arm64emu
