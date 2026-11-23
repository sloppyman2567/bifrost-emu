// SyscallNames.hpp — the AArch64 name of a guest syscall number: the labels of the syscall statistics and crash lines and of
// the Tracy timeline.
#pragma once
#include <cstdint>

namespace arm64emu {

// The Linux AArch64 name of syscall `number` ("openat", "futex"), or nullptr when the number is not one.
const char* aarch64_syscall_name(uint64_t number);

}  // namespace arm64emu
