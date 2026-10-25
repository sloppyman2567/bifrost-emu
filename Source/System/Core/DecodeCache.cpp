// DecodeCache.cpp — the per-vCPU interpreter decode cache's storage (see CPU::DecodeCache).
#include "System/Core/CPU.hpp"
#include <new>
#include <sys/mman.h>

namespace arm64emu {
namespace {
constexpr size_t kEntryBytes = CPU::DECODE_CACHE_SIZE * sizeof(CPU::CacheEntry);
constexpr size_t kLruBytes = (CPU::DECODE_CACHE_SETS + 7) / 8;
constexpr size_t kBytes = (kEntryBytes + kLruBytes + 4095) & ~size_t{4095};
}

void CPU::DecodeCache::allocate() {
    // Zero-filled by the kernel and faulted in per page: an empty slot is a zero tag.
    void* p = ::mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    // A 1.7 MiB mapping next to others could be merged into a huge page, which would make a
    // handful of touched entries cost 2 MiB again.
    ::madvise(p, kBytes, MADV_NOHUGEPAGE);
    entries_ = static_cast<CacheEntry*>(p);
    lru_ = reinterpret_cast<uint8_t*>(entries_ + DECODE_CACHE_SIZE);
}

void CPU::DecodeCache::clear() {
    if (!entries_) return;
    // Anonymous private pages read as zero after MADV_DONTNEED: every entry is empty again.
    ::madvise(entries_, kBytes, MADV_DONTNEED);
}

CPU::DecodeCache::~DecodeCache() {
    if (entries_) ::munmap(entries_, kBytes);
}

} // namespace arm64emu
