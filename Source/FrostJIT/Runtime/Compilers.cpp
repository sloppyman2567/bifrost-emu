// Compilers.cpp — several threads translating at once (multithreaded, shared-JIT mode).
//
// The core (the FrostJIT the emulator knows) owns the code buffer and the block table. A compiler is another FrostJIT
// that borrows them: it emits into a chunk of the buffer it leased, keeps its scratch state and everything the compile
// creates (edges, code words, flag dependencies, register-passing records, side exits) to itself, and holds no lock
// while it works. The core adopts a finished translation under the exclusive table lock, which is also where it checks
// that no invalidation touched the code meanwhile. Generated code addresses the core, globals and cells, never a compiler.
//
// Compiles stay serialized (compile_entry's compile_mutex_ path) with one compiler, without the shared JIT, under
// differential verification and with the persistent cache, whose recording compiles a block twice.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/TranslationClock.hpp"
#include "System/Core/DebugFlags.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace arm64emu {

void FrostJIT::init_compiler_state() {
    // prev_max_vreg_ must be large enough that the first translate_block() clears all 4096 entries.
    prev_max_vreg_ = 4095;
    for (int i = 0; i < 4096; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
}

// The caller holds the core's pool_mutex_; nothing here changes once the core is multithreaded.
FrostJIT::FrostJIT(FrostJIT& core) {
    core_ = &core;
    dispatch_stats_enabled_ = core.dispatch_stats_enabled_;
    block_stats_ = core.block_stats_;
    mt_active_.store(true, std::memory_order_relaxed);
    code_buf_limit_ = core.code_buf_limit_;
    code_capacity_ = core.code_buf_limit_;
    code_buf_ = core.code_buf_;
    code_buf_used_ = code_lease_end_ = 0;
    window_base_ = core.window_base_;
    guest_memory_ = core.guest_memory_;
    fault_sites_ = core.fault_sites_;
    fault_chunks_ = core.fault_chunks_;
    vdso_base_ = core.vdso_base_;
    vdso_size_ = core.vdso_size_;
    cpu_features_ = core.cpu_features_;
    no_fma3_ = core.no_fma3_;
    no_avx2_ = core.no_avx2_;
    cold_stubs_ready_ = core.cold_stubs_ready_;
    std::copy(&core.guard_stub_[0][0][0], &core.guard_stub_[0][0][0] + 16 * 4 * 2, &guard_stub_[0][0][0]);
    fault_exit_stub_ = core.fault_exit_stub_;
    frame_exit_stub_ = core.frame_exit_stub_;
    disk_cache_checked_ = true;  // no persistent cache here
    init_compiler_state();
}

// A chunk of the code buffer for one compiler. The stubs every block shares precede the first one.
bool FrostJIT::lease_code_chunk(size_t& start) {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    start = code_next_lease_.load(std::memory_order_relaxed);
    if (!code_buf_ || start > code_buf_limit_ || code_buf_limit_ - start < kCodeChunk) return false;
    const size_t next = start + kCodeChunk;
    code_next_lease_.store(next, std::memory_order_relaxed);
    if (next > code_capacity_) {
        code_capacity_ = std::min(code_buf_limit_, std::max(next, code_capacity_ * 2));
        code_cache_growths.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
bool FrostJIT::take_code_lease() {
    size_t start = 0;
    if (!core_->lease_code_chunk(start)) return false;
    code_buf_used_ = start;
    code_lease_end_ = start + kCodeChunk;
    return true;
}

bool FrostJIT::parallel_compiles(Emulator& emu) {
    const int decided = parallel_compiles_.load(std::memory_order_acquire);
    if (decided >= 0) return decided != 0;
    // Undecided until the last line: a thread that took the serialized path meanwhile would compile in the core's last chunk.
    std::lock_guard<std::mutex> pool(pool_mutex_);
    if (parallel_compiles_ >= 0) return parallel_compiles_ != 0;
    const int compilers = set_up_compilers(emu);
    parallel_compiles_.store(compilers, std::memory_order_release);
    return compilers != 0;
}
// Prepares the core to share its buffer; the caller holds pool_mutex_. Returns how many may compile at once, 0 to serialize.
int FrostJIT::set_up_compilers(Emulator& emu) {
    // Every compiler holds a chunk of its own: a small code cache has room for few.
    const int wanted = std::min<int>(dbg().jit_compilers ? dbg().jit_compilers
                                                         : std::min<int>(kMaxCompilers, static_cast<int>(std::thread::hardware_concurrency())),
                                     static_cast<int>(code_buf_limit_ / kCodeChunk / 8));
    if (wanted <= 1 || !renewable_ || !code_buf_ || dbg().jit_verify) return 0;
    // The core compiles nothing from here on, and its own region ends at the next chunk.
    std::lock_guard<std::mutex> compiling(compile_mutex_);
    std::unique_lock<std::shared_mutex> table(blocks_mutex_);
    if (disk_cache_ready(emu)) return 0;
    make_writable();
    code_buf_overflow_ = false;
    if (!cold_stubs_ready_ && fault_sites_ && guest_memory_ && window_base_ && !dbg().fault_trace) emit_cold_stubs();
    make_executable();
    if (code_buf_overflow_) return 0;
    code_lease_end_ = std::min(code_buf_limit_, (code_buf_used_ / kCodeChunk + 1) * kCodeChunk);
    code_next_lease_.store(code_lease_end_, std::memory_order_relaxed);
    return wanted;
}

thread_local const FrostJIT* FrostJIT::tls_last_compiler_ = nullptr;

FrostJIT* FrostJIT::claim_compiler(uint64_t pc) {
    std::unique_lock<std::mutex> pool(pool_mutex_);
    for (;;) {
        if (compiles_paused_until_ != std::chrono::steady_clock::time_point{}) {
            const auto until = compiles_paused_until_;
            if (std::chrono::steady_clock::now() < until) { pool_free_.wait_until(pool, until); continue; }
            compiles_paused_until_ = {};
        }
        if (std::find(compiling_pcs_.begin(), compiling_pcs_.end(), pc) != compiling_pcs_.end()) {
            ++done_waiters_;
            pool_done_.wait(pool);
            --done_waiters_;
            return nullptr;
        }
        if (!free_compilers_.empty()) break;
        const size_t count = compilers_count_.load(std::memory_order_relaxed);
        if (count < static_cast<size_t>(parallel_compiles_.load(std::memory_order_relaxed))) {
            compilers_[count].reset(new FrostJIT(*this));
            free_compilers_.push_back(compilers_[count].get());
            compilers_count_.store(count + 1, std::memory_order_release);
            break;
        }
        ++pool_waiters_;
        pool_free_.wait(pool);
        --pool_waiters_;
    }
    auto pick = std::find(free_compilers_.begin(), free_compilers_.end(), tls_last_compiler_);
    if (pick == free_compilers_.end()) --pick;
    FrostJIT* compiler = *pick;
    *pick = free_compilers_.back();
    free_compilers_.pop_back();
    compiling_pcs_.push_back(pc);
    tls_last_compiler_ = compiler;
    return compiler;
}

void FrostJIT::release_compiler(FrostJIT* compiler, uint64_t pc) {
    bool wake_free, wake_done;
    {
        std::lock_guard<std::mutex> pool(pool_mutex_);
        compiling_pcs_.erase(std::find(compiling_pcs_.begin(), compiling_pcs_.end(), pc));
        free_compilers_.push_back(compiler);
        wake_free = pool_waiters_ != 0;
        wake_done = done_waiters_ != 0;
    }
    if (wake_free) pool_free_.notify_one();
    if (wake_done) pool_done_.notify_all();
}

bool FrostJIT::compile_entry_parallel(Emulator& emu, uint64_t pc, BlockEntry& out, TranslationClock& clock) {
    auto copy_locked = [&] {
        const auto it = blocks_.find(pc);
        if (it == blocks_.end()) return false;
        out = it->second;
        return true;
    };
    FrostJIT* compiler = nullptr;
    for (;;) {
        bool found = false;
        clock.wait([&] {
            compiler = claim_compiler(pc);
            // After the claim, so that a translation another thread has just published is seen (its claim ended after it set the bit).
            if (!block_starts_.may_contain(pc)) return;
            lock_table_shared();
            found = copy_locked();
            blocks_mutex_.unlock_shared();
        });
        if (found) {
            if (compiler) release_compiler(compiler, pc);
            return true;
        }
        if (compiler) break;
    }
    struct Claim {
        FrostJIT* core; FrostJIT* compiler; uint64_t pc;
        ~Claim() {
            compiler->shared_compile_active_.store(false, std::memory_order_relaxed);
            compiler->staging_ = compiler->table_held_ = false;
            compiler->discard_staged();
            core->release_compiler(compiler, pc);
        }
    } claim{this, compiler, pc};
    if (compiler->code_lease_end_ == 0 && !compiler->take_code_lease()) code_budget_exhausted_ = true;
    {
        std::lock_guard<std::mutex> log(compiler->compile_invalidations_mu_);
        compiler->compile_invalidations_.clear();
        compiler->compile_invalidations_overflow_ = false;
    }
    compiler->shared_compile_active_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);  // pairs with log_invalidation_for_compiles
    compiler->discard_staged();
    compiler->staging_ = true;
    compiler->translate_block(emu, pc);
    lean_shared_compiles.fetch_add(1, std::memory_order_relaxed);
    if (compiler->staged_) {
        compiler->prepare_publication(pc);
        out = compiler->staged_entry_;
    }
    clock.wait([&] { lock_table(); });
    struct Unlock { std::shared_mutex& mutex; ~Unlock() { mutex.unlock(); } } unlock{blocks_mutex_};
    const bool raced = block_starts_.may_contain(pc) && blocks_.count(pc) != 0;
    if (compiler->staged_ && !raced && compiler->staged_entry_invalidated(pc)) {
        // Guest code changed while compiling: translate the current bytes with the table held, which no invalidation can
        // interleave.
        lean_compile_retries.fetch_add(1, std::memory_order_relaxed);
        compiler->rewind_codegen(compiler->staged_start_);
        compiler->discard_staged();
        compiler->table_held_ = true;
        compiler->translate_block(emu, pc);
        if (compiler->staged_) {
            compiler->prepare_publication(pc);
            out = compiler->staged_entry_;
        }
    }
    if (!compiler->staged_ || raced) return copy_locked();
    adopt_staged(*compiler, pc, out);
    return true;
}

// What a publication needs that can be found or made before the table is locked.
void FrostJIT::prepare_publication(uint64_t pc) {
    staged_self_ = &core_->mt_targets_.at(pc);
    staged_nodes_.emplace(pc, staged_entry_);
    staged_node_ = staged_nodes_.extract(pc);
}

// Moves what `compiler` created for pc into the tables and publishes it; `out` is the caller's copy of the entry. Offsets into
// the compiler's side tables are rebased; edges keep their addresses, which the generated code holds.
void FrostJIT::adopt_staged(FrostJIT& compiler, uint64_t pc, BlockEntry& out) {
    // The compiler's words are one run from index 0 (attach_code_words bounds them), and stay one run here.
    const size_t word_count = compiler.code_words_.size();
    const uint32_t words = word_count ? static_cast<uint32_t>(code_words_.append_run(&compiler.code_words_[0], word_count)) : 0;
    const uint32_t deps = static_cast<uint32_t>(flag_dep_table_.size());
    const uint32_t passes = static_cast<uint32_t>(reg_pass_.size()), stubs = static_cast<uint32_t>(reg_stubs_.size());
    for (BlockEntry* entry : {&compiler.staged_node_.mapped(), &out}) {
        entry->code_words_off += words;
        entry->flag_deps_off += deps;
        if (entry->reg_pass) entry->reg_pass += passes;
    }
    for (FlagDep& dep : compiler.flag_dep_table_) {
        if (dep.words_off != UINT32_MAX) dep.words_off += words;
        flag_dep_table_.push_back(dep);
    }
    for (RegPass& pass : compiler.reg_pass_) {
        pass.stub_first += stubs;
        reg_pass_.push_back(pass);
    }
    for (const RegStub& stub : compiler.reg_stubs_) reg_stubs_.push_back(stub);
    for (auto& info : compiler.carry_info_) carry_info_[info.first] = info.second;
    for (auto& edge : compiler.mt_edges_) mt_edges_.push_back(std::move(edge));
    compiler.mt_edges_.clear();
    compiler.staged_ = false;
    note_published(pc, compiler.staged_node_.mapped());
    const BlockEntry& entry = blocks_.insert_node(std::move(compiler.staged_node_));
    if (!compiler.staged_linked_) return;
    blocks_translated++;
    link_mt_block(pc, *compiler.staged_self_, entry);
}

void FrostJIT::discard_staged() {
    if (core_ == this) return;
    staged_ = staged_linked_ = false;
    staged_start_ = code_buf_used_;
    staged_entry_ = BlockEntry{};
    staged_node_ = {};
    mt_edges_.clear();
    code_words_.clear();
    flag_dep_table_.clear();
    reg_pass_.clear();
    reg_stubs_.clear();
    carry_info_.clear();
}

}  // namespace arm64emu
