// SpinWait.hpp — waiting without sleeping for something another thread finishes in microseconds, which is less than a sleep and
// a wake cost. Bounded by time, since the length of a pause differs between CPUs.
#pragma once
#include <chrono>

namespace arm64emu {

// True once done() holds; false after the limit, and the caller sleeps.
template <class Done> bool spin_until(std::chrono::nanoseconds limit, Done&& done) {
    const auto end = std::chrono::steady_clock::now() + limit;
    do {
        for (int i = 0; i < 8; ++i) __builtin_ia32_pause();
        if (done()) return true;
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
constexpr std::chrono::microseconds kSpinLimit{20};

}  // namespace arm64emu
