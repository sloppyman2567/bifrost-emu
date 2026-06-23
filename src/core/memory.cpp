// core/memory.cpp — implementation of the Memory sparse-paged model.
//
// All public methods take the page-map mutex internally; per-page data is
// not locked (guest code is responsible for its own atomicity). The hot
// read/write paths short-circuit to the direct window (low 4 GiB) without
// touching the mutex at all.
#include "core/memory.h"

#include <algorithm>
#include <shared_mutex>
#include <sys/mman.h>

namespace arm64emu {

Memory::Memory() {
    // Allocate a 4 GiB direct-access window for the JIT. This is a lazy
    // mmap — Linux only allocates physical pages on first access (demand
    // paging). The window is PROT_READ|PROT_WRITE.
    void* p = mmap(nullptr, DIRECT_WINDOW_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED) {
        direct_window_ = static_cast<uint8_t*>(p);
    }
}

Memory::~Memory() {
    if (direct_window_) {
        munmap(direct_window_, DIRECT_WINDOW_SIZE);
    }
}

void Memory::map_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t start = addr & ~PAGE_MASK;
    uint64_t end = addr + size;
    for (; start < end; start += PAGE_SIZE) {
        uint64_t pn = start / PAGE_SIZE;
        // For addresses in the direct window (< 4 GiB), the window IS
        // the storage — no need to create a pages_ entry.
        if (direct_window_ && start < DIRECT_WINDOW_SIZE) {
            continue;
        }
        auto it = pages_.find(pn);
        if (it == pages_.end()) {
            pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
        }
    }
}

bool Memory::is_mapped(uint64_t addr, uint64_t size) const {
    if (size == 0) return true;
    if (direct_window_ && addr + size <= DIRECT_WINDOW_SIZE) {
        return true;
    }
    std::shared_lock<std::shared_mutex> g(mu_);
    uint64_t start = addr & ~PAGE_MASK;
    uint64_t end = addr + size;
    for (; start < end; start += PAGE_SIZE) {
        if (!pages_.count(start / PAGE_SIZE)) return false;
    }
    return true;
}

void Memory::write(uint64_t addr, const void* src, size_t n, PageCache* pc) {
    if (n == 0) return;
    // Fast path: direct window for addresses < 4 GiB.
    if (direct_window_ && addr + n <= DIRECT_WINDOW_SIZE) {
        memcpy(direct_window_ + addr, src, n);
        return;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    uint64_t cur = addr;
    size_t remaining = n;
    while (remaining > 0) {
        uint64_t pn = cur / PAGE_SIZE;
        uint64_t off = cur & PAGE_MASK;
        size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
        if (pc && __builtin_expect(pn == pc->write_page, 1)) {
            memcpy(pc->write_ptr + off, p, take);
        } else {
            std::vector<uint8_t>* page = nullptr;
            {
                std::unique_lock<std::shared_mutex> g(mu_);
                auto it = pages_.find(pn);
                if (it == pages_.end()) {
                    it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                }
                page = &it->second;
            }
            memcpy(page->data() + off, p, take);
            if (pc) {
                pc->write_page = pn;
                pc->write_ptr = page->data();
            }
        }
        p += take;
        cur += take;
        remaining -= take;
    }
}

void Memory::read(uint64_t addr, void* dst, size_t n, PageCache* pc) const {
    if (n == 0) return;
    if (direct_window_ && addr + n <= DIRECT_WINDOW_SIZE) {
        memcpy(dst, direct_window_ + addr, n);
        return;
    }
    uint8_t* p = static_cast<uint8_t*>(dst);
    uint64_t cur = addr;
    size_t remaining = n;
    while (remaining > 0) {
        uint64_t pn = cur / PAGE_SIZE;
        uint64_t off = cur & PAGE_MASK;
        size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
        if (pc && __builtin_expect(pn == pc->read_page, 1)) {
            memcpy(p, pc->read_ptr + off, take);
        } else {
            const std::vector<uint8_t>* page = nullptr;
            {
                std::shared_lock<std::shared_mutex> g(mu_);
                auto it = pages_.find(pn);
                if (it == pages_.end()) throw UnmappedMemory(cur, false);
                page = &it->second;
            }
            memcpy(p, page->data() + off, take);
            if (pc) {
                pc->read_page = pn;
                pc->read_ptr = page->data();
            }
        }
        p += take;
        cur += take;
        remaining -= take;
    }
}

uint64_t Memory::mmap_alloc(uint64_t size, uint64_t hint) {
    if (size == 0) size = PAGE_SIZE;
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t base = hint;
    uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (base == 0) {
        base = mmap_next_;
        mmap_next_ += aligned_size;
    } else {
        mmap_next_ = std::max(mmap_next_, base + aligned_size);
    }
    uint64_t start = base & ~PAGE_MASK;
    uint64_t end = base + size;
    for (; start < end; start += PAGE_SIZE) {
        auto it = pages_.find(start / PAGE_SIZE);
        if (it == pages_.end()) {
            pages_.emplace(start / PAGE_SIZE,
                           std::vector<uint8_t>(PAGE_SIZE, 0));
        }
        // Preserve existing pages on MAP_FIXED (don't zero). Needed
        // because musl's mallocng uses MAP_FIXED for guard pages, and
        // zeroing would corrupt metadata.
    }
    allocations_[base] = aligned_size;
    return base;
}

uint64_t Memory::mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size) {
    if (new_size == 0) new_size = PAGE_SIZE;
    uint64_t old_aligned = (old_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t new_aligned = (new_size + PAGE_MASK) & ~PAGE_MASK;

    // Shrink: keep the same address, just record the smaller size.
    // We don't actually unmap the freed tail pages, but that's fine —
    // the guest won't access them, and we never reclaim address space
    // anyway (bump allocator).
    if (new_aligned <= old_aligned) {
        std::unique_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(old_addr);
        if (it != allocations_.end()) {
            it->second = new_aligned;
        } else {
            allocations_[old_addr] = new_aligned;
        }
        return old_addr;
    }

    // Grow: check for collision with the pages just past the current
    // allocation's end. Done under the same lock that protects pages_
    // and allocations_ so a concurrent mmap can't slip in.
    uint64_t extra_start = (old_addr + old_aligned) & ~PAGE_MASK;
    uint64_t extra_end   = (old_addr + new_aligned) & ~PAGE_MASK;

    bool can_grow_in_place = true;
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        for (const auto& kv : allocations_) {
            uint64_t other_base = kv.first;
            uint64_t other_size = kv.second;
            if (other_base == old_addr) continue;
            uint64_t other_end = other_base + other_size;
            if (extra_start < other_end && other_base < extra_end) {
                can_grow_in_place = false;
                break;
            }
        }
    }

    if (can_grow_in_place) {
        map_range(extra_start, extra_end - extra_start);
        std::shared_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(old_addr);
        if (it != allocations_.end()) {
            it->second = new_aligned;
        } else {
            allocations_[old_addr] = new_aligned;
        }
        return old_addr;
    }

    // Collision detected: allocate a fresh region, copy the data, and
    // return the new address. Old pages stay mapped (munmap is a no-op
    // in our sparse model) — safe because musl's metadata will be
    // updated to point at the new address.
    uint64_t new_addr = mmap_alloc(new_size, 0);
    if (old_size > 0) {
        std::vector<uint8_t> buf(old_size);
        read(old_addr, buf.data(), old_size);
        write(new_addr, buf.data(), old_size);
    }
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        allocations_.erase(old_addr);
    }
    return new_addr;
}

void Memory::untrack_allocation(uint64_t addr) {
    std::shared_lock<std::shared_mutex> g(mu_);
    allocations_.erase(addr);
}

bool Memory::atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired) {
    std::unique_lock<std::shared_mutex> g(mu_);
    auto it = pages_.find(addr / PAGE_SIZE);
    if (it == pages_.end()) {
        if (expected != 0) return false;
        it = pages_.emplace(addr / PAGE_SIZE,
                            std::vector<uint8_t>(PAGE_SIZE, 0)).first;
    }
    uint64_t off = addr & PAGE_MASK;
    uint32_t cur;
    memcpy(&cur, it->second.data() + off, 4);
    if (cur == expected) {
        memcpy(it->second.data() + off, &desired, 4);
        return true;
    }
    return false;
}

bool Memory::atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired) {
    std::unique_lock<std::shared_mutex> g(mu_);
    auto it = pages_.find(addr / PAGE_SIZE);
    if (it == pages_.end()) {
        if (expected != 0) return false;
        it = pages_.emplace(addr / PAGE_SIZE,
                            std::vector<uint8_t>(PAGE_SIZE, 0)).first;
    }
    uint64_t off = addr & PAGE_MASK;
    uint64_t cur;
    memcpy(&cur, it->second.data() + off, 8);
    if (cur == expected) {
        memcpy(it->second.data() + off, &desired, 8);
        return true;
    }
    return false;
}

size_t Memory::page_count() const {
    std::shared_lock<std::shared_mutex> g(mu_);
    return pages_.size();
}

} // namespace arm64emu
