// ThunkStub.hpp — in-memory AArch64 shared object backed by host thunks.
//
// A guest ld.so (--guest-ld) opens thunked libraries (libGL, libSDL2,
// libvulkan, ...) by file. The thunks live in a trampoline page, so the
// stub exports each thunked symbol as a jump to its existing trampoline.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace arm64emu {
// ET_DYN image named `soname` exporting each (name, guest address) as a
// function that branches to that address. Needs no relocations or
// dependencies; duplicate names keep the first address.
std::vector<uint8_t> build_thunk_stub_elf(
    const std::string& soname,
    const std::vector<std::pair<std::string, uint64_t>>& symbols);
}  // namespace arm64emu
