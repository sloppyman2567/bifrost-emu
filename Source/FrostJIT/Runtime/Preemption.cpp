// Preemption.cpp — epoch ticker behind Preemption.hpp.
#include "FrostJIT/Runtime/Preemption.hpp"
#include <pthread.h>
#include <time.h>
namespace arm64emu {
namespace {
std::atomic<bool> ticker_running{false};
void* tick_main(void*) {
    const timespec delay{0, static_cast<long>(JIT_PREEMPT_TICK_US) * 1000};
    for (;;) {
        nanosleep(&delay, nullptr);
        g_jit_preempt_epoch.fetch_add(1, std::memory_order_release);
    }
    return nullptr;
}
} // namespace
// A fork child runs interpreter-only (Emulator::fork_guest), so it never
// executes polled native code and needs no ticker of its own.
void jit_start_preempt_ticker() {
    bool expected = false;
    if (!ticker_running.compare_exchange_strong(expected, true)) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t tid;
    if (pthread_create(&tid, &attr, tick_main, nullptr) != 0)
        ticker_running.store(false, std::memory_order_relaxed);
    pthread_attr_destroy(&attr);
}
} // namespace arm64emu
