// CodeIndex.hpp — which translations were made from which guest code: the candidates of a range invalidation.
#pragma once
#include "FrostJIT/Runtime/GrowingTables.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace arm64emu {

// Guest pages, each with the block pcs listed on each of its 256-byte granules. A lookup by page is one hash probe, since every
// publication makes one; the pages are also kept in order, so that a huge range visits only the pages that hold code. Lists are
// conservative: they may hold retired or non-overlapping pcs, and a visit prunes what the caller does not keep. A granule
// smaller than a page keeps a cache-line publish from scanning every block on its page.
class CodeIndex {
    static constexpr uint64_t kPage = 4096;
public:
    static constexpr uint64_t kGranule = 256;
private:
    struct Page {
        std::vector<uint64_t> pcs[kPage / kGranule];
        bool listed = false;  // in order_
    };
    Sharded<std::unordered_map<uint64_t, Page>> pages_;
    std::set<uint64_t> order_;
public:
    static constexpr uint64_t granule_of(uint64_t address) { return address & ~(kGranule - 1); }
    void add(uint64_t pc, uint64_t start, uint64_t last_byte) {
        Page* page = nullptr;
        uint64_t page_start = 0;
        const uint64_t last = granule_of(last_byte);
        for (uint64_t granule = granule_of(start);; granule += kGranule) {
            if (!page || (granule & ~(kPage - 1)) != page_start) {
                page_start = granule & ~(kPage - 1);
                page = &pages_[page_start];
                if (!page->listed) { order_.insert(page_start); page->listed = true; }
            }
            auto& pcs = page->pcs[(granule - page_start) / kGranule];
            if (pcs.empty()) pcs.reserve(4);
            if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end()) pcs.push_back(pc);
            if (granule == last) break;
        }
    }
    // Is anything listed on the granules of [lo, hi)? Both are granule addresses.
    bool any(uint64_t lo, uint64_t hi) const {
        for (auto it = order_.lower_bound(lo & ~(kPage - 1)); it != order_.end() && *it < hi; ++it) {
            const Page& page = pages_.find(*it)->second;
            for (uint64_t g = std::max(lo, *it); g < hi && g - *it < kPage; g += kGranule)
                if (!page.pcs[(g - *it) / kGranule].empty()) return true;
        }
        return false;
    }
    // Calls keep(pc) for every pc listed on the granules of [lo, hi) and forgets those it answers false for.
    template <class Keep> void visit(uint64_t lo, uint64_t hi, Keep&& keep) {
        for (auto it = order_.lower_bound(lo & ~(kPage - 1)); it != order_.end() && *it < hi;) {
            const auto found = pages_.find(*it);
            Page& page = found->second;
            for (uint64_t g = std::max(lo, *it); g < hi && g - *it < kPage; g += kGranule) {
                auto& pcs = page.pcs[(g - *it) / kGranule];
                pcs.erase(std::remove_if(pcs.begin(), pcs.end(), [&](uint64_t pc) { return !keep(pc); }), pcs.end());
            }
            const bool empty = std::all_of(std::begin(page.pcs), std::end(page.pcs), [](const auto& pcs) { return pcs.empty(); });
            if (empty) { pages_.erase(found); it = order_.erase(it); }
            else ++it;
        }
    }
    void clear() { pages_.clear(); order_.clear(); }
    void swap(CodeIndex& other) { pages_.swap(other.pages_); order_.swap(other.order_); }
    size_t pages() const { return order_.size(); }
    const auto& page_table() const { return pages_; }     // for the memory report
    template <class Fn> void each_list(Fn&& fn) const { for (const auto& kv : pages_) for (const auto& pcs : kv.second.pcs) fn(pcs); }
};

}  // namespace arm64emu
