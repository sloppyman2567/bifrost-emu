// Fork.cpp — a JIT that a fork child keeps (Emulator::fork_guest).
//
// A host fork copies only the forking thread. Every lock another thread may hold while it translates, publishes
// or invalidates is taken before the fork, so the child finds the JIT's tables consistent, and re-created in the
// child: a std::shared_mutex copies its count of waiting writers, which no thread there will ever clear
// (Memory::reset_mutex_in_fork_child does the same). The code buffer is a private mapping, so parent and child
// each patch and extend only their own copy.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/Preemption.hpp"
#include <new>
namespace arm64emu {

bool FrostJIT::try_lock_for_fork() {
    if (!blocks_mutex_.try_lock()) return false;
    if (!compile_mutex_.try_lock()) { blocks_mutex_.unlock(); return false; }
    if (!compile_invalidations_mu_.try_lock()) {
        compile_mutex_.unlock();
        blocks_mutex_.unlock();
        return false;
    }
    // The compilers work without a lock: hold the pool and find them all idle. While they are not, new compiles wait a
    // moment, or a process whose threads translate all the time would never let the fork in.
    const bool pool = pool_mutex_.try_lock();
    if (pool && compiling_pcs_.empty()) return true;
    if (pool) {
        compiles_paused_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        pool_mutex_.unlock();
    }
    compile_invalidations_mu_.unlock();
    compile_mutex_.unlock();
    blocks_mutex_.unlock();
    return false;
}

void FrostJIT::unlock_after_fork() {
    compiles_paused_until_ = {};
    pool_mutex_.unlock();
    pool_free_.notify_all();
    compile_invalidations_mu_.unlock();
    compile_mutex_.unlock();
    blocks_mutex_.unlock();
}

void FrostJIT::reset_after_fork_child() {
    // No compiler was busy; the ones the parent made are idle here too.
    new (&pool_mutex_) std::mutex();
    new (&pool_free_) std::condition_variable();
    new (&pool_done_) std::condition_variable();
    new (&lease_mutex_) std::mutex();
    mt_targets_.renew_locks();
    pool_waiters_ = done_waiters_ = 0;
    compiles_paused_until_ = {};
    for (size_t i = 0; i < compilers_count_.load(std::memory_order_relaxed); ++i)
        new (&compilers_[i]->compile_invalidations_mu_) std::mutex();
    new (&compile_invalidations_mu_) std::mutex();
    new (&compile_mutex_) std::mutex();
    new (&blocks_mutex_) std::shared_mutex();
    shared_compile_active_.store(false, std::memory_order_relaxed);
    compile_invalidations_.clear();
    compile_invalidations_overflow_ = false;
    // MT code returns to the dispatcher (signals, exit) only when the epoch moves, and the parent's ticker is gone.
    if (mt_active()) jit_start_preempt_ticker();
}

}  // namespace arm64emu
