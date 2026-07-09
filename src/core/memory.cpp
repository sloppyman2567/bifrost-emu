// core/memory.cpp — implementation of the Memory sparse-paged model.
//
// All public methods take the page-map mutex internally; per-page data is
// not locked (guest code is responsible for its own atomicity). The hot
// read/write paths short-circuit to the direct window (low 4 GiB) without
// touching the mutex at all.
#include "core/memory.h"

#include <algorithm>
#include <fcntl.h>
#include <shared_mutex>
#include <sys/mman.h>
#include <unistd.h>

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

    // v1.5.0.alpha: ASLR for mmap base. Randomize the starting address
    // for future mmap_alloc calls using /dev/urandom. The base is
    // page-aligned and within the high mmap region (0x5000000000 +
    // random offset up to 0x10000000000 = ~64 GiB of ASLR entropy).
    // This prevents guest-side info leaks that rely on a fixed mmap
    // base (common in sandbox escapes and ROP chain construction).
    //
    // We use /dev/urandom (not rand()) because:
    // 1. rand() is predictable if the guest can observe any output
    // 2. /dev/urandom is the standard kernel CSPRNG on Linux
    // 3. It's async-signal-safe (no malloc, no locks)
    //
    // BIFROST_NO_ASLR=1 disables randomization (for debugging and
    // reproducible trace comparison between JIT and interpreter).
    if (getenv("BIFROST_NO_ASLR")) {
        mmap_next_ = 0x5000000000ULL;
    } else {
        int fd = ::open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            uint64_t entropy = 0;
            ssize_t n = ::read(fd, &entropy, sizeof(entropy));
            ::close(fd);
            if (n == sizeof(entropy)) {
                // Mask to 36 bits (64 GiB range), page-align, add base.
                // Base = 0x5000000000 (above the 4 GiB direct window).
                entropy &= 0xFFFFFFFFFULL;
                mmap_next_ = 0x5000000000ULL + (entropy & ~PAGE_MASK);
            } else {
                // Fallback: use address of a stack variable as entropy.
                uint64_t stack_addr = reinterpret_cast<uint64_t>(&p);
                mmap_next_ = 0x5000000000ULL + ((stack_addr ^ 0x5DEECE66DULL)
                           & 0xFFFFFFFFFULL & ~PAGE_MASK);
            }
        } else {
            // /dev/urandom not available (chroot? container?). Use the
            // old fixed base — better than crashing.
            mmap_next_ = 0x5000000000ULL;
        }
    }
}

Memory::~Memory() {
    if (direct_window_) {
        munmap(direct_window_, DIRECT_WINDOW_SIZE);
    }
}

// v1.5.0.alpha: Validate that an address range is within the guest's
// usable address space. Rejects:
//   - Addresses below NULL_PAGE_LIMIT (NULL dereference protection)
//   - Addresses above 0x7FFFFFFFFFFF (kernel space on AArch64 Linux)
//   - Ranges that would wrap around (addr + size < addr)
bool Memory::is_valid_guest_range(uint64_t addr, uint64_t size) const {
    if (size == 0) return true;
    if (addr < NULL_PAGE_LIMIT) return false;
    // AArch64 Linux user space is 0..0x7FFFFFFFFFFF (48-bit VA).
    // The kernel uses 0xFFFF000000000000 and above.
    if (addr > 0x7FFFFFFFFFFFULL) return false;
    // Overflow check: addr + size must not wrap.
    if (addr + size < addr) return false;
    if (addr + size > 0x800000000000ULL) return false;
    return true;
}

bool Memory::would_exceed_page_limit(size_t num_pages) const {
    return total_pages_.load(std::memory_order_relaxed) + num_pages
           > MAX_TOTAL_PAGES;
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
    // BUGFIX: avoid integer overflow when addr + size wraps around.
    // If addr is near UINT64_MAX, addr + size can wrap to a small value,
    // which would falsely satisfy `<= DIRECT_WINDOW_SIZE`. Use a safe
    // range check instead: addr < DIRECT_WINDOW_SIZE AND size <= DIRECT_WINDOW_SIZE - addr.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        size <= DIRECT_WINDOW_SIZE - addr) {
        return true;
    }
    std::shared_lock<std::shared_mutex> g(mu_);
    uint64_t start = addr & ~PAGE_MASK;
    // BUGFIX: also guard the page-iteration end against overflow.
    // If addr + size wraps, the loop would terminate prematurely.
    uint64_t end = addr + size;
    if (end < addr) end = UINT64_MAX;  // saturate
    for (; start < end; start += PAGE_SIZE) {
        if (!pages_.count(start / PAGE_SIZE)) return false;
    }
    return true;
}

void Memory::write(uint64_t addr, const void* src, size_t n, PageCache* pc) {
    if (n == 0) return;
    // Fast path: direct window for addresses < 4 GiB.
    // BUGFIX: avoid integer overflow. `addr + n` can wrap to a small
    // value when addr is near UINT64_MAX, causing the check to pass
    // and the subsequent memcpy to write out-of-bounds at
    // `direct_window_ + addr` (a huge offset). Use a safe range check.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        n <= DIRECT_WINDOW_SIZE - addr) {
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
                    // v1.5.0.alpha: OOM protection for write path.
                    if (would_exceed_page_limit(1)) {
                        throw UnmappedMemory(cur, true);
                    }
                    it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                    total_pages_.fetch_add(1, std::memory_order_relaxed);
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
    // BUGFIX: same integer-overflow guard as write() — `addr + n` can
    // wrap when addr is near UINT64_MAX, causing the direct-window fast
    // path to fire for an out-of-bounds address.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        n <= DIRECT_WINDOW_SIZE - addr) {
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
            // v1.5.0.alpha: FEX-style demand paging. On real Linux, reads
            // to unmapped pages in the user address space trigger a page
            // fault, and the kernel zero-fills the page (for anonymous
            // mappings). This is critical for programs that read past the
            // end of heap/mmap allocations (common in regex engines,
            // string processing, vectorized loops). Previously we threw
            // UnmappedMemory, which caused SIGSEGV in busybox grep/sed.
            //
            // We still throw for addresses below PAGE_SIZE (the NULL page
            // region) — genuine NULL pointer dereferences should fault.
            if (cur < PAGE_SIZE) throw UnmappedMemory(cur, false);

            const std::vector<uint8_t>* page = nullptr;
            {
                std::unique_lock<std::shared_mutex> g(mu_);
                auto it = pages_.find(pn);
                if (it == pages_.end()) {
                    // v1.5.0.alpha: OOM protection for demand paging.
                    // If auto-allocation would exceed the page limit,
                    // throw UnmappedMemory (causing SIGSEGV delivery)
                    // instead of letting the host OOM.
                    if (would_exceed_page_limit(1)) {
                        throw UnmappedMemory(cur, false);
                    }
                    it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                    total_pages_.fetch_add(1, std::memory_order_relaxed);
                }
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

    // v1.5.0.alpha: Per-allocation size cap. Prevents a malicious guest
    // from requesting SIZE_MAX and OOMing the host.
    if (size > MAX_MMAP_LENGTH) return 0;  // caller maps 0 to -ENOMEM

    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t base = hint;
    uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (base == 0) {
        base = mmap_next_;
        mmap_next_ += aligned_size;
    } else {
        // v1.5.0.alpha: Validate MAP_FIXED address range. Reject
        // addresses in the NULL page region or kernel space.
        if (!is_valid_guest_range(base, aligned_size)) return 0;
        mmap_next_ = std::max(mmap_next_, base + aligned_size);
    }

    // v1.5.0.alpha: OOM protection. Check page count before allocating.
    size_t num_new_pages = aligned_size / PAGE_SIZE;
    if (would_exceed_page_limit(num_new_pages)) return 0;

    uint64_t start = base & ~PAGE_MASK;
    uint64_t end = base + aligned_size;  // page-aligned end
    size_t pages_added = 0;
    for (; start < end; start += PAGE_SIZE) {
        uint64_t pn = start / PAGE_SIZE;
        if (direct_window_ && start < DIRECT_WINDOW_SIZE) {
            continue;
        }
        auto it = pages_.find(pn);
        if (it == pages_.end()) {
            pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
            pages_added++;
        } else if (hint == 0) {
            std::fill(it->second.begin(), it->second.end(), 0);
        }
    }
    // Track total pages atomically (relaxed — no cross-thread sync needed).
    total_pages_.fetch_add(pages_added, std::memory_order_relaxed);
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
    // allocation's end. The collision check, the in-place extension,
    // and the allocations_/mmap_next_ updates must all happen under
    // the SAME unique lock — otherwise a concurrent mmap_alloc (which
    // also takes the unique lock) could slip in between the check and
    // the extension and grab the very pages we're about to grow into.
    //
    // The previous code used a shared_lock for the check, released it,
    // then re-acquired a unique_lock for the update — a classic TOCTOU
    // race. It also failed to bump mmap_next_ after an in-place grow,
    // so a subsequent mmap_alloc(NULL,...) could return an address
    // inside the just-grown region (because the bump pointer still
    // pointed at the pre-grow end). Combined with munmap, this caused
    // musl's mallocng to receive "fresh" mmap pages that were actually
    // dirty with leftover data, corrupting its meta_area headers and
    // crashing with BRK #1000 on the next free().
    uint64_t extra_start = (old_addr + old_aligned) & ~PAGE_MASK;
    uint64_t extra_end   = (old_addr + new_aligned) & ~PAGE_MASK;

    bool can_grow_in_place = true;
    std::unique_lock<std::shared_mutex> g(mu_);
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

    if (can_grow_in_place) {
        // Inline the map_range logic so we keep holding the unique lock
        // (map_range would otherwise re-acquire it → deadlock).
        for (uint64_t s = extra_start; s < extra_end; s += PAGE_SIZE) {
            uint64_t pn = s / PAGE_SIZE;
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) {
                continue;
            }
            auto it = pages_.find(pn);
            if (it == pages_.end()) {
                pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
            }
            // else: preserve existing page data — mremap_grow is
            // extending the mapping, not zeroing it. musl's realloc
            // expects the old data to be preserved at the start.
        }
        auto it = allocations_.find(old_addr);
        if (it != allocations_.end()) {
            it->second = new_aligned;
        } else {
            allocations_[old_addr] = new_aligned;
        }
        // BUGFIX: bump mmap_next_ past the grown region. Without this,
        // a subsequent mmap_alloc(NULL,...) would return an address
        // inside the grown (and possibly already-freed) region.
        mmap_next_ = std::max(mmap_next_, old_addr + new_aligned);
        return old_addr;
    }

    // Collision detected: allocate a fresh region, copy the data, and
    // return the new address. We must release the unique lock here
    // because mmap_alloc/read/write all acquire it themselves.
    g.unlock();

    uint64_t new_addr = mmap_alloc(new_size, 0);
    if (old_size > 0) {
        std::vector<uint8_t> buf(old_size);
        read(old_addr, buf.data(), old_size);
        write(new_addr, buf.data(), old_size);
    }
    {
        // BUGFIX: unique_lock, not shared_lock — we're mutating allocations_.
        std::unique_lock<std::shared_mutex> g2(mu_);
        allocations_.erase(old_addr);
    }
    return new_addr;
}

void Memory::untrack_allocation(uint64_t addr) {
    // BUGFIX: must hold a *unique* lock to mutate allocations_. The old
    // code used shared_lock, which is a data race (UB) if another thread
    // is concurrently reading allocations_ via mremap_grow().
    std::unique_lock<std::shared_mutex> g(mu_);
    allocations_.erase(addr);
}

bool Memory::atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired) {
    // BUGFIX: the old code only consulted the sparse pages_ map and never
    // the 4 GiB direct window. For any atomic address below 4 GiB (which
    // is where ALL user-space code, data, and futex words live in our
    // memory model), the CAS would silently create a *separate* pages_
    // entry that was independent of the direct-window storage — so
    // plain writes via Memory::write() went to the window, but CAS
    // read/wrote a stale duplicate. This broke LSE atomics (LDADD/CAS/
    // SWP) and futex for any address below 4 GiB.
    //
    // The fix: for addresses in the direct window, operate directly on
    // the window storage. We use a std::atomic<uint32_t> ref to get a
    // well-defined CAS without invoking UB by racing on a plain uint32_t.
    // The direct window is mmap'd MAP_PRIVATE|MAP_ANONYMOUS, so we own it
    // exclusively — no other process can touch it, and we serialize
    // cross-vCPU access via the shared_mutex below for the pages_ path.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        4 <= DIRECT_WINDOW_SIZE - addr) {
        // Page-aligned check: the entire 4-byte word must be within the
        // window (already checked above). Use an atomic CAS on the
        // underlying storage. This is safe because the window is private
        // to this process.
        std::atomic<uint32_t>* slot =
            reinterpret_cast<std::atomic<uint32_t>*>(direct_window_ + addr);
        return slot->compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel);
    }
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
    // See atomic_cas_32 for the direct-window rationale.
    // BUGFIX: same integer-overflow guard as atomic_cas_32.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        8 <= DIRECT_WINDOW_SIZE - addr) {
        std::atomic<uint64_t>* slot =
            reinterpret_cast<std::atomic<uint64_t>*>(direct_window_ + addr);
        return slot->compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel);
    }
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

// ── Fork support ───────────────────────────────────────────────────────
std::vector<Memory::PageSnapshot> Memory::snapshot_pages() const {
    std::shared_lock<std::shared_mutex> g(mu_);
    std::vector<PageSnapshot> out;

    // 1. Pages from the sparse pages_ map (addresses >= 4 GiB).
    for (const auto& [page_num, data] : pages_) {
        out.push_back({page_num * PAGE_SIZE, data});
    }

    // 2. Pages from the direct window (addresses < 4 GiB).
    //
    // BUGFIX: the previous implementation only copied pages that had at
    // least one non-zero byte. This silently dropped all-zero mapped
    // pages (e.g., BSS, freshly-mmap'd pages that haven't been written
    // yet). The previous comment incorrectly claimed "a read from an
    // unmapped page returns 0 in our model" — actually, Memory::read()
    // throws UnmappedMemory for unmapped pages, which the JIT slow path
    // converts to SIGSEGV. So a forked child that inherited an all-zero
    // mapped page would crash with SIGSEGV on first access.
    //
    // The fix: iterate allocations_ (which tracks every mmap'd region)
    // and copy ALL pages within those regions — including all-zero ones.
    // We also do a full scan of the direct window to catch pages that
    // were mapped implicitly (e.g., via map_range for the brk region,
    // which doesn't go through mmap_alloc and therefore isn't in
    // allocations_). Pages outside any tracked region that are all-zero
    // are still skipped (they're genuinely unmapped).
    if (direct_window_) {
        // 2a. Copy every page within a tracked allocation.
        //     This catches all-zero pages (BSS, fresh mmaps).
        std::vector<bool> copied(DIRECT_WINDOW_SIZE / PAGE_SIZE, false);
        for (const auto& [base, size] : allocations_) {
            if (base >= DIRECT_WINDOW_SIZE) continue;
            uint64_t end = base + size;
            if (end < base || end > DIRECT_WINDOW_SIZE) end = DIRECT_WINDOW_SIZE;
            for (uint64_t a = base & ~PAGE_MASK; a < end; a += PAGE_SIZE) {
                uint64_t pn = a / PAGE_SIZE;
                if (pn < copied.size() && !copied[pn]) {
                    const uint8_t* page = direct_window_ + a;
                    out.push_back({a, std::vector<uint8_t>(page, page + PAGE_SIZE)});
                    copied[pn] = true;
                }
            }
        }
        // 2b. Scan the direct window for any other non-zero pages
        //     (e.g., brk pages, ELF-loaded pages — these may not be
        //     tracked in allocations_). Pages that are all-zero AND
        //     not in any allocation are skipped (genuinely unmapped).
        const uint64_t num_pages = DIRECT_WINDOW_SIZE / PAGE_SIZE;
        for (uint64_t p = 0; p < num_pages; p++) {
            if (copied[p]) continue;
            const uint8_t* page = direct_window_ + p * PAGE_SIZE;
            // Quick check: if the first 64 bytes are all zero, skip
            // (most pages are zero). This is a heuristic — we might
            // miss a page that has non-zero data only after offset 64,
            // but that's extremely rare for typical guests.
            bool has_data = false;
            for (int i = 0; i < 64; i++) {
                if (page[i] != 0) { has_data = true; break; }
            }
            if (!has_data) {
                // Double-check the rest of the page (for correctness).
                for (size_t i = 64; i < PAGE_SIZE; i++) {
                    if (page[i] != 0) { has_data = true; break; }
                }
            }
            if (has_data) {
                out.push_back({p * PAGE_SIZE,
                               std::vector<uint8_t>(page, page + PAGE_SIZE)});
            }
        }
    }

    return out;
}

std::unique_ptr<Memory> Memory::clone_for_fork() const {
    auto child = std::make_unique<Memory>();
    // Copy the direct window base (child gets its own mmap'd window
    // from its constructor). Then copy all mapped pages.
    auto snapshots = snapshot_pages();
    for (const auto& snap : snapshots) {
        if (child->in_direct_window(snap.addr)) {
            // Write to the direct window.
            child->write(snap.addr, snap.data.data(), snap.data.size());
        } else {
            // Write to the sparse pages_ map.
            child->map_range(snap.addr, snap.data.size());
            child->write(snap.addr, snap.data.data(), snap.data.size());
        }
    }
    // BUGFIX: also copy mmap_next_ and allocations_ so the child's
    // future mmaps don't collide with pages already copied from the
    // parent. Without this, a child that calls mmap(NULL, ...) after
    // fork could get an address that overlaps with the parent's
    // (now-copied) data — silently corrupting the child's heap.
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        child->mmap_next_ = mmap_next_;
        for (const auto& [base, size] : allocations_) {
            child->allocations_[base] = size;
        }
        // v1.5.0.alpha: copy total page count for OOM tracking.
        child->total_pages_.store(total_pages_.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
    return child;
}

} // namespace arm64emu
