// TranslationClock.hpp — BIFROST_PROF: splits one translation into waiting for locks and translating, for the thread's
// line of the waiting report (System/Core/WaitProfile.hpp).
#pragma once
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/WaitProfile.hpp"
#include <chrono>
#include <cstdint>

namespace arm64emu {

class FrostJIT::TranslationClock {
public:
    using Time = std::chrono::steady_clock::time_point;
    static uint64_t elapsed_ns(Time start) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    }
    TranslationClock() : on_(dbg().cpu_profile) { if (on_) start_ = std::chrono::steady_clock::now(); }
    ~TranslationClock() {
        if (!on_) return;
        const uint64_t total = elapsed_ns(start_);
        wait_prof_note_translation(total - wait_ns_, wait_ns_);
    }
    template <class Lock> void wait(Lock&& lock) {
        if (!on_) { lock(); return; }
        const Time from = std::chrono::steady_clock::now();
        lock();
        wait_ns_ += elapsed_ns(from);
    }
private:
    const bool on_;
    Time start_;
    uint64_t wait_ns_ = 0;
};

}  // namespace arm64emu
