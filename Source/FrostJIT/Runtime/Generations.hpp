// Generations.hpp — code-cache generations: when and how a full FrostJIT is replaced.
//
// A FrostJIT never recycles code bytes (a stale function pointer or a live
// native frame may still point into them), so a full code cache used to leave
// the process in the interpreter for good. Instead the emulator now replaces
// the whole instance (Emulator::renew_jit): a fresh FrostJIT becomes current,
// and the full one is retired. A retired instance stays mapped while any thread
// can still run its code; once none can, a reaper thread releases its code
// mapping and per-block metadata (FrostJIT::release_resources), leaving a small
// shell behind, because generated code reads state in the FrostJIT object and
// stale pointers to it can still sit in a thread's frames.
//
// A thread can no longer run a retired instance's code when it has no
// generated-code frames and will not enter that code again:
//   - acked: at a dispatch boundary with no native frames (jit_call_depth == 0)
//     it saw an epoch at or after the retirement. A retired instance forwards
//     run_block to the current one, so the thread then runs only newer code.
//   - parked: it is blocked in a syscall that a generated block handed back to
//     C++ (cpu.jit_svc_pc), so it holds no generated frame either. Leaving the
//     park (the next dispatch boundary) exchanges the flag with a full barrier
//     before it reads the epoch, so a reaper that saw "parked" is ordered
//     before the thread's forward to the current instance.
// A thread blocked inside generated code (a thunk SVC, a nested call) is
// neither, and delays the release until it returns.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace arm64emu {
class FrostJIT;

namespace jit_generations {

struct ThreadSlot {
    std::atomic<uint64_t> acked{0};
    std::atomic<bool> parked{false};
};

// Advanced (seq_cst) by every retirement, after the instance's retired flag.
extern std::atomic<uint64_t> g_epoch;
extern thread_local ThreadSlot* tls_slot;
// Retired instances a renewal may leave unreleased. One bounds the worst case (a thread that never
// leaves its generated frames pins its generation) at twice the code budget, the same as the single
// 1 GiB cache before generations; while it is pinned the current instance, once full, falls back to
// the interpreter exactly as that cache did.
constexpr size_t kMaxUnreleased = 1;

ThreadSlot* register_thread();  // first dispatch of a host thread

// Dispatch boundary with no generated-code frames: leave a park, observe the epoch.
inline void boundary() {
    ThreadSlot* slot = tls_slot;
    if (__builtin_expect(!slot, 0)) slot = register_thread();
    if (__builtin_expect(slot->parked.load(std::memory_order_relaxed), 0))
        slot->parked.exchange(false, std::memory_order_seq_cst);
    const uint64_t epoch = g_epoch.load(std::memory_order_seq_cst);
    if (__builtin_expect(epoch != slot->acked.load(std::memory_order_relaxed), 0))
        slot->acked.store(epoch, std::memory_order_release);
}

// About to block in C++ with no generated-code frame on this thread's stack.
inline void park() {
    ThreadSlot* slot = tls_slot;
    if (__builtin_expect(!slot, 0)) slot = register_thread();
    slot->parked.store(true, std::memory_order_seq_cst);
}

// Retirement: advance the epoch and queue `retired` for release.
uint64_t retire(FrostJIT* retired);
// The instance is about to be destroyed (its emulator is going away): drop it from the queue and
// wait for a release the reaper has already started on it.
void forget(FrostJIT* jit);
// Instances retired and not yet released.
size_t unreleased();
// Totals for diagnostics, and how many threads kept the oldest unreleased
// instance alive at the reaper's last look (neither acked nor parked).
void counts(size_t& retired, size_t& released);
size_t blockers();

} // namespace jit_generations
} // namespace arm64emu
