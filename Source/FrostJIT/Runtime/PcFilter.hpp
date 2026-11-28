// PcFilter.hpp — answers "could this table hold the guest pc?" without the table's lock. A bit is set before its pc is inserted
// and never cleared by a removal, so "no" is exact for everything published before the question and "maybe" costs a locked lookup.
#pragma once
#include "System/Core/HostPagePolicy.hpp"
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace arm64emu {

// One frame holds the 1,024 instruction addresses of a guest page; pages share frames when there are more pages than frames.
// The bits are reserved address space: only the frames in use take memory.
template <size_t kFrameBits> class PcFilter {
    static constexpr size_t kFrameWords = 16, kBytes = (size_t{1} << kFrameBits) * kFrameWords * sizeof(uint64_t);
    uint64_t* words_ = nullptr;  // null: every answer is "maybe"
    uint64_t* word_of(uint64_t pc) const {
        const size_t frame = ((pc >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - kFrameBits);
        return words_ + frame * kFrameWords + ((pc >> 8) & (kFrameWords - 1));
    }
    static uint64_t bit_of(uint64_t pc) { return uint64_t{1} << ((pc >> 2) & 63); }
public:
    PcFilter() = default;
    PcFilter(const PcFilter&) = delete;
    PcFilter& operator=(const PcFilter&) = delete;
    ~PcFilter() { if (words_) ::munmap(words_, kBytes); }
    void reserve() {
        void* p = ::mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return;
        // The bits are sparse: one set bit must not commit a 2 MiB page.
        host_pages_no_huge(p, kBytes);
        words_ = static_cast<uint64_t*>(p);
    }
    bool may_contain(uint64_t pc) const {
        return !words_ || (__atomic_load_n(word_of(pc), __ATOMIC_ACQUIRE) & bit_of(pc));
    }
    // One writer at a time (the table's exclusive lock). True if the filter may have held pc before.
    bool add(uint64_t pc) {
        if (!words_) return true;
        uint64_t* const word = word_of(pc);
        const uint64_t old = __atomic_load_n(word, __ATOMIC_RELAXED), bit = bit_of(pc);
        if (old & bit) return true;
        __atomic_store_n(word, old | bit, __ATOMIC_RELEASE);
        return false;
    }
    // Gives the memory back; a late reader sees "no". The table must be empty for good.
    void release() { if (words_) ::madvise(words_, kBytes, MADV_DONTNEED); }
};

}  // namespace arm64emu
