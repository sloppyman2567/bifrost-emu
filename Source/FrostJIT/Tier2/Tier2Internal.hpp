// Tier2Internal.hpp — helpers shared between the Tier-2 source files.
// Not part of FrostJIT's interface; include only from Source/FrostJIT/Tier2/.
#pragma once
#include <cstdint>

namespace arm64emu {
class Emulator;
class CPU;
namespace tier2 {
// Generated trial-wrapper entry: runs one measured window (Trials.cpp).
uint64_t measured_region_entry(CPU* cpu, Emulator* emu, void* trial);
// Cold-exit feedback tail call, emitted by the region compiler
// (Feedback.cpp). Never unwinds into generated code.
uint64_t feedback_exit(CPU* cpu, Emulator* emu, void* monitor) noexcept;
}  // namespace tier2
}  // namespace arm64emu
