// ExclusiveMonitor.hpp — the global exclusive monitor behind LL/SC atomics.
//
// A process-wide table of bucket words, one per cache line, indexed by a hash
// of the 8-byte granule an access touches. A bucket word is a lock bit (bit 0)
// and a version (the rest, which only grows). A CPU's reservation is its own
// tag (CPU::excl_tag_*, the loaded value) plus the bucket version at
// load-exclusive time (CPU::excl_ver).
//   LDXR  reads the version, then the value, with no lock.
//   STXR  locks the bucket, requires the version to still be the LDXR's, does a
//         compare-and-swap of the loaded value, then unlocks; a successful
//         write unlocks with a version bump.
//   STLR  locks the bucket, stores, unlocks with a version bump.
// A write is therefore visible before its bump, so a reader that sees a
// version also sees every write that preceded it, and a store-exclusive can
// neither interleave with another CPU's store-exclusive/store-release to the
// same bucket nor miss one that completed after its load-exclusive (the old
// sharded-mutex monitor had that property; a lock-free bump-then-write scheme
// does not: another CPU's store can land in the cache-miss latency between the
// version check and the compare-and-swap, and recycled pointers then pass the
// value compare. jit_llsc_native's LIFO checks exactly that).
// Hash collisions only make a store-exclusive fail spuriously, which the
// architecture allows and guest retry loops absorb. Plain stores and LSE
// atomics do not bump (the previous monitor did not either); the value compare
// covers them. The interpreter, the JIT's C++ helpers and the native sequences
// (CodeGen/Integer/Atomics.cpp) all follow this protocol.
#pragma once
#include "System/Core/CPU.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sched.h>

namespace arm64emu {

struct ExclusiveMonitor {
    static constexpr unsigned kBucketBits = 11;
    static constexpr size_t kBuckets = size_t(1) << kBucketBits;
    static constexpr uint32_t kHashMul = 0x9E3779B1u;
    struct alignas(64) Bucket { std::atomic<uint64_t> word{0}; };
    Bucket buckets[kBuckets];

    // 32-bit arithmetic on the low address bits (the generated code does
    // `mov edx, eax; shr edx, 3; imul edx, edx, kHashMul; shr edx, 21`).
    static constexpr unsigned index(uint64_t addr) {
        return ((static_cast<uint32_t>(addr) >> 3) * kHashMul) >> (32 - kBucketBits);
    }
    Bucket& bucket(uint64_t addr) { return buckets[index(addr)]; }
    // The version, without the lock bit.
    static constexpr uint64_t version_of(uint64_t word) { return word & ~uint64_t(1); }

    // The version a load-exclusive records. Read it before the value.
    uint64_t version(uint64_t addr) {
        return version_of(bucket(addr).word.load(std::memory_order_acquire));
    }

    // Holds one bucket for a store. Destruction unlocks, with a version bump if
    // commit() was called (an exception leaves the version alone).
    class Hold {
    public:
        explicit Hold(Bucket& b) : b_(b) { lock(b); }
        ~Hold() {
            if (bump_) b_.word.fetch_add(1, std::memory_order_release);   // bit 0 is set: carries into the version
            else       b_.word.fetch_and(~uint64_t(1), std::memory_order_release);
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        uint64_t version() const { return version_of(b_.word.load(std::memory_order_relaxed)); }
        void commit() { bump_ = true; }
    private:
        static void lock(Bucket& b) {
            for (unsigned spins = 0;; ) {
                if (!(b.word.fetch_or(1, std::memory_order_acquire) & 1)) return;
                while (b.word.load(std::memory_order_relaxed) & 1) {
                    __builtin_ia32_pause();
                    if (++spins >= 2048) { spins = 0; sched_yield(); }
                }
            }
        }
        Bucket& b_;
        bool bump_ = false;
    };
};

static_assert(sizeof(ExclusiveMonitor::Bucket) == 64, "the JIT indexes buckets by index << 6");

inline ExclusiveMonitor g_exclusive_monitor;

} // namespace arm64emu
