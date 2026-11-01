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

// Guest address of `symbol` (a defined object in .dynsym) in the glibc mapped in `mem`, or 0
// when libc is not mapped, is not glibc, or has no such symbol.
uint64_t find_guest_libc_symbol(const Memory& mem, const std::string& symbol);

// The same for a function, e.g. the GLIBC_PRIVATE __ctype_init.
uint64_t find_guest_libc_function(const Memory& mem, const std::string& symbol);

// Guest address of `symbol` (a defined function in .dynsym) in the glibc ld.so mapped in `mem`,
// e.g. the GLIBC_PRIVATE _dl_allocate_tls; 0 when there is none.
uint64_t find_guest_loader_symbol(const Memory& mem, const std::string& symbol);

}  // namespace arm64emu
