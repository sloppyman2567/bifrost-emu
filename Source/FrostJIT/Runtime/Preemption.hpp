// Preemption.hpp — bounded native execution for multithreaded FrostJIT code.
//
// Generated MT code never decrements a shared counter. Backward edges and
// indirect-branch probes only compare the CPU's observed epoch with the global
// one (two loads, no store); a mismatch returns to the dispatcher, which
// grants a fresh lease. A ticker thread advances the epoch every millisecond
// once the process runs a second vCPU, so signals, group exits and other
// dispatcher work are never starved by a native loop.
#pragma once
#include "System/Core/CPU.hpp"
#include <atomic>
#include <cstdint>
namespace arm64emu {
// Read by generated code through its absolute address; only the ticker (and
// tests forcing an expiry) write it.
inline std::atomic<uint32_t> g_jit_preempt_epoch{0};
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t) &&
              std::atomic<uint32_t>::is_always_lock_free);
// Host-side tick delay between epoch advances.
constexpr unsigned JIT_PREEMPT_TICK_US = 1000;
// Dispatcher entry: restore the ST edge budget and adopt the current epoch.
// The epoch is read before the code runs, so a tick landing afterwards is
// seen by the next poll.
inline void jit_grant_lease(CPU& cpu) {
    cpu.jit_chain_budget = 256;
    cpu.jit_epoch_seen = g_jit_preempt_epoch.load(std::memory_order_relaxed);
}
// Start the process-wide ticker (idempotent).
void jit_start_preempt_ticker();
// Last epoch a dispatch loop acted on; a loop drains signals and similar
// host work whenever the tick moved on. Cheap enough for every dispatch.
inline bool jit_preempt_tick(uint32_t& last) {
    const uint32_t now = g_jit_preempt_epoch.load(std::memory_order_relaxed);
    if (now == last) return false;
    last = now;
    return true;
}
} // namespace arm64emu
