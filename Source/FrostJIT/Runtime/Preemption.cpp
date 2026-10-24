// Preemption.cpp — epoch ticker behind Preemption.hpp.
#include "FrostJIT/Runtime/Preemption.hpp"
#include <pthread.h>
#include <time.h>
#include <mutex>
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
// A fork child has none of the parent's threads, so a ticker the parent started is gone while
// the flag still says it runs. Forget it: the child is interpreter-only until it execs, then
// gets a fresh JIT (Emulator::restart_jit_for_exec), and if that program goes multi-threaded
// it must start its own ticker.
void forget_ticker_in_child() { ticker_running.store(false, std::memory_order_relaxed); }

void jit_start_preempt_ticker() {
    static std::once_flag atfork_once;
    std::call_once(atfork_once, [] { pthread_atfork(nullptr, nullptr, forget_ticker_in_child); });
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
