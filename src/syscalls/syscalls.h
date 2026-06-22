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

namespace arm64emu {

// Sentinel return value: this handler didn't recognize `num`.
constexpr int64_t SYSCALL_NOT_HANDLED = INT64_MIN;

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
int64_t syscall_misc(Emulator& emu, CPU& cpu, uint64_t num);  // catch-all in syscalls.cpp

} // namespace arm64emu
