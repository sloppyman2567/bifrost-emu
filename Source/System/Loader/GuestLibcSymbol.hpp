// GuestLibcSymbol.hpp — find a symbol in the glibc that a guest ld.so mapped.
//
// Under --guest-ld the guest's own ld.so loads libc, so the emulator's DynamicLinker never indexes
// it. The emulator still has to reach a few libc variables (__libc_single_threaded), so this reads
// the symbol from libc's file and adds the base of its mapping.
#pragma once
#include <cstdint>
#include <string>

namespace arm64emu {

class Memory;

// Guest address of `symbol` (a defined, non-TLS object in .dynsym) in the glibc mapped in `mem`, or 0
// when libc is not mapped, is not glibc, or has no such symbol.
uint64_t find_guest_libc_symbol(const Memory& mem, const std::string& symbol);

}  // namespace arm64emu
