// MemoryStats.cpp — host heap held by FrostJIT's per-block bookkeeping.
//
// Printed with the code-cache stats (BIFROST_STATS_PERIOD, --verbose exit).
// Sizes come from container sizes and capacities with glibc's chunk rounding
// and libstdc++'s node layouts, so they track what malloc holds for each
// container without walking the heap.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/PersistentCache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
namespace arm64emu {
namespace {
// glibc x86-64: an 8-byte chunk header, 16-byte alignment, 32-byte minimum.
size_t chunk(size_t bytes) { return std::max<size_t>(32, (bytes + 8 + 15) & ~size_t{15}); }
template <class Vector> size_t vector_bytes(const Vector& v) {
    return v.capacity() ? chunk(v.capacity() * sizeof(typename Vector::value_type)) : 0;
}
// A libstdc++ hash node is the next pointer plus the value (integer keys do
// not cache their hash); the bucket array is one pointer per bucket.
template <class Map> size_t hash_bytes(const Map& map) {
    return map.bucket_count() * sizeof(void*) +
           map.size() * chunk(sizeof(void*) + sizeof(typename Map::value_type));
}
// Hash or tree maps whose values are vectors also own each vector's buffer.
template <class Map> size_t vector_values_bytes(const Map& map) {
    size_t bytes = 0;
    for (const auto& kv : map) bytes += vector_bytes(kv.second);
    return bytes;
}
// A red-black tree node is a 32-byte header plus the value.
template <class Map> size_t tree_bytes(const Map& map) {
    return map.size() * chunk(32 + sizeof(typename Map::value_type));
}
double mib(size_t bytes) { return double(bytes) / (1024.0 * 1024.0); }
} // namespace

void FrostJIT::dump_memory_stats() {
    // Compilers take compile_mutex_ before blocks_mutex_; MT compiles append
    // edges under the shared table lock, so both are needed for a stable read.
    // At exit a stopped guest thread may still own the compiler: skip the line
    // rather than wait.
    std::unique_lock<std::mutex> compiling(compile_mutex_, std::defer_lock);
    for (int attempt = 0; attempt < 100 && !compiling.try_lock(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!compiling.owns_lock()) {
        fprintf(stderr, "[JIT] metadata: skipped, compiler busy\n");
        return;
    }
    std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
    size_t entry_vectors = 0;
    for (const auto& kv : blocks_) {
        entry_vectors += vector_bytes(kv.second.inlined_ranges_);
        if (kv.second.store_infos) entry_vectors += vector_bytes(*kv.second.store_infos);
    }
    const size_t blocks = hash_bytes(blocks_) + entry_vectors;
    const size_t index = tree_bytes(code_index_) + vector_values_bytes(code_index_);
    const size_t back_refs = hash_bytes(back_refs_) + vector_values_bytes(back_refs_);
    const size_t calls = hash_bytes(pending_call_sites_) + vector_values_bytes(pending_call_sites_) +
                         hash_bytes(direct_call_sites_) + vector_values_bytes(direct_call_sites_);
    const size_t edges = vector_bytes(mt_edges_) + mt_edges_.size() * chunk(sizeof(MtEdge));
    const size_t edge_maps = hash_bytes(mt_edge_refs_) + vector_values_bytes(mt_edge_refs_) +
                             hash_bytes(mt_source_refs_) + vector_values_bytes(mt_source_refs_);
    const size_t tier2 = vector_bytes(mt_hot_counters_) + mt_hot_counters_.size() * chunk(sizeof(MtHotCounter)) +
                         hash_bytes(mt_counter_sites_) + vector_bytes(tier2_branch_profiles_) +
                         tier2_branch_profiles_.size() * chunk(sizeof(Tier2BranchProfile)) +
                         hash_bytes(tier2_profiles_by_pc_);
    const size_t words = vector_bytes(code_words_) + vector_bytes(flag_dep_table_);
    const size_t retired = hash_bytes(retired_pcs_);
    const size_t total = blocks + index + back_refs + calls + edges + edge_maps + tier2 + words + retired;
    fprintf(stderr, "[JIT] metadata MiB: total=%.1f per_block=%zuB blocks=%.1f(%zu) code_index=%.1f(%zu granules) "
                    "back_refs=%.1f(%zu) calls=%.1f mt_edges=%.1f(%zu) mt_edge_maps=%.1f tier2=%.1f "
                    "code_words=%.1f retired=%.1f(%zu) thread_inline_caches=%.1f\n",
            mib(total), blocks_.empty() ? size_t{0} : total / blocks_.size(), mib(blocks), blocks_.size(),
            mib(index), code_index_.size(), mib(back_refs), back_refs_.size(), mib(calls), mib(edges),
            mt_edges_.size(), mib(edge_maps), mib(tier2), mib(words), mib(retired), retired_pcs_.size(),
            mib(inline_cache_bytes_.load(std::memory_order_relaxed)));
    if (disk_cache_) disk_cache_->dump_stats();
}
} // namespace arm64emu
