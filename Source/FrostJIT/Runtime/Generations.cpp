// Generations.cpp — retiring a full FrostJIT and releasing it once no thread can run its code.
// See Generations.hpp for the protocol.
#include "FrostJIT/Runtime/Generations.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <vector>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace arm64emu {
namespace jit_generations {

std::atomic<uint64_t> g_epoch{1};
thread_local ThreadSlot* tls_slot = nullptr;

namespace {

struct Registry {
    std::mutex mu;
    std::condition_variable wake;
    std::vector<ThreadSlot*> slots;
    std::vector<FrostJIT*> pending;    // retired, not yet handed to the reaper's release
    std::vector<FrostJIT*> releasing;  // being released right now (without the lock)
    std::condition_variable released_cv;
    bool reaper_started = false;
    std::atomic<size_t> retired_total{0}, released_total{0};  // read without the lock by renew_jit
    std::atomic<size_t> blockers{0};
};

// Never destroyed: the reaper and late thread exits may run during static destruction.
std::atomic<Registry*> g_registry{nullptr};

Registry& registry() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_registry.store(new Registry, std::memory_order_release);
        // A fork child has only the forking thread: the other threads' slots and any queued
        // retirement are gone with them, and the reaper does not exist. Start the child's
        // registry afresh with the calling thread's slot (it may have been locked by another thread).
        pthread_atfork(nullptr, nullptr, [] {
            auto* fresh = new Registry;
            if (tls_slot) fresh->slots.push_back(tls_slot);
            g_registry.store(fresh, std::memory_order_release);
        });
    });
    return *g_registry.load(std::memory_order_acquire);
}

struct SlotHolder {
    ThreadSlot* slot = nullptr;
    ~SlotHolder() {
        if (!slot) return;
        Registry& r = registry();
        {
            std::lock_guard<std::mutex> g(r.mu);
            r.slots.erase(std::remove(r.slots.begin(), r.slots.end(), slot), r.slots.end());
        }
        tls_slot = nullptr;
        delete slot;
    }
};
thread_local SlotHolder tls_holder;

// Every thread has acked the retirement or is parked in C++. `blocking` counts the others.
bool quiescent(const Registry& r, uint64_t epoch, size_t& blocking) {
    blocking = 0;
    for (const ThreadSlot* slot : r.slots) {
        if (slot->parked.load(std::memory_order_seq_cst)) continue;
        if (slot->acked.load(std::memory_order_acquire) >= epoch) continue;
        ++blocking;
    }
    return blocking == 0;
}

void* reaper_main(void*) {
    Registry& r = registry();
    std::unique_lock<std::mutex> lock(r.mu);
    unsigned idle_rounds = 0;
    for (;;) {
        if (r.pending.empty()) { idle_rounds = 0; r.blockers.store(0, std::memory_order_relaxed); r.wake.wait(lock); continue; }
        std::vector<FrostJIT*> ready;
        size_t blocking = 0;
        for (auto it = r.pending.begin(); it != r.pending.end();) {
            if (quiescent(r, (*it)->retired_epoch(), blocking)) { ready.push_back(*it); it = r.pending.erase(it); }
            else ++it;
        }
        r.blockers.store(r.pending.empty() ? 0 : blocking, std::memory_order_relaxed);
        if (!ready.empty()) {
            idle_rounds = 0;
            r.releasing = ready;
            lock.unlock();
            for (FrostJIT* jit : ready) jit->release_resources();
#ifdef __GLIBC__
            malloc_trim(0);  // hand the freed per-block nodes back to the OS
#endif
            lock.lock();
            r.releasing.clear();
            r.released_total.fetch_add(ready.size(), std::memory_order_relaxed);
            r.released_cv.notify_all();
        }
        // A thread inside generated frames can hold a generation for a long time: back off.
        if (!r.pending.empty())
            r.wake.wait_for(lock, std::chrono::milliseconds(5u << std::min(idle_rounds++, 5u)));
    }
    return nullptr;
}

void start_reaper_locked(Registry& r) {
    if (r.reaper_started) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t tid;
    r.reaper_started = pthread_create(&tid, &attr, reaper_main, nullptr) == 0;
    pthread_attr_destroy(&attr);
}

} // namespace

ThreadSlot* register_thread() {
    Registry& r = registry();
    auto* slot = new ThreadSlot;
    // A thread that starts after a retirement never ran that code.
    slot->acked.store(g_epoch.load(std::memory_order_seq_cst), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> g(r.mu);
        r.slots.push_back(slot);
    }
    tls_holder.slot = slot;
    tls_slot = slot;
    return slot;
}

uint64_t retire(FrostJIT* retired) {
    Registry& r = registry();
    const uint64_t epoch = g_epoch.fetch_add(1, std::memory_order_seq_cst) + 1;
    retired->set_retired_epoch(epoch);
    std::lock_guard<std::mutex> g(r.mu);
    r.pending.push_back(retired);
    r.retired_total.fetch_add(1, std::memory_order_relaxed);
    start_reaper_locked(r);
    r.wake.notify_one();
    return epoch;
}

void forget(FrostJIT* jit) {
    Registry& r = registry();
    std::unique_lock<std::mutex> lock(r.mu);
    auto it = std::find(r.pending.begin(), r.pending.end(), jit);
    if (it != r.pending.end()) {
        r.pending.erase(it);
        // Counted as released so the cap on unreleased instances does not stay occupied.
        r.released_total.fetch_add(1, std::memory_order_relaxed);
    }
    r.released_cv.wait(lock, [&] {
        return std::find(r.releasing.begin(), r.releasing.end(), jit) == r.releasing.end();
    });
}

size_t unreleased() {
    Registry& r = registry();
    const size_t released = r.released_total.load(std::memory_order_relaxed);
    return r.retired_total.load(std::memory_order_relaxed) - released;
}

size_t blockers() { return registry().blockers.load(std::memory_order_relaxed); }

void counts(size_t& retired, size_t& released) {
    Registry& r = registry();
    released = r.released_total.load(std::memory_order_relaxed);
    retired = r.retired_total.load(std::memory_order_relaxed);
}

} // namespace jit_generations

// Called by Emulator::renew_jit after it published the successor. The retired flag is visible
// before the epoch advances, so a thread that observes the new epoch also sees the flag.
void FrostJIT::retire() {
    // Drop every block and revoke every chain and edge: running code leaves at its next exit
    // instead of following links into the old generation for as long as it likes.
    invalidate_all();
    retired_.store(true, std::memory_order_seq_cst);
    jit_generations::retire(this);
}

// The reaper found every thread past this instance. Frees what only its own generated code
// referenced: the code mapping, the fault table and the per-block metadata (including the
// heap objects the code addressed directly: MT edge cells, counters, Tier-2 monitors). The
// object itself stays, because generated code embeds the address of members such as
// cache_generation_ and a parked thread may still return through run_block on it.
void FrostJIT::release_resources() {
    std::lock_guard<std::mutex> compiling(compile_mutex_);
    std::unique_lock<std::shared_mutex> table(blocks_mutex_);
    if (released_.load(std::memory_order_relaxed)) return;
    jit_disabled_.store(true, std::memory_order_relaxed);
    code_budget_exhausted_ = true;
    // The fault handler tests fault_sites_ before it reads anything else.
    destroy_fault_recovery();
    fault_sites_ = nullptr;
    fault_site_count_.store(0, std::memory_order_relaxed);
    fault_site_constructed_ = 0;
    if (code_buf_) {
        munmap(code_buf_, code_buf_limit_);
        code_buf_ = nullptr;
        code_buf_used_ = 0;
        code_capacity_ = 0;
    }
    auto clear = [](auto& container) { std::remove_reference_t<decltype(container)>().swap(container); };
    // Blocks first: their entries point at the monitors and edges below.
    clear(blocks_);
    clear(code_index_);
    clear(back_refs_);
    clear(pending_call_sites_);
    clear(direct_call_sites_);
    clear(mt_edge_refs_);
    clear(mt_source_refs_);
    clear(mt_edges_);
    clear(hot_counter_sites_);
    clear(hot_counters_);
    clear(tier2_trials_);
    clear(tier2_region_monitors_);
    clear(tier2_profiles_by_pc_);
    clear(tier2_settled_);
    clear(tier2_settle_queue_);
    clear(tier2_refresh_queue_);
    clear(tier2_branch_profiles_);
    clear(tier2_retrains_);
    clear(tier2_block_limits_);
    clear(code_words_);
    clear(flag_dep_table_);
    clear(retired_pcs_);
    clear(compile_invalidations_);
    clear(allocation_events_);
    released_.store(true, std::memory_order_release);
}

} // namespace arm64emu
