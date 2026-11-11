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
    // A trial's lock is taken by a vCPU that times one of its windows; holding the table lock keeps the set of
    // trials fixed, and a trial lock is held only for a few statements.
    size_t held = 0;
    for (; held < tier2_trials_.size(); ++held)
        if (!tier2_trials_[held]->mutex.try_lock()) break;
    if (held == tier2_trials_.size()) return true;
    while (held) tier2_trials_[--held]->mutex.unlock();
    compile_invalidations_mu_.unlock();
    compile_mutex_.unlock();
    blocks_mutex_.unlock();
    return false;
}

void FrostJIT::unlock_after_fork() {
    for (auto& trial : tier2_trials_) trial->mutex.unlock();
    compile_invalidations_mu_.unlock();
    compile_mutex_.unlock();
    blocks_mutex_.unlock();
}

void FrostJIT::reset_after_fork_child() {
    for (auto& trial : tier2_trials_) new (&trial->mutex) std::mutex();
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
