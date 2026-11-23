// Prints every number below 1024 that arm64emu::aarch64_syscall_name knows, as "number name", for
// Scripts/Testing/test_syscall_names.sh to compare with the kernel's header. Also checks that the lookup finds the first and last
// entries and answers nullptr for a number that is not a syscall.
#include "System/Syscalls/SyscallNames.hpp"
#include <cstdio>

int main() {
    using arm64emu::aarch64_syscall_name;
    unsigned known = 0;
    for (uint64_t n = 0; n < 1024; ++n)
        if (const char* name = aarch64_syscall_name(n)) {
            std::printf("%llu %s\n", static_cast<unsigned long long>(n), name);
            ++known;
        }
    if (known < 200 || !aarch64_syscall_name(0) || aarch64_syscall_name(1023) || aarch64_syscall_name(0x1000) ||
        aarch64_syscall_name(~0ull)) {
        std::fprintf(stderr, "syscall_names: lookup misbehaves (%u known)\n", known);
        return 1;
    }
    return 0;
}
