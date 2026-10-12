// MemoryInternal.hpp — state shared by the Memory implementation files
// (Memory.cpp, Allocation.cpp, Discard.cpp). Include only from
// Source/System/Memory/.
#pragma once
#include <atomic>
#include <cstdint>
namespace arm64emu {
// Cumulative bytes allocated/freed, reported by BIFROST_MEMSTATS.
extern std::atomic<uint64_t> g_memstats_cum_alloc, g_memstats_cum_free;
} // namespace arm64emu
