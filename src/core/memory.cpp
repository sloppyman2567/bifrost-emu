// core/memory.cpp — implementation of the Memory sparse-paged model.
//
// All public methods take the page-map mutex internally; per-page data is
// not locked (guest code is responsible for its own atomicity). The hot
// read/write paths short-circuit to the direct window (low 4 GiB) without
// touching the mutex at all.
#include "core/memory.h"
#include "debug_flags.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <shared_mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
namespace arm64emu {
static_assert(sizeof(std::atomic<uint8_t>) == sizeof(uint8_t),
              "JIT byte-loads the atomic guest page flags directly");

// BIFROST_WATCH=addr[:size] (hex): log every Memory::write overlapping the
// range with the current interp pc (heap wild-write hunt; use --no-jit so
// all guest stores flow through write()). Parsed once; a single cached
// branch when unset.
namespace {
thread_local uint64_t t_watch_pc = 0;
struct WatchRange {
    bool on = false;
    uint64_t lo = 0, hi = 0;
};
const WatchRange& watch_range() {
    static const WatchRange w = [] {
        WatchRange r;
        const char* s = getenv("BIFROST_WATCH");
        if (!s || !*s) return r;
        char* end = nullptr;
        uint64_t addr = strtoull(s, &end, 0);
        uint64_t size = 64;
        if (end && *end == ':') size = strtoull(end + 1, nullptr, 0);
        if (size == 0) size = 64;
        r.on = true;
        r.lo = addr;
        r.hi = addr + size;
        return r;
    }();
    return w;
}
// BIFROST_WRITE_TRACE_RANGE=lo:hi (hex): restrict the store log to one
// region. Parsed once; `on == false` means log every write.
const WatchRange& trace_range() {
    static const WatchRange w = [] {
        WatchRange r;
        const std::string& s = dbg().write_trace_range;
        if (s.empty()) return r;
        char* end = nullptr;
        uint64_t lo = strtoull(s.c_str(), &end, 0);
        uint64_t hi = ~0ull;
        if (end && *end == ':') hi = strtoull(end + 1, nullptr, 0);
        r.on = true;
        r.lo = lo;
        r.hi = hi;
        return r;
    }();
    return w;
}
}  // namespace
void Memory::note_interp_pc(uint64_t pc) { t_watch_pc = pc; }

// BIFROST_MEMSTATS=N (vkQuake AllocBlock leak hunt): background reporter
// printing live guest allocation count/bytes + host RSS every N seconds,
// plus the top live allocations by size. Flat curve = no allocator-side
// leak; linear growth names the leaking pool via its addresses.
static std::atomic<uint64_t> g_memstats_cum_alloc{0}, g_memstats_cum_free{0};
void Memory::memstats_reporter(uint64_t period_secs) {
    uint64_t t = 0;
    while (true) {
        std::this_thread::sleep_for(
            std::chrono::seconds(static_cast<int64_t>(period_secs)));
        t += period_secs;
        {
            std::shared_lock<std::shared_mutex> g(mu_);
            uint64_t live_bytes = 0;
            // Top-5 largest live allocations (address, size MB).
            std::pair<uint64_t, uint64_t> top[5] = {};
            for (const auto& kv : allocations_) {
                live_bytes += kv.second;
                for (int i = 0; i < 5; i++) {
                    if (kv.second > top[i].second) {
                        for (int j = 4; j > i; j--) top[j] = top[j - 1];
                        top[i] = kv;
                        break;
                    }
                }
            }
            long rss_pages = 0;
            if (FILE* f = fopen("/proc/self/statm", "r")) {
                unsigned long tot = 0, res = 0;
                if (fscanf(f, "%lu %lu", &tot, &res) == 2)
                    rss_pages = static_cast<long>(res);
                fclose(f);
            }
            fprintf(stderr,
                    "[memstats] t=%llus live=%zu allocs %.1f MB "
                    "cum_alloc=%.1f MB cum_free=%.1f MB rss=%ld MB | top:",
                    (unsigned long long)t, allocations_.size(),
                    static_cast<double>(live_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(
                        g_memstats_cum_alloc.load(std::memory_order_relaxed)) /
                        (1024.0 * 1024.0),
                    static_cast<double>(
                        g_memstats_cum_free.load(std::memory_order_relaxed)) /
                        (1024.0 * 1024.0),
                    rss_pages * 4 / 1024);
            for (int i = 0; i < 5; i++) {
                if (!top[i].second) break;
                fprintf(stderr, " [0x%llx +%.2fMB]",
                        (unsigned long long)top[i].first,
                        static_cast<double>(top[i].second) / (1024.0 * 1024.0));
            }
            fprintf(stderr, "\n");
        }
    }
}

Memory::Memory(bool start_reporter) {
    constexpr size_t DIRECT_PAGES = DIRECT_WINDOW_SIZE / PAGE_SIZE;
    direct_page_flags_ = std::make_unique<std::atomic<uint8_t>[]>(DIRECT_PAGES);
    for (size_t i = 0; i < DIRECT_PAGES; ++i)
        direct_page_flags_[i].store(0, std::memory_order_relaxed);
    // Allocate a 4 GiB direct-access window for the JIT. This is a lazy
    // mmap — Linux only allocates physical pages on first access (demand
    // paging). Unmapped guest pages start inaccessible to JIT fast paths.
    void* p = mmap(nullptr, DIRECT_WINDOW_SIZE,
                   PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED) {
        direct_window_ = static_cast<uint8_t*>(p);
    }
    // 1.5.4-alpha: ASLR for mmap base. Randomize the starting address
    // for future mmap_alloc calls using /dev/urandom. The base is
    // page-aligned and within the low heap region (MMAP_BASE_MIN +
    // random offset up to ~512 MiB of ASLR entropy). It's INSIDE the
    // 4 GiB direct window (1.5.3) so guest heap accesses hit the JIT
    // fast path instead of the pages_ + rwlock slow path.
    // This prevents guest-side info leaks that rely on a fixed mmap
    // base (common in sandbox escapes and ROP chain construction).
    //
    // BIFROST_NO_ASLR=1 disables ALL randomization (heap base, PIE load
    // bias, stack top) — for debugging and reproducible trace
    // comparison between JIT and interpreter.
    mmap_next_ = MMAP_BASE_MIN + random_offset(MMAP_BASE_MAX - MMAP_BASE_MIN);
    pie_base_ = PIE_BASE_MIN + random_offset(PIE_JITTER);
    stack_top_ = STACK_TOP - random_offset(STACK_JITTER);
    if (getenv("BIFROST_MEMGUARD") && direct_window_) {
        fprintf(stderr, "[memguard] window=%p stack_top=0x%llx\n",
                static_cast<void*>(direct_window_),
                (unsigned long long)stack_top_);
    }
    if (start_reporter) {
        if (const char* ms = getenv("BIFROST_MEMSTATS")) {
        uint64_t period = strtoull(ms, nullptr, 10);
        if (period == 0) period = 5;
        std::thread(&Memory::memstats_reporter, this, period).detach();
        }
    }
}

bool Memory::aslr_disabled() {
    static const bool disabled = getenv("BIFROST_NO_ASLR") != nullptr;
    return disabled;
}

uint64_t Memory::random_offset(uint64_t range) {
    if (aslr_disabled() || range == 0) return 0;
    uint64_t npages = range / PAGE_SIZE;
    if (npages == 0) return 0;
    int fd = ::open("/dev/urandom", O_RDONLY);
    uint64_t entropy = 0;
    if (fd >= 0) {
        ssize_t n = ::read(fd, &entropy, sizeof(entropy));
        ::close(fd);
        if (n != sizeof(entropy)) entropy = 0;
    }
    if (entropy == 0) {
        // Fallback: use the address of a stack variable as entropy
        // (urandom unavailable — chroot/container). Better than a
        // fully fixed layout; still page-aligned.
        uint64_t stack_addr = reinterpret_cast<uint64_t>(&fd);
        entropy = stack_addr ^ 0x5DEECE66DULL;
    }
    return (entropy % npages) * PAGE_SIZE;
}
Memory::~Memory() {
    if (direct_window_) {
        munmap(direct_window_, DIRECT_WINDOW_SIZE);
    }
}
// 1.5.4-alpha: Validate that an address range is within the guest's
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
bool Memory::set_direct_prot(uint64_t lo, uint64_t hi, int prot) const {
    if (!direct_window_ || lo >= hi || lo >= DIRECT_WINDOW_SIZE) return true;
    hi = std::min<uint64_t>(hi, DIRECT_WINDOW_SIZE);
    return ::mprotect(direct_window_ + lo, hi - lo, prot) == 0;
}
void Memory::split_high_mapping_locked(uint64_t at) {
    auto it = high_mappings_.upper_bound(at);
    if (it == high_mappings_.begin()) return;
    --it;
    if (it->first >= at || it->second.end <= at) return;
    const HighMapping right{it->second.end, it->second.prot};
    it->second.end = at;
    high_mappings_.emplace(at, right);
}
void Memory::erase_high_mapping_locked(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    split_high_mapping_locked(lo);
    split_high_mapping_locked(hi);
    auto it = high_mappings_.lower_bound(lo);
    while (it != high_mappings_.end() && it->first < hi)
        it = high_mappings_.erase(it);
}
void Memory::set_high_mapping_locked(uint64_t lo, uint64_t hi, uint8_t prot,
                                     bool combine) {
    if (lo >= hi) return;
    split_high_mapping_locked(lo);
    split_high_mapping_locked(hi);
    if (!combine) {
        erase_high_mapping_locked(lo, hi);
        high_mappings_[lo] = HighMapping{hi, prot};
    } else {
        uint64_t cur = lo;
        auto it = high_mappings_.lower_bound(lo);
        while (cur < hi) {
            if (it == high_mappings_.end() || it->first > cur) {
                const uint64_t gap_end = (it == high_mappings_.end())
                    ? hi : std::min<uint64_t>(hi, it->first);
                it = high_mappings_.emplace_hint(it, cur,
                    HighMapping{gap_end, prot});
                cur = gap_end;
                ++it;
            } else {
                it->second.prot |= prot;
                cur = std::min<uint64_t>(hi, it->second.end);
                ++it;
            }
        }
    }
    // Coalesce adjacent ranges with equal permissions.
    auto it = high_mappings_.lower_bound(lo);
    if (it != high_mappings_.begin()) --it;
    while (it != high_mappings_.end()) {
        auto next = std::next(it);
        if (next == high_mappings_.end() || next->first > hi) break;
        if (it->second.end == next->first &&
            it->second.prot == next->second.prot) {
            it->second.end = next->second.end;
            high_mappings_.erase(next);
        } else {
            it = next;
        }
    }
}
bool Memory::high_range_mapped_locked(uint64_t lo, uint64_t hi,
                                      uint8_t access) const {
    uint64_t cur = lo;
    auto it = high_mappings_.upper_bound(cur);
    if (it != high_mappings_.begin()) --it;
    while (cur < hi) {
        while (it != high_mappings_.end() && it->second.end <= cur) ++it;
        if (it == high_mappings_.end() || it->first > cur) return false;
        if (access && (it->second.prot & access) != access) return false;
        cur = std::min<uint64_t>(hi, it->second.end);
        ++it;
    }
    return true;
}
bool Memory::range_mapped_locked(uint64_t lo, uint64_t hi) const {
    if (lo >= hi) return true;
    if (direct_window_ && lo < DIRECT_WINDOW_SIZE) {
        uint64_t low_hi = std::min<uint64_t>(hi, DIRECT_WINDOW_SIZE);
        for (uint64_t p = lo / PAGE_SIZE; p < (low_hi + PAGE_MASK) / PAGE_SIZE; ++p)
            if (!(direct_page_flags_[p].load(std::memory_order_relaxed) & PAGE_MAPPED))
                return false;
        lo = low_hi;
    }
    return lo >= hi || high_range_mapped_locked(lo, hi, 0);
}
bool Memory::range_overlaps_locked(uint64_t lo, uint64_t hi) const {
    if (lo >= hi) return false;
    if (direct_window_ && lo < DIRECT_WINDOW_SIZE) {
        uint64_t low_hi = std::min<uint64_t>(hi, DIRECT_WINDOW_SIZE);
        for (uint64_t p = lo / PAGE_SIZE; p < (low_hi + PAGE_MASK) / PAGE_SIZE; ++p)
            if (direct_page_flags_[p].load(std::memory_order_relaxed) & PAGE_MAPPED)
                return true;
        lo = low_hi;
    }
    if (lo >= hi) return false;
    auto it = high_mappings_.lower_bound(lo);
    if (it != high_mappings_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > lo) return true;
    }
    return it != high_mappings_.end() && it->first < hi;
}
bool Memory::mapping_prot_at_locked(uint64_t addr, uint8_t& prot) const {
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE) {
        const uint8_t flags = direct_page_flags_[addr / PAGE_SIZE].load(
            std::memory_order_relaxed);
        if (!(flags & PAGE_MAPPED)) return false;
        prot = flags & 7;
        return true;
    }
    auto it = high_mappings_.upper_bound(addr);
    if (it == high_mappings_.begin()) return false;
    --it;
    if (it->first > addr || it->second.end <= addr) return false;
    prot = it->second.prot & 7;
    return true;
}
void Memory::set_mapping_locked(uint64_t lo, uint64_t hi, uint8_t prot,
                                bool combine) {
    if (lo >= hi) return;
    if (direct_window_ && lo < DIRECT_WINDOW_SIZE) {
        uint64_t low_hi = std::min<uint64_t>(hi, DIRECT_WINDOW_SIZE);
        // map_range is used for loader images and runtime-created regions;
        // overlapping ELF segment pages receive the union of their flags.
        if (!combine) (void)set_direct_prot(lo, low_hi, prot);
        uint64_t run_start = lo;
        uint8_t run_prot = 0;
        for (uint64_t p = lo / PAGE_SIZE; p < low_hi / PAGE_SIZE; ++p) {
            uint8_t old = direct_page_flags_[p].load(std::memory_order_relaxed);
            uint8_t next = combine && (old & PAGE_MAPPED)
                ? static_cast<uint8_t>((old & 7) | prot) : prot;
            direct_page_flags_[p].store(static_cast<uint8_t>(PAGE_MAPPED | next),
                                        std::memory_order_release);
            if (combine) {
                uint64_t page_addr = p * PAGE_SIZE;
                if (page_addr == lo) {
                    run_start = page_addr;
                    run_prot = next;
                } else if (next != run_prot) {
                    (void)set_direct_prot(run_start, page_addr, run_prot);
                    run_start = page_addr;
                    run_prot = next;
                }
            }
        }
        if (combine && run_start < low_hi)
            (void)set_direct_prot(run_start, low_hi, run_prot);
        lo = low_hi;
    }
    if (lo < hi) set_high_mapping_locked(lo, hi, prot, combine);
}
void Memory::check_access(uint64_t addr, size_t size, uint8_t access) const {
    if (size == 0) return;
    uint64_t end;
    if (__builtin_add_overflow(addr, static_cast<uint64_t>(size), &end))
        throw UnmappedMemory(addr, (access & GUEST_PROT_WRITE) != 0);
    uint64_t cur = addr;
    if (direct_window_ && cur < DIRECT_WINDOW_SIZE) {
        uint64_t low_end = std::min<uint64_t>(end, DIRECT_WINDOW_SIZE);
        for (uint64_t p = cur / PAGE_SIZE; p < (low_end + PAGE_MASK) / PAGE_SIZE; ++p) {
            uint8_t flags = direct_page_flags_[p].load(std::memory_order_acquire);
            if (!(flags & PAGE_MAPPED))
                throw UnmappedMemory(std::max(cur, p * PAGE_SIZE),
                                     (access & GUEST_PROT_WRITE) != 0);
            if (access && (flags & access) != access)
                throw UnmappedMemory(std::max(cur, p * PAGE_SIZE),
                                     (access & GUEST_PROT_WRITE) != 0, true);
        }
        cur = low_end;
    }
    if (cur < end) {
        std::shared_lock<std::shared_mutex> g(mu_);
        uint64_t pos = cur;
        auto it = high_mappings_.upper_bound(pos);
        if (it != high_mappings_.begin()) --it;
        while (pos < end) {
            while (it != high_mappings_.end() && it->second.end <= pos) ++it;
            if (it == high_mappings_.end() || it->first > pos)
                throw UnmappedMemory(pos, (access & GUEST_PROT_WRITE) != 0);
            if (access && (it->second.prot & access) != access)
                throw UnmappedMemory(pos, (access & GUEST_PROT_WRITE) != 0, true);
            pos = std::min<uint64_t>(end, it->second.end);
            ++it;
        }
    }
}
bool Memory::range_accessible(uint64_t addr, size_t size, uint8_t access) const {
    try {
        check_access(addr, size, access);
        return true;
    } catch (const UnmappedMemory&) {
        return false;
    }
}
bool Memory::mprotect_guest(uint64_t addr, uint64_t size, uint8_t prot) {
    if ((addr & PAGE_MASK) || (prot & ~uint8_t(7))) return false;
    if (size == 0) return true;
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end) ||
        __builtin_add_overflow(end, PAGE_MASK, &end)) return false;
    end &= ~PAGE_MASK;
    if (end <= addr) return false;
    std::unique_lock<std::shared_mutex> g(mu_);
    if (!range_mapped_locked(addr, end)) return false;
    uint64_t low_hi = std::min<uint64_t>(end, DIRECT_WINDOW_SIZE);
    if (direct_window_ && addr < low_hi &&
        !set_direct_prot(addr, low_hi, prot)) return false;
    if (direct_window_ && addr < low_hi) {
        for (uint64_t p = addr / PAGE_SIZE; p < low_hi / PAGE_SIZE; ++p)
            direct_page_flags_[p].store(static_cast<uint8_t>(PAGE_MAPPED | prot),
                                        std::memory_order_release);
    }
    uint64_t high_lo = direct_window_ ? std::max<uint64_t>(addr, DIRECT_WINDOW_SIZE) : addr;
    if (high_lo < end) set_high_mapping_locked(high_lo, end, prot, false);
    return true;
}
bool Memory::map_range(uint64_t addr, uint64_t size) {
    if (size == 0) return true;
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end) ||
        end > UINT64_MAX - PAGE_MASK || end > 0x800000000000ULL) return false;
    const uint64_t map_lo = addr & ~PAGE_MASK;
    const uint64_t map_hi = (end + PAGE_MASK) & ~PAGE_MASK;
    const uint64_t high_lo = direct_window_
        ? std::max<uint64_t>(map_lo, DIRECT_WINDOW_SIZE) : map_lo;
    if (map_hi > high_lo &&
        (map_hi - high_lo) / PAGE_SIZE > MAX_TOTAL_PAGES) return false;
    std::unique_lock<std::shared_mutex> g(mu_);
    size_t missing = 0;
    for (uint64_t s = map_lo; s < map_hi; s += PAGE_SIZE) {
        if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
        if (pages_.find(s / PAGE_SIZE) == pages_.end()) missing++;
    }
    if (missing && would_exceed_page_limit(missing)) return false;
    for (uint64_t start = map_lo; start < map_hi; start += PAGE_SIZE) {
        uint64_t pn = start / PAGE_SIZE;
        // For addresses in the direct window (< 4 GiB), the window IS
        // the storage — no need to create a pages_ entry.
        if (direct_window_ && start < DIRECT_WINDOW_SIZE) {
            continue;
        }
        auto it = pages_.find(pn);
        if (it == pages_.end()) {
            pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    set_mapping_locked(map_lo, map_hi,
        GUEST_PROT_READ | GUEST_PROT_WRITE, true);
    if (map_hi > DIRECT_WINDOW_SIZE)
        above_window_next_ = std::max(above_window_next_, map_hi);
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (map_lo < stack_bottom && map_hi > mmap_next_)
        mmap_next_ = map_hi;
    return true;
}
bool Memory::is_mapped(uint64_t addr, uint64_t size) const {
    if (size == 0) return true;
    return range_accessible(addr, size, 0);
}
void Memory::write(uint64_t addr, const void* src, size_t n, PageCache* pc) {
    if (n == 0) return;
    check_access(addr, n, GUEST_PROT_WRITE);
    // A direct-window access may straddle the 4 GiB storage boundary. Make
    // sure the sparse suffix can be materialized before committing the
    // direct prefix, so an OOM does not turn one guest store into a partial
    // write.
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        static_cast<uint64_t>(n) > DIRECT_WINDOW_SIZE - addr) {
        const uint64_t end = addr + static_cast<uint64_t>(n);
        const uint64_t high_end = (end + PAGE_MASK) & ~PAGE_MASK;
        std::unique_lock<std::shared_mutex> g(mu_);
        size_t missing = 0;
        for (uint64_t s = DIRECT_WINDOW_SIZE; s < high_end; s += PAGE_SIZE)
            if (pages_.find(s / PAGE_SIZE) == pages_.end()) ++missing;
        if (would_exceed_page_limit(missing))
            throw UnmappedMemory(DIRECT_WINDOW_SIZE, true);
        for (uint64_t s = DIRECT_WINDOW_SIZE; s < high_end; s += PAGE_SIZE) {
            const bool inserted = pages_.emplace(s / PAGE_SIZE,
                std::vector<uint8_t>(PAGE_SIZE, 0)).second;
            if (inserted) total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // BIFROST_WATCH: attribute stores overlapping the watch range. Log the
    // first 8 bytes of the written value too — the heap-stomp hunt needs to
    // spot the offending VALUE (e.g. a negative extents 0xC700 or a shifted
    // handle), not just the address.
    {
        const WatchRange& wr = watch_range();
        // BIFROST_WATCH_EXTENTS: temporary vkQuake hunt — log ONLY the four
        // CalcSurfaceExtents extent/texturemin stores (guest pcs) with the
        // value, so the run isn't drowned by unrelated scalar stores and can
        // actually reach the AllocBlock fault.
        // BIFROST_WATCH_VAL=0xc700: log any write whose first 8 bytes contain
        // the 16-bit value anywhere (the stomp that writes the corrupt
        // extents). Near-zero volume, so the run reaches the fault.
        static const uint32_t watch_val_ = []() -> uint32_t {
            const char* s = getenv("BIFROST_WATCH_VAL");
            return s ? static_cast<uint32_t>(strtoul(s, nullptr, 0)) : 0;
        }();
        bool val_match = false;
        if (watch_val_) {
            uint64_t vv = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&vv, src, vn);
            for (size_t b = 0; b + 2 <= vn; b++)
                if (static_cast<uint16_t>(vv >> (b * 8)) == watch_val_) { val_match = true; break; }
        }
        // BIFROST_WATCH_BADEXT: value-agnostic extents/stomp hunt. With
        // BIFROST_JIT_SLOW_STORES=1 every store lands here, so flag any
        // 2-byte store of a value that could drive AllocBlock (|v| > 4096;
        // legitimate extents/texturemins for small maps are tiny). No guess
        // about the exact corrupt value.
        static const bool bad_ext_ = getenv("BIFROST_WATCH_BADEXT") != nullptr;
        bool bad_ext = false;
        if (bad_ext_ && n == 2) {
            int16_t sv;
            std::memcpy(&sv, src, 2);
            if (sv < -4096 || sv > 4096) bad_ext = true;
        }
        // n<=8 only by default: the heap-stomp hunt cares about scalar
        // stores (handles, extents, pointers); logging every large memcpy
        // drowns the trace. BIFROST_WATCH_TRAP widens to every width so the
        // offending store is caught regardless of size.
        const bool range_hit = wr.on && addr < wr.hi && addr + n > wr.lo;
        if (val_match || bad_ext || (range_hit && (n <= 8 || dbg().watch_trap))) {
            uint64_t v0 = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&v0, src, vn);
            fprintf(stderr, "[watch] w 0x%llx n=%zu v=0x%llx pc=0x%llx\n",
                    (unsigned long long)addr, n,
                    (unsigned long long)v0,
                    (unsigned long long)t_watch_pc);
            fflush(stderr);
        }
        // BIFROST_WATCH_TRAP: stop dead at the first write into the watched
        // range. The guest pc has just been printed — that IS the stomping
        // store. Host SIGILL here is intentional (loud, unambiguous).
        if (range_hit && dbg().watch_trap) __builtin_trap();
    }
    // BIFROST_WRITE_TRACE=<path>: log every guest store (addr, size, tid,
    // guest pc, value). With an interpreter run plus BIFROST_JIT_SLOW_STORES
    // on a JIT run this is a COMPLETE store stream (JIT inline-window stores
    // bypass write() otherwise), so a clean-vs-bad differential localizes a
    // stomp to the exact writing instruction. BIFROST_WRITE_TRACE_RANGE=lo:hi
    // narrows the log to one region.
    static const int wt_fd = [] {
        if (dbg().write_trace.empty()) return -1;
        return ::open(dbg().write_trace.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }();
    static thread_local const long wt_tid = wt_fd >= 0 ? ::syscall((long)186) : 0;
    if (wt_fd >= 0) {
        const WatchRange& tr = trace_range();
        if (!tr.on || (addr < tr.hi && addr + n > tr.lo)) {
            uint64_t v0 = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&v0, src, vn);
            char rec[128];
            int len = snprintf(rec, sizeof(rec),
                               "w 0x%llx n=0x%zx t%ld pc=0x%llx v=0x%llx\n",
                               (unsigned long long)addr, n, wt_tid,
                               (unsigned long long)t_watch_pc,
                               (unsigned long long)v0);
            (void)::write(wt_fd, rec, len);
        }
    }
    // Fast path: direct window for addresses < 4 GiB.
    // BUGFIX: avoid integer overflow. `addr + n` can wrap to a small
    // value when addr is near UINT64_MAX, causing the check to pass
    // and the subsequent memcpy to write out-of-bounds at
    // `direct_window_ + addr` (a huge offset). Use a safe range check.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    uint64_t cur = addr;
    size_t remaining = n;
    if (direct_window_ && cur < DIRECT_WINDOW_SIZE) {
        const size_t window_take = static_cast<size_t>(std::min<uint64_t>(
            remaining, DIRECT_WINDOW_SIZE - cur));
        std::memcpy(direct_window_ + cur, p, window_take);
        if (window_take == remaining) return;
        p += window_take;
        cur += window_take;
        remaining -= window_take;
    }
    while (remaining > 0) {
        uint64_t pn = cur / PAGE_SIZE;
        uint64_t off = cur & PAGE_MASK;
        size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
        // Sparse-page path (>4 GiB). Hold mu_ for the whole access: a
        // concurrent munmap can erase this page's vector, so a cached raw
        // pointer must be validated AND dereferenced while no mutator can
        // run (erase takes the unique lock; we hold shared). The 4 GiB
        // direct window never reaches here, so the lock cost is off the
        // normal heap/stack hot path.
        bool cached_hit = false;
        {
            std::shared_lock<std::shared_mutex> g(mu_);
            if (pc && __builtin_expect(pn == pc->write_page &&
                        pc->write_epoch == page_epoch_.load(std::memory_order_relaxed), 1)) {
                memcpy(pc->write_ptr + off, p, take);
                cached_hit = true;
            }
        }
        if (!cached_hit) {
            std::unique_lock<std::shared_mutex> g(mu_);
            auto it = pages_.find(pn);
            if (it == pages_.end()) {
                // 1.5.4-alpha: OOM protection for write path.
                if (would_exceed_page_limit(1)) {
                    throw UnmappedMemory(cur, true);
                }
                it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                total_pages_.fetch_add(1, std::memory_order_relaxed);
            }
            memcpy(it->second.data() + off, p, take);
            if (pc) {
                pc->write_page = pn;
                pc->write_ptr = it->second.data();
                pc->write_epoch = page_epoch_.load(std::memory_order_relaxed);
            }
        }
        p += take;
        cur += take;
        remaining -= take;
    }
}
void Memory::read(uint64_t addr, void* dst, size_t n, PageCache* pc) const {
    read_access(addr, dst, n, pc, GUEST_PROT_READ);
}
void Memory::read_access(uint64_t addr, void* dst, size_t n, PageCache* pc,
                         uint8_t access) const {
    if (n == 0) return;
    check_access(addr, n, access);
    // BUGFIX: same integer-overflow guard as write() — `addr + n` can
    // wrap when addr is near UINT64_MAX, causing the direct-window fast
    // path to fire for an out-of-bounds address.
    uint8_t* p = static_cast<uint8_t*>(dst);
    uint64_t cur = addr;
    size_t remaining = n;
    if (direct_window_ && cur < DIRECT_WINDOW_SIZE) {
        const size_t window_take = static_cast<size_t>(std::min<uint64_t>(
            remaining, DIRECT_WINDOW_SIZE - cur));
        std::memcpy(p, direct_window_ + cur, window_take);
        if (window_take == remaining) return;
        p += window_take;
        cur += window_take;
        remaining -= window_take;
    }
    while (remaining > 0) {
        uint64_t pn = cur / PAGE_SIZE;
        uint64_t off = cur & PAGE_MASK;
        size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
        bool cached_hit = false;
        {
            std::shared_lock<std::shared_mutex> g(mu_);
            if (pc && __builtin_expect(pn == pc->read_page &&
                        pc->read_epoch == page_epoch_.load(std::memory_order_relaxed), 1)) {
                memcpy(p, pc->read_ptr + off, take);
                cached_hit = true;
            }
        }
        if (!cached_hit) {
            // 1.5.4-alpha: FEX-style demand paging. On real Linux, reads
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
            std::unique_lock<std::shared_mutex> g(mu_);
            auto it = pages_.find(pn);
            if (it == pages_.end()) {
                // 1.5.4-alpha: OOM protection for demand paging.
                // If auto-allocation would exceed the page limit,
                // throw UnmappedMemory (causing SIGSEGV delivery)
                // instead of letting the host OOM.
                if (would_exceed_page_limit(1)) {
                    throw UnmappedMemory(cur, false);
                }
                it = pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                total_pages_.fetch_add(1, std::memory_order_relaxed);
            }
            memcpy(p, it->second.data() + off, take);
            if (pc) {
                pc->read_page = pn;
                pc->read_ptr = it->second.data();
                pc->read_epoch = page_epoch_.load(std::memory_order_relaxed);
            }
        }
        p += take;
        cur += take;
        remaining -= take;
    }
}
uint64_t Memory::mmap_alloc(uint64_t size, uint64_t hint, bool noreserve,
                            uint8_t prot) {
    // BIFROST_ALLOC_TRACE=1: print every mmap_alloc (addr,size) — used to
    // audit placement/overlap of thunk bounces vs callback scratch stacks.
    static const bool alloc_trace_ = getenv("BIFROST_ALLOC_TRACE") != nullptr;
    if (alloc_trace_)
        fprintf(stderr, "[alloc] mmap_alloc(%zu) ...", (size_t)size);
    if (size == 0) size = PAGE_SIZE;
    // 1.5.4-alpha: Per-allocation length cap. Prevents a malicious guest
    // from requesting SIZE_MAX and OOMing the host. MAP_NORESERVE
    // reservations are exempt: they are VIRTUAL on real Linux (cost
    // nothing until touched), and touch-time faults still run the page
    // cap. Rejecting them made vkQuake's mimalloc fall through its whole
    // arena-size cascade (8 GiB → 4 → 2 → 1 GiB) opening an arena per
    // fallback level.
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;  // caller maps 0 to -ENOMEM
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t base = hint;
    uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    bool reused = false;
    if (base == 0) {
        // BUGFIX: reuse a reclaimed (munmap'd) range first instead of
        // always bumping. The bump allocator never recycled address
        // space, so the guest heap marched unboundedly (the minecraft
        // game's per-frame 18 MB+2.3 MB mesh buffers hit the 1M-page
        // cap after ~400 meshes and mmap started returning 0 → arena at
        // address 0 → free() BRK #1000). First-fit keeps the heap
        // clustered in the direct window (JIT fast path).
        for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
            if (it->second >= aligned_size) {
                // Never hand out a range that overlaps the main-stack
                // region — the stack is a fixed mapping, not allocator
                // space (vkQuake: mimalloc's page-zeroing memset wiped
                // live stack frames when a "heap" segment landed in the
                // stack band).
                if (it->first < stack_top_ &&
                    it->first + it->second > stack_top_ - STACK_SIZE)
                    continue;
                base = it->first;
                reused = true;
                break;
            }
        }
        if (!reused) {
            base = mmap_next_;
            mmap_next_ += aligned_size;
            // The bump must never place guest allocations inside the
            // main-stack region [stack_top_ - STACK_SIZE, stack_top_).
            // mmap_next_ starts at MMAP_BASE_MIN and grows without an
            // upper bound; a guest with a large heap (vkQuake's mimalloc
            // arena) marched it into the stack band and its zeroing
            // memset destroyed live frames (saved LR = 0 → pc=0
            // DecodeError, "AllocBlock: full"). Skip OVER the stack —
            // Linux-like (the kernel picks a free area) and it keeps the
            // allocation inside the 4 GiB direct window (JIT fast path).
            const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
            if (base < stack_top_ && base + aligned_size > stack_bottom) {
                base = stack_top_;
                mmap_next_ = stack_top_ + aligned_size;
            }
            // Above-window fallback: the 4 GiB window can be exhausted
            // by a guest with an enormous heap (vkQuake's lightmap atlas
            // plus its upstream per-map-reload lightstyle_data leak).
            // Pages above the window use the sparse pages_ storage —
            // the JIT's direct-window fast path won't cover them (the
            // bounds checks fall back to the pages_ + rwlock path), but
            // the guest keeps running instead of cascading into failed
            // allocations and NULL-data chaos.
            if (base + aligned_size > DIRECT_WINDOW_SIZE) {
                base = std::max<uint64_t>(DIRECT_WINDOW_SIZE,
                                          above_window_next_);
                above_window_next_ = base + aligned_size;
            }
        }
    } else {
        // 1.5.4-alpha: Validate MAP_FIXED address range. Reject
        // addresses in the NULL page region or kernel space.
        if (!is_valid_guest_range(base, aligned_size)) return 0;
        mmap_next_ = std::max(mmap_next_, base + aligned_size);
        // A fixed mapping above the window must also advance the
        // above-window bump cursor, or a later mmap_alloc(NULL,…) can
        // hand out an address overlapping it.
        if (base + aligned_size > DIRECT_WINDOW_SIZE)
            above_window_next_ = std::max(above_window_next_, base + aligned_size);
    }
    // 1.5.4-alpha: OOM protection. Check page count before allocating.
    // Count only pages that would actually be added (non-window pages not
    // already present in pages_). Reused window ranges add nothing, and a
    // reused above-window range re-adds pages that untrack freed — so the
    // check reflects the LIVE page count, not a naive aligned_size count.
    size_t num_new_pages = 0;
    // All-window ranges add no pages_ entry (the window IS the storage and
    // never counts toward total_pages_) — skip the per-page scan entirely
    // instead of walking ~4608 pages doing nothing.
    if (!(direct_window_ && base + aligned_size <= DIRECT_WINDOW_SIZE)) {
        for (uint64_t s = base & ~PAGE_MASK; s < base + aligned_size; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            uint64_t pn = s / PAGE_SIZE;
            if (pages_.find(pn) == pages_.end()) num_new_pages++;
        }
    }
    if (would_exceed_page_limit(noreserve ? 0 : num_new_pages)) return 0;
    // BIFROST_MEMGUARD=1 (vkQuake AllocBlock hunt): scream if the chosen
    // range overlaps ANY allocation still tracked as live. Should be
    // impossible — free_ranges_ must never contain tracked-live space —
    // so a hit here means accounting drift somewhere (untrack/mremap/
    // clone_for_fork) and directly explains "zero-and-rewrite of live
    // guest heap" corruption.
    static const bool memguard_ = getenv("BIFROST_MEMGUARD") != nullptr;
    if (memguard_) {
        const uint64_t glo = base, ghi = base + aligned_size;
        for (const auto& kv : allocations_) {
            if (kv.first < ghi && kv.first + kv.second > glo) {
                fprintf(stderr,
                        "[memguard] LIVE-OVERLAP new=[0x%llx..0x%llx) "
                        "live=[0x%llx..0x%llx)\n",
                        (unsigned long long)glo, (unsigned long long)ghi,
                        (unsigned long long)kv.first,
                        (unsigned long long)(kv.first + kv.second));
            }
        }
    }
    // Consume the taken free range (leave the tail for later reuse).
    if (reused) remove_free_range(base, aligned_size);
    // A MAP_FIXED allocation reclaims any free range it overlaps.
    if (hint != 0) remove_free_range(base, aligned_size);
    uint64_t start = base & ~PAGE_MASK;
    uint64_t end = base + aligned_size;  // page-aligned end
    // Reused window pages may hold stale data from the previous owner —
    // zero them for MAP_ANONYMOUS semantics with ONE bulk memset.
    // Perf (host microbench, 18 MB): bulk memset 0.22 ms, per-page
    // memsets ~0.5 ms, but madvise(MADV_DONTNEED)-based lazy zeroing is
    // ~5.1 ms because every page faults back through the kernel on the
    // game's next write. This game fully overwrites its mesh buffers
    // every frame, so eager zeroing beats fault-based zeroing ~20x — the
    // window pages stay resident across munmap/reuse (munmap does NOT
    // madvise), keeping the game's writes fault-free.
    if ((reused || hint != 0) && direct_window_ && start < DIRECT_WINDOW_SIZE) {
        uint64_t wend = std::min(end, static_cast<uint64_t>(DIRECT_WINDOW_SIZE));
        if (wend > start) {
            (void)set_direct_prot(start, wend, PROT_READ | PROT_WRITE);
            std::memset(direct_window_ + start, 0, wend - start);
        }
    }
    size_t pages_added = 0;
    for (; start < end; start += PAGE_SIZE) {
        uint64_t pn = start / PAGE_SIZE;
        if (direct_window_ && start < DIRECT_WINDOW_SIZE) {
            continue;
        }
        // MAP_NORESERVE / PROT_NONE reservations are VIRTUAL on real
        // Linux — cost nothing until touched. Do NOT materialize their
        // pages eagerly; read()/write() demand-fault them one at a time
        // (each fault still runs the OOM check). vkQuake's bundled
        // mimalloc reserves GiB-scale arenas this way: eager materialize
        // + page-cap accounting turned every reserve into a live-GiB
        // charge, exhausted MAX_TOTAL_PAGES, and forced mimalloc into a
        // multi-arena fallback cascade that shredded its own heap.
        if (noreserve) continue;
        auto it = pages_.find(pn);
        if (it == pages_.end()) {
            pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
            pages_added++;
        } else if (reused) {
            // Reclaimed pages above the window: zero stale data.
            std::fill(it->second.begin(), it->second.end(), 0);
        }
    }
    // Track total pages atomically (relaxed — no cross-thread sync needed).
    total_pages_.fetch_add(pages_added, std::memory_order_relaxed);
    // BIFROST_GUARD_ALLOCS=1: append a PROT_NONE guard page right after
    // each bump-path window allocation so an out-of-bounds guest access
    // faults at the offending store (reported by the guard-fault handler
    // in signal.cpp) instead of silently shredding the next chunk. Only
    // anonymous bump allocations are guarded: reused ranges, MAP_FIXED
    // hints, above-window and MAP_NORESERVE mappings are left alone (their
    // tail page may belong to a live neighbour). The guard page is never
    // entered into allocations_/free_ranges_ — deliberately leaked for the
    // debug run so it can never be handed out while PROT_NONE. It IS in
    // guard_pages_, so snapshot/madvise/reuse skip it, and
    // mmap_fixed_replace restores it if a later MAP_FIXED covers it.
    static const bool guard_allocs_ = getenv("BIFROST_GUARD_ALLOCS") != nullptr;
    if (guard_allocs_ && !reused && hint == 0 && !noreserve && direct_window_ &&
        base + aligned_size + PAGE_SIZE <= DIRECT_WINDOW_SIZE) {
        const uint64_t gpage = base + aligned_size;  // page-aligned
        const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
        const bool over_stack = gpage < stack_top_ && gpage + PAGE_SIZE > stack_bottom;
        if (!over_stack &&
            ::mprotect(direct_window_ + gpage, PAGE_SIZE, PROT_NONE) == 0) {
            guard_pages_.insert(gpage / PAGE_SIZE);
            direct_page_flags_[gpage / PAGE_SIZE].store(PAGE_MAPPED,
                                                         std::memory_order_release);
            // Reserve the guard VA so the bump allocator never reuses it.
            if (gpage + PAGE_SIZE > mmap_next_) mmap_next_ = gpage + PAGE_SIZE;
        }
    }
    allocations_[base] = aligned_size;
    set_mapping_locked(base, base + aligned_size, prot, false);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    if (alloc_trace_)
        fprintf(stderr, " [0x%llx..0x%llx)\n",
                (unsigned long long)base,
                (unsigned long long)(base + aligned_size));
    return base;
}
uint64_t Memory::mmap_fixed_noreplace(uint64_t addr, uint64_t size,
                                      bool noreserve, uint8_t prot) {
    if (size == 0 || (addr & PAGE_MASK) || size > UINT64_MAX - PAGE_MASK)
        return 0;
    const uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (!is_valid_guest_range(addr, aligned_size)) return 0;
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;
    const uint64_t end = addr + aligned_size;
    std::unique_lock<std::shared_mutex> g(mu_);
    if (range_overlaps_locked(addr, end)) return UINT64_MAX;
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (addr < stack_top_ && end > stack_bottom) return 0;

    size_t num_new_pages = 0;
    if (!(direct_window_ && end <= DIRECT_WINDOW_SIZE) && !noreserve) {
        for (uint64_t s = addr; s < end; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            if (pages_.find(s / PAGE_SIZE) == pages_.end()) ++num_new_pages;
        }
    }
    if (would_exceed_page_limit(num_new_pages)) return 0;

    if (direct_window_ && addr < DIRECT_WINDOW_SIZE) {
        uint64_t low_hi = std::min<uint64_t>(end, DIRECT_WINDOW_SIZE);
        if (!set_direct_prot(addr, low_hi, PROT_READ | PROT_WRITE)) return 0;
        std::memset(direct_window_ + addr, 0, low_hi - addr);
    }
    size_t pages_added = 0;
    if (!noreserve) {
        for (uint64_t s = addr; s < end; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            auto [it, inserted] = pages_.emplace(
                s / PAGE_SIZE, std::vector<uint8_t>(PAGE_SIZE, 0));
            if (inserted) ++pages_added;
        }
    }
    total_pages_.fetch_add(pages_added, std::memory_order_relaxed);
    allocations_[addr] = aligned_size;
    remove_free_range(addr, aligned_size);
    mmap_next_ = std::max(mmap_next_, end);
    if (end > DIRECT_WINDOW_SIZE)
        above_window_next_ = std::max(above_window_next_, end);
    set_mapping_locked(addr, end, prot, false);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    page_epoch_.fetch_add(1, std::memory_order_relaxed);
    return addr;
}
uint64_t Memory::mmap_fixed_replace(uint64_t addr, uint64_t size, bool noreserve,
                                    bool zero_contents, uint8_t prot) {
    if (size == 0) size = PAGE_SIZE;
    // Fixed mappings require a page-aligned address (Linux returns EINVAL
    // otherwise; the syscall entry checks this too, this is defense here so
    // tracking keys never go unaligned).
    if (addr & PAGE_MASK) return 0;
    uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (!is_valid_guest_range(addr, aligned_size)) return 0;
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;
    std::unique_lock<std::shared_mutex> g(mu_);
    // The main stack is a fixed map_range mapping, not an allocation. A
    // fixed mmap over it would create dual ownership and shred frames, so
    // refuse (caller maps 0 to -ENOMEM). Real Linux would replace it, but
    // corrupting the stack is worse than diverging here; guests never do
    // this except by bug.
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (addr < stack_top_ && addr + aligned_size > stack_bottom) return 0;
    const uint64_t lo = addr;
    const uint64_t hi = addr + aligned_size;
    // Pre-mutation OOM check (gross need). Eviction below can only free
    // pages, so passing here means the post-evict state also passes. This
    // must run before mutating so a failed fixed map leaves live state
    // intact.
    size_t gross_new = 0;
    if (!(direct_window_ && hi <= DIRECT_WINDOW_SIZE)) {
        for (uint64_t s = lo; s < hi; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            if (pages_.find(s / PAGE_SIZE) == pages_.end()) gross_new++;
        }
    }
    if (would_exceed_page_limit(noreserve ? 0 : gross_new)) return 0;
    // VMA-split eviction of overlapping live allocations.
    std::vector<std::pair<uint64_t, uint64_t>> hits;
    for (const auto& kv : allocations_) {
        const uint64_t b = kv.first;
        const uint64_t e = b + kv.second;
        if (b < hi && e > lo) hits.push_back({b, kv.second});
    }
    for (const auto& h : hits) {
        const uint64_t b = h.first;
        const uint64_t e = b + h.second;
        // BIFROST_MAPFIX_TRACE: log every live allocation a MAP_FIXED
        // replaces (mimalloc commits/decommits arenas this way; an eviction
        // of a live large allocation is the heap-overlap suspect).
        static const bool mapfix_trace_ =
            getenv("BIFROST_MAPFIX_TRACE") != nullptr;
        if (mapfix_trace_)
            fprintf(stderr,
                    "[mapfix] fixed [0x%llx..0x%llx) %s evicts live "
                    "[0x%llx..0x%llx) len=%llu\n",
                    (unsigned long long)lo, (unsigned long long)hi,
                    zero_contents ? "zero" : "keep",
                    (unsigned long long)b, (unsigned long long)e,
                    (unsigned long long)h.second);
        allocations_.erase(b);
        const uint64_t flo = (lo > b) ? lo : b;
        const uint64_t fhi = (hi < e) ? hi : e;
        if (flo > b) allocations_[b] = flo - b;
        if (e > fhi) allocations_[fhi] = e - fhi;
        if (fhi <= flo) continue;
        g_memstats_cum_free.fetch_add(fhi - flo, std::memory_order_relaxed);
        for (uint64_t s = flo; s < fhi; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            auto pit = pages_.find(s / PAGE_SIZE);
            if (pit != pages_.end()) {
                pages_.erase(pit);
                total_pages_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
    // Restore host prot on reclaimed callback-stack guard pages. The guards
    // are host PROT_NONE inside the window; leaving them protected would
    // host-SIGSEGV on the next window access.
    for (uint64_t s = lo; s < hi; s += PAGE_SIZE) {
        uint64_t pn = s / PAGE_SIZE;
        auto git = guard_pages_.find(pn);
        if (git != guard_pages_.end()) {
            guard_pages_.erase(git);
            if (direct_window_ && s < DIRECT_WINDOW_SIZE)
                (void)::mprotect(direct_window_ + s, PAGE_SIZE, PROT_READ | PROT_WRITE);
        }
    }
    remove_free_range(lo, aligned_size);
    // Fresh-zero anonymous semantics for the window (the window IS the
    // storage, so there is no lazy fault path here). Sparse pages stay lazy
    // when noreserve: absent pages fault in as zero, existing pages are
    // still cleared when zero_contents is set.
    if (zero_contents && direct_window_ && lo < DIRECT_WINDOW_SIZE) {
        uint64_t wend = std::min(hi, static_cast<uint64_t>(DIRECT_WINDOW_SIZE));
        if (wend > lo) {
            (void)set_direct_prot(lo, wend, PROT_READ | PROT_WRITE);
            std::memset(direct_window_ + lo, 0, wend - lo);
        }
    }
    size_t pages_added = 0;
    for (uint64_t s = lo; s < hi; s += PAGE_SIZE) {
        if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
        uint64_t pn = s / PAGE_SIZE;
        auto it = pages_.find(pn);
        if (it == pages_.end()) {
            if (noreserve) continue;
            pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
            pages_added++;
        } else if (zero_contents) {
            std::fill(it->second.begin(), it->second.end(), 0);
        }
    }
    total_pages_.fetch_add(pages_added, std::memory_order_relaxed);
    allocations_[lo] = aligned_size;
    set_mapping_locked(lo, hi, prot, false);
    mmap_next_ = std::max(mmap_next_, hi);
    // Keep the above-window bump cursor ahead of fixed high mappings so a
    // later mmap_alloc(NULL,…) can't return an address inside them.
    if (hi > DIRECT_WINDOW_SIZE)
        above_window_next_ = std::max(above_window_next_, hi);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    // Any pages_/prot change invalidates cached raw pointers.
    page_epoch_.fetch_add(1, std::memory_order_relaxed);
    return lo;
}
// ── mmap_alloc_callback_stack — guarded scratch stack for host-thread ──
// guest callbacks. Layout: [GUARD][usable][GUARD]. The guards are
// mprotect(PROT_NONE) pages inside the direct window, so any overflow
// faults immediately (guest SIGSEGV) instead of silently corrupting the
// neighboring malloc chunks. See the neverball/vkQuake heap-corruption
// hunt (2026-08-25): the audio pump's callback descended >72 KB through
// adjacent 4/8/64 KB scratch stacks and shredded live chunks.
uint64_t Memory::mmap_alloc_callback_stack(uint64_t usable_size) {
    constexpr uint64_t P = PAGE_SIZE;
    uint64_t usable = (usable_size + P - 1) & ~(P - 1);
    // Base allocation covers [guard][usable][guard].
    uint64_t base = mmap_alloc(usable + 2 * P);
    if (base == 0) return 0;
    if (direct_window_) {
        // Guards are per-page PROT_NONE holes inside the RW window.
        mprotect(direct_window_ + base, P, PROT_NONE);
        mprotect(direct_window_ + base + P + usable, P, PROT_NONE);
        std::unique_lock<std::shared_mutex> g(mu_);
        guard_pages_.insert(base / P);
        guard_pages_.insert((base + P + usable) / P);
        direct_page_flags_[base / P].store(PAGE_MAPPED, std::memory_order_release);
        direct_page_flags_[(base + P + usable) / P].store(PAGE_MAPPED,
                                                          std::memory_order_release);
    }
    (void)mprotect_guest(base + P, usable,
                         GUEST_PROT_READ | GUEST_PROT_WRITE);
    return base + P;  // usable base; sp starts at base + P + usable
}
uint64_t Memory::mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size) {
    if (old_addr == 0 || old_size == 0) return 0;
    if (new_size == 0) new_size = PAGE_SIZE;
    if (old_size > MAX_MMAP_LENGTH || new_size > MAX_MMAP_LENGTH ||
        old_size > UINT64_MAX - PAGE_MASK ||
        new_size > UINT64_MAX - PAGE_MASK) return 0;
    uint64_t old_aligned = (old_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t new_aligned = (new_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t old_end;
    uint64_t new_end;
    if (__builtin_add_overflow(old_addr, old_aligned, &old_end) ||
        __builtin_add_overflow(old_addr, new_aligned, &new_end) ||
        !is_valid_guest_range(old_addr, std::max(old_aligned, new_aligned)))
        return 0;
    // Shrink: keep the same address, reclaim the tail pages + address
    // range so repeated realloc-shrink doesn't march the page cap.
    if (new_aligned <= old_aligned) {
        std::unique_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(old_addr);
        if (it == allocations_.end() || it->second != old_aligned) return 0;
        const uint64_t fstart = old_addr + new_aligned;
        const uint64_t fend = old_end;
        it->second = new_aligned;
        if (fstart < fend) {
            for (uint64_t s = fstart; s < fend; s += PAGE_SIZE) {
                if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
                auto pit = pages_.find(s / PAGE_SIZE);
                if (pit != pages_.end()) {
                    pages_.erase(pit);
                    total_pages_.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            const uint64_t low_hi = std::min<uint64_t>(fend, DIRECT_WINDOW_SIZE);
            if (direct_window_ && fstart < low_hi) {
                (void)set_direct_prot(fstart, low_hi, PROT_NONE);
                for (uint64_t p = fstart / PAGE_SIZE; p < low_hi / PAGE_SIZE; ++p) {
                    direct_page_flags_[p].store(0, std::memory_order_release);
                    guard_pages_.erase(p);
                }
            }
            const uint64_t high_lo = direct_window_
                ? std::max<uint64_t>(fstart, DIRECT_WINDOW_SIZE) : fstart;
            if (high_lo < fend) erase_high_mapping_locked(high_lo, fend);
            add_free_range(fstart, fend - fstart);
            page_epoch_.fetch_add(1, std::memory_order_relaxed);
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
    const uint64_t extra_start = old_end;
    const uint64_t extra_end = new_end;
    bool can_grow_in_place = true;
    // The stack region is a fixed mapping, not allocator space — refuse
    // an in-place grow that would extend into it (the caller falls back
    // to move; mmap_alloc's bump skips over the stack the same way).
    if (extra_start < stack_top_ && extra_end > stack_top_ - STACK_SIZE)
        can_grow_in_place = false;
    std::unique_lock<std::shared_mutex> g(mu_);
    auto allocation = allocations_.find(old_addr);
    // The old range must still be the tracked mapping described by
    // old_size. This also gives us the page permissions to preserve across
    // either an in-place extension or a move.
    if (allocation == allocations_.end() || allocation->second != old_aligned ||
        !range_mapped_locked(old_addr, old_end)) return 0;
    std::vector<uint8_t> old_page_prots;
    old_page_prots.reserve(old_aligned / PAGE_SIZE);
    for (uint64_t s = old_addr; s < old_end; s += PAGE_SIZE) {
        uint8_t page_prot = 0;
        if (!mapping_prot_at_locked(s, page_prot)) {
            can_grow_in_place = false;
            break;
        }
        old_page_prots.push_back(page_prot);
    }
    if (old_page_prots.size() != old_aligned / PAGE_SIZE) return 0;
    // Any existing VMA, including map_range-only images/guards, blocks an
    // in-place extension. Allocation tracking alone misses those mappings.
    if (range_overlaps_locked(extra_start, extra_end))
        can_grow_in_place = false;
    if (can_grow_in_place) {
        // OOM check first: count above-window pages that would be added.
        // (Window pages need no pages_ entry and don't count.)
        size_t need = 0;
        for (uint64_t s = extra_start; s < extra_end; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            if (pages_.find(s / PAGE_SIZE) == pages_.end()) need++;
        }
        if (would_exceed_page_limit(need)) return 0;  // keep old mapping
        // New pages in a grown anonymous mapping start as zero, including
        // direct-window pages left over from a previous mapping.
        const uint64_t low_hi = std::min<uint64_t>(extra_end, DIRECT_WINDOW_SIZE);
        if (direct_window_ && extra_start < low_hi) {
            (void)set_direct_prot(extra_start, low_hi, PROT_READ | PROT_WRITE);
            std::memset(direct_window_ + extra_start, 0, low_hi - extra_start);
        }
        size_t added = 0;
        for (uint64_t s = extra_start; s < extra_end; s += PAGE_SIZE) {
            uint64_t pn = s / PAGE_SIZE;
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) {
                continue;
            }
            auto it = pages_.find(pn);
            if (it == pages_.end()) {
                pages_.emplace(pn, std::vector<uint8_t>(PAGE_SIZE, 0));
                added++;
            } else {
                std::fill(it->second.begin(), it->second.end(), 0);
            }
        }
        total_pages_.fetch_add(added, std::memory_order_relaxed);
        // can_grow_in_place guarantees old_addr is tracked.
        allocations_[old_addr] = new_aligned;
        set_mapping_locked(extra_start, extra_end, old_page_prots.back(), false);
        // BUGFIX: bump mmap_next_ past the grown region. Without this,
        // a subsequent mmap_alloc(NULL,...) would return an address
        // inside the grown (and possibly already-freed) region.
        mmap_next_ = std::max(mmap_next_, old_addr + new_aligned);
        // Same for the above-window cursor when the grown region is high.
        if (old_addr + new_aligned > DIRECT_WINDOW_SIZE)
            above_window_next_ = std::max(above_window_next_, old_addr + new_aligned);
        // The grown region now belongs to this allocation — remove any
        // reclaimed free range it overlaps (mremap grows into space that
        // a prior munmap may have returned to the pool).
        remove_free_range(extra_start, extra_end - extra_start);
        page_epoch_.fetch_add(1, std::memory_order_relaxed);
        return old_addr;
    }
    // Collision detected: allocate a fresh region, copy the data, and
    // return the new address. We must release the unique lock here
    // because mmap_alloc/read/write all acquire it themselves.
    g.unlock();
    uint64_t new_addr = mmap_alloc(new_size, 0, false,
        GUEST_PROT_READ | GUEST_PROT_WRITE);
    if (new_addr == 0) return 0;  // oom: keep old mapping intact
    const size_t copy_size = static_cast<size_t>(std::min(old_size, new_size));
    if (copy_size > 0) {
        std::vector<uint8_t> buf(copy_size);
        std::unique_lock<std::shared_mutex> copy_lock(mu_);
        const uint64_t new_aligned_end = new_addr + new_aligned;
        // Copy through the emulator's backing stores so read-only and
        // PROT_NONE source mappings can be moved without weakening their
        // guest permissions. Direct-window host protection is temporary.
        const uint64_t old_low_hi = std::min<uint64_t>(old_end, DIRECT_WINDOW_SIZE);
        const uint64_t new_low_hi = std::min<uint64_t>(new_aligned_end, DIRECT_WINDOW_SIZE);
        if (direct_window_ && old_addr < old_low_hi)
            (void)set_direct_prot(old_addr, old_low_hi, PROT_READ | PROT_WRITE);
        if (direct_window_ && new_addr < new_low_hi)
            (void)set_direct_prot(new_addr, new_low_hi, PROT_READ | PROT_WRITE);
        size_t copied = 0;
        while (copied < copy_size) {
            const uint64_t src_addr = old_addr + copied;
            const size_t take = std::min<size_t>(copy_size - copied,
                static_cast<size_t>(PAGE_SIZE - (src_addr & PAGE_MASK)));
            if (direct_window_ && src_addr < DIRECT_WINDOW_SIZE) {
                std::memcpy(buf.data() + copied, direct_window_ + src_addr, take);
            } else {
                auto it = pages_.find(src_addr / PAGE_SIZE);
                if (it == pages_.end())
                    std::memset(buf.data() + copied, 0, take);
                else
                    std::memcpy(buf.data() + copied,
                                it->second.data() + (src_addr & PAGE_MASK), take);
            }
            copied += take;
        }
        copied = 0;
        while (copied < copy_size) {
            const uint64_t dst_addr = new_addr + copied;
            const size_t take = std::min<size_t>(copy_size - copied,
                static_cast<size_t>(PAGE_SIZE - (dst_addr & PAGE_MASK)));
            if (direct_window_ && dst_addr < DIRECT_WINDOW_SIZE) {
                std::memcpy(direct_window_ + dst_addr, buf.data() + copied, take);
            } else {
                auto it = pages_.find(dst_addr / PAGE_SIZE);
                if (it == pages_.end()) {
                    it = pages_.emplace(dst_addr / PAGE_SIZE,
                        std::vector<uint8_t>(PAGE_SIZE, 0)).first;
                    total_pages_.fetch_add(1, std::memory_order_relaxed);
                }
                std::memcpy(it->second.data() + (dst_addr & PAGE_MASK),
                            buf.data() + copied, take);
            }
            copied += take;
        }
        // Preserve every old page's protection; newly added tail pages
        // inherit the former last page's permissions.
        for (uint64_t i = 0; i < new_aligned / PAGE_SIZE;) {
            const uint8_t prot = old_page_prots[std::min<uint64_t>(
                i, old_page_prots.size() - 1)];
            uint64_t j = i + 1;
            while (j < new_aligned / PAGE_SIZE &&
                   old_page_prots[std::min<uint64_t>(j,
                       old_page_prots.size() - 1)] == prot) ++j;
            set_mapping_locked(new_addr + i * PAGE_SIZE,
                              new_addr + j * PAGE_SIZE, prot, false);
            i = j;
        }
        // Restore the source's host permissions after the internal copy.
        for (uint64_t i = 0; i < old_page_prots.size();) {
            const uint8_t prot = old_page_prots[i];
            uint64_t j = i + 1;
            while (j < old_page_prots.size() && old_page_prots[j] == prot) ++j;
            (void)set_direct_prot(old_addr + i * PAGE_SIZE,
                                  old_addr + j * PAGE_SIZE, prot);
            i = j;
        }
    }
    // Reclaim the old range (free its pages + hand the address space
    // back to the free list) instead of just erasing the tracking entry.
    untrack_allocation(old_addr, old_aligned);
    return new_addr;
}
void Memory::untrack_allocation(uint64_t addr, uint64_t size) {
    // munmap(addr, 0) is a no-op. Without this guard the page-rounding
    // below would compute hi = lo + PAGE_SIZE for an UNALIGNED addr and
    // free the page containing it (Linux rejects unaligned munmap with
    // EINVAL; musl always aligns, but be defensive).
    if (size == 0) return;
    if (addr & PAGE_MASK) return;
    // BUGFIX: must hold a *unique* lock to mutate allocations_. The old
    // code used shared_lock, which is a data race (UB) if another thread
    // is concurrently reading allocations_ via mremap_grow().
    //
    // BUGFIX: the old code only erased the allocation from the tracking
    // map. It never reclaimed the pages_ entries, never decremented
    // total_pages_, and never returned the address range to a pool. That
    // made mmap_alloc a pure bump allocator: mallocng's huge allocations
    // (~18 MB DATA + ~2.3 MB INDICES per chunk mesh) were mmap'd and
    // munmap'd every frame, marching mmap_next_ up to ~8.5 GB and
    // exhausting MAX_TOTAL_PAGES (1M pages = 4 GiB) after ~400 churn
    // cycles. mmap_alloc then returned 0, which the mmap syscall handed
    // to the guest as a *successful* mapping at address 0 (musl only
    // treats -1/MAP_FAILED as failure), so mallocng built its arena at
    // address 0 and free() crashed with BRK #1000 in get_meta.
    //
    // Now munmap truly frees: page storage is dropped, the page-count
    // cap reflects live memory, and the address range goes on a free list
    // for reuse (keeping the guest heap inside the 4 GiB direct window,
    // which is also the JIT fast path).
std::unique_lock<std::shared_mutex> g(mu_);
    // Linux-like VMA-split semantics (2026-08-24, vkQuake hardening):
    // a munmap may cover any sub-range of one or more tracked
    // allocations. Free exactly the tracked intersection — NEVER hand
    // untracked space to free_ranges_: a later mmap_alloc would first-
    // fit it and its MAP_ANONYMOUS zeroing would destroy LIVE data
    // (garbage guest heap → corrupted BSP vertices → lightmap allocator
    // explosion). The old code did allocations_.erase(addr) (exact
    // match — silently missing interior/oversized munmaps) and then
    // add_free_range(addr, size) UNCONDITIONALLY.
    const uint64_t lo = addr & ~PAGE_MASK;
    // Reject wrapping ranges BEFORE page-rounding. addr + size + PAGE_MASK
    // can overflow to a small value (silently skipping the free) or, with
    // a huge size, match every tracked allocation and free live memory.
    // Linux returns -EINVAL for a wrapping munmap; the syscall layer
    // enforces that, and this is defense in depth for internal callers.
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end)) return;
    uint64_t hi;
    if (__builtin_add_overflow(end, static_cast<uint64_t>(PAGE_MASK), &hi)) {
        hi = ~static_cast<uint64_t>(0);  // saturate: covers the rest
    } else {
        hi &= ~PAGE_MASK;
    }
    if (hi <= lo) return;  // nothing after page rounding
    // Collect intersecting allocations first (can't mutate while iterating).
    std::vector<std::pair<uint64_t, uint64_t>> hits;  // [base, size)
    for (const auto& kv : allocations_) {
        const uint64_t b = kv.first;
        const uint64_t e = b + kv.second;
        if (b < hi && e > lo) hits.push_back({b, kv.second});
    }
    for (const auto& h : hits) {
        const uint64_t b = h.first;
        const uint64_t e = b + h.second;
        allocations_.erase(b);
        // Page-aligned freed intersection.
        const uint64_t flo = (lo > b) ? lo : b;
        const uint64_t fhi = (hi < e) ? hi : e;
        // Head/tail remainders stay tracked (VMA-split semantics).
        if (flo > b) allocations_[b] = flo - b;
        if (e > fhi) allocations_[fhi] = e - fhi;
        if (fhi <= flo) continue;  // nothing left after page rounding
        g_memstats_cum_free.fetch_add(fhi - flo, std::memory_order_relaxed);
        // Free page storage for the freed intersection.
        for (uint64_t s = flo; s < fhi; s += PAGE_SIZE) {
            // Direct-window addresses have no pages_ entry (the window
            // IS the storage) — nothing to reclaim there, and they
            // don't count toward total_pages_.
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            uint64_t pn = s / PAGE_SIZE;
            auto it = pages_.find(pn);
            if (it != pages_.end()) {
                pages_.erase(it);
                total_pages_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        add_free_range(flo, fhi - flo);
    }
    // Remove mapping state even when the range was installed through
    // map_range (ELF PT_LOAD, brk, stacks, vDSO) rather than mmap_alloc.
    const uint64_t low_hi = std::min<uint64_t>(hi, DIRECT_WINDOW_SIZE);
    if (direct_window_ && lo < low_hi) {
        (void)set_direct_prot(lo, low_hi, PROT_NONE);
        for (uint64_t p = lo / PAGE_SIZE; p < low_hi / PAGE_SIZE; ++p)
            direct_page_flags_[p].store(0, std::memory_order_release);
        for (uint64_t p = lo / PAGE_SIZE; p < low_hi / PAGE_SIZE; ++p)
            guard_pages_.erase(p);
    }
    const uint64_t high_lo = direct_window_
        ? std::max<uint64_t>(lo, DIRECT_WINDOW_SIZE) : lo;
    if (high_lo < hi) erase_high_mapping_locked(high_lo, hi);
    for (auto it = pages_.begin(); it != pages_.end();) {
        const uint64_t page_addr = it->first * PAGE_SIZE;
        if (page_addr >= high_lo && page_addr < hi) {
            it = pages_.erase(it);
            total_pages_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
    // Any pages_ erase invalidates cached raw pointers held by per-cpu
    // PageCache entries. Bump the epoch so stale entries miss instead
    // of uaf into freed vector storage.
    page_epoch_.fetch_add(1, std::memory_order_relaxed);
}
void Memory::madvise_dontneed(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    // Linux applies MADV_DONTNEED to whole pages: addr is page-aligned and
    // len is rounded upward. Internal callers get the same behavior even
    // if they pass an unaligned range.
    uint64_t start = addr & ~PAGE_MASK;
    uint64_t end;
    if (__builtin_add_overflow(addr, len, &end) ||
        __builtin_add_overflow(end, PAGE_MASK, &end)) end = UINT64_MAX & ~PAGE_MASK;
    else end &= ~PAGE_MASK;
    // Direct-window part: the window IS the storage — memset zeros.
    // Skip PROT_NONE callback-stack guard pages (host would SIGSEGV).
    if (direct_window_ && start < DIRECT_WINDOW_SIZE) {
        uint64_t wend = std::min(end, static_cast<uint64_t>(DIRECT_WINDOW_SIZE));
        {
            std::unique_lock<std::shared_mutex> g(mu_);
            for (uint64_t s = start; s < wend; s += PAGE_SIZE) {
                if (guard_pages_.count(s / PAGE_SIZE)) continue;
                const uint8_t flags = direct_page_flags_[s / PAGE_SIZE].load(
                    std::memory_order_relaxed);
                if (!(flags & PAGE_MAPPED)) continue;
                const uint8_t prot = flags & 7;
                if (!(prot & GUEST_PROT_WRITE))
                    (void)set_direct_prot(s, s + PAGE_SIZE, PROT_READ | PROT_WRITE);
                std::memset(direct_window_ + s, 0, PAGE_SIZE);
                if (!(prot & GUEST_PROT_WRITE))
                    (void)set_direct_prot(s, s + PAGE_SIZE, prot);
            }
        }
        if (end <= DIRECT_WINDOW_SIZE) return;
        start = DIRECT_WINDOW_SIZE;
    }
    // Sparse pages_: zero covered bytes in place. Never erase entries —
    // a cached PageCache pointer into an erased vector would dangle
    // (same reason untrack's page reclaim is munmap-only). Absent pages
    // already read as zero (demand paging), so nothing to do for them.
    // Linux ignores unmapped holes inside the range; so do we.
    std::unique_lock<std::shared_mutex> g(mu_);
    // Iterate the pages that EXIST rather than every page in [addr, end).
    // A guest can pass an enormous len; the direct-window path above is
    // bounded by the 4 GiB window, but this loop used to walk one page at
    // a time and could spin for ~2^52 iterations (host hang). Absent
    // pages already read as zero, so they need no work.
    for (auto& kv : pages_) {
        uint64_t pb = kv.first * PAGE_SIZE;
        uint64_t page_end = pb + PAGE_SIZE;
        if (page_end <= pb) continue;                 // overflow guard
        if (page_end <= start || pb >= end) continue;  // outside range
        uint64_t lo = (pb > start) ? pb : start;
        uint64_t hi = (page_end < end) ? page_end : end;
        if (hi <= lo) continue;
        std::memset(kv.second.data() + (lo - pb), 0, hi - lo);
    }
}
void Memory::add_free_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    // Merge with a previous adjacent free range.
    auto it = free_ranges_.lower_bound(addr);
    if (it != free_ranges_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == addr) {
            // Extend the previous range to cover this one.
            prev->second += size;
            addr = prev->first;
            size = prev->second;
            free_ranges_.erase(prev);
            it = free_ranges_.lower_bound(addr);
        }
    }
    // Merge with a following adjacent free range.
    if (it != free_ranges_.end() && it->first == addr + size) {
        size += it->second;
        free_ranges_.erase(it);
    }
    free_ranges_[addr] = size;
}
void Memory::remove_free_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    uint64_t lo = addr;
    uint64_t hi = addr + size;
    auto it = free_ranges_.lower_bound(addr);
    // Check if an existing free range starts before addr but overlaps.
    if (it != free_ranges_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second > addr) it = prev;
    }
    while (it != free_ranges_.end() && it->first < hi) {
        uint64_t r_lo = it->first;
        uint64_t r_hi = it->first + it->second;
        // Split the reclaimed range around the overlap.
        uint64_t left_size = (lo > r_lo) ? lo - r_lo : 0;
        uint64_t right_lo = (hi < r_hi) ? hi : r_hi;
        uint64_t right_size = (hi < r_hi) ? r_hi - hi : 0;
        uint64_t keep_lo = left_size ? r_lo : 0;
        uint64_t keep_size = left_size;
        it = free_ranges_.erase(it);
        if (keep_size) free_ranges_[keep_lo] = keep_size;
        if (right_size) free_ranges_[right_lo] = right_size;
    }
}
bool Memory::atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired) {
    check_access(addr, sizeof(uint32_t), GUEST_PROT_READ | GUEST_PROT_WRITE);
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
        4 <= DIRECT_WINDOW_SIZE - addr && (addr & 3) == 0) {
        // Page-aligned check: the entire 4-byte word must be within the
        // window (already checked above). Use an atomic CAS on the
        // underlying storage. This is safe because the window is private
        // to this process. Unaligned addrs fall through to the locked
        // slow path: std::atomic on unaligned storage is ub/torn.
        std::atomic<uint32_t>* slot =
            reinterpret_cast<std::atomic<uint32_t>*>(direct_window_ + addr);
        return slot->compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel);
    }
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t off = addr & PAGE_MASK;
    if (off + 4 <= PAGE_SIZE) {
        auto it = pages_.find(addr / PAGE_SIZE);
        if (it == pages_.end()) {
            if (expected != 0) return false;
            if (would_exceed_page_limit(1)) return false;  // OOM: CAS fails
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
        uint32_t cur;
        memcpy(&cur, it->second.data() + off, 4);
        if (cur == expected) {
            memcpy(it->second.data() + off, &desired, 4);
            return true;
        }
        return false;
    }
    // Cross-page word: assemble byte-by-byte so we never overrun the
    // first page's vector. Missing pages read as zero.
    uint32_t cur = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        uint8_t b = (it == pages_.end()) ? 0 : it->second[a & PAGE_MASK];
        cur |= static_cast<uint32_t>(b) << (i * 8);
    }
    if (cur != expected) return false;
    // Pre-check the page cap for the pages this write would create (dead
    // code today — no atomic_cas_* callers — but keep it consistent with
    // read/write so wiring LSE atomics later can't bypass the cap).
    {
        size_t missing = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t a = addr + i;
            if (pages_.find(a / PAGE_SIZE) == pages_.end()) missing++;
        }
        if (missing && would_exceed_page_limit(missing)) return false;
    }
    for (int i = 0; i < 4; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        if (it == pages_.end()) {
            it = pages_.emplace(a / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
        it->second[a & PAGE_MASK] = static_cast<uint8_t>(desired >> (i * 8));
    }
    return true;
}
bool Memory::atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired) {
    check_access(addr, sizeof(uint64_t), GUEST_PROT_READ | GUEST_PROT_WRITE);
    // See atomic_cas_32 for the direct-window rationale.
    // BUGFIX: same integer-overflow guard as atomic_cas_32, plus
    // alignment gate (unaligned atomic is ub -> slow path).
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        8 <= DIRECT_WINDOW_SIZE - addr && (addr & 7) == 0) {
        std::atomic<uint64_t>* slot =
            reinterpret_cast<std::atomic<uint64_t>*>(direct_window_ + addr);
        return slot->compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel);
    }
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t off = addr & PAGE_MASK;
    if (off + 8 <= PAGE_SIZE) {
        auto it = pages_.find(addr / PAGE_SIZE);
        if (it == pages_.end()) {
            if (expected != 0) return false;
            if (would_exceed_page_limit(1)) return false;  // OOM: CAS fails
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t cur;
        memcpy(&cur, it->second.data() + off, 8);
        if (cur == expected) {
            memcpy(it->second.data() + off, &desired, 8);
            return true;
        }
        return false;
    }
    uint64_t cur = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        uint8_t b = (it == pages_.end()) ? 0 : it->second[a & PAGE_MASK];
        cur |= static_cast<uint64_t>(b) << (i * 8);
    }
    if (cur != expected) return false;
    {
        size_t missing = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t a = addr + i;
            if (pages_.find(a / PAGE_SIZE) == pages_.end()) missing++;
        }
        if (missing && would_exceed_page_limit(missing)) return false;
    }
    for (int i = 0; i < 8; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        if (it == pages_.end()) {
            it = pages_.emplace(a / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
        it->second[a & PAGE_MASK] = static_cast<uint8_t>(desired >> (i * 8));
    }
    return true;
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
        auto append_direct_page = [&](uint64_t page_no) {
            const uint8_t flags = direct_page_flags_[page_no].load(
                std::memory_order_relaxed);
            if (!(flags & PAGE_MAPPED) || guard_pages_.count(page_no)) return;
            const uint64_t page_addr = page_no * PAGE_SIZE;
            const uint8_t prot = flags & 7;
            if (!(prot & GUEST_PROT_READ))
                (void)set_direct_prot(page_addr, page_addr + PAGE_SIZE,
                                      PROT_READ | PROT_WRITE);
            const uint8_t* page = direct_window_ + page_addr;
            out.push_back({page_addr, std::vector<uint8_t>(page, page + PAGE_SIZE)});
            if (!(prot & GUEST_PROT_READ))
                (void)set_direct_prot(page_addr, page_addr + PAGE_SIZE, prot);
        };
        for (const auto& [base, size] : allocations_) {
            if (base >= DIRECT_WINDOW_SIZE) continue;
            uint64_t end = base + size;
            if (end < base || end > DIRECT_WINDOW_SIZE) end = DIRECT_WINDOW_SIZE;
            for (uint64_t a = base & ~PAGE_MASK; a < end; a += PAGE_SIZE) {
                uint64_t pn = a / PAGE_SIZE;
                if (guard_pages_.count(pn)) continue;
                if (pn < copied.size() && !copied[pn]) {
                    append_direct_page(pn);
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
            if (guard_pages_.count(p)) continue;
            append_direct_page(p);
        }
    }
    return out;
}
std::unique_ptr<Memory> Memory::clone_for_fork() const {
    auto child = std::make_unique<Memory>(false);
    // Snapshot bytes before restoring the original protection state.
    auto snapshots = snapshot_pages();
    constexpr size_t DIRECT_PAGES = DIRECT_WINDOW_SIZE / PAGE_SIZE;
    std::vector<uint8_t> low_flags(DIRECT_PAGES, 0);
    std::map<uint64_t, HighMapping> high_mappings;
    std::unordered_map<uint64_t, uint64_t> allocations;
    std::map<uint64_t, uint64_t> free_ranges;
    uint64_t mmap_next, above_window_next, child_pie_base, child_stack_top;
    size_t child_total_pages;
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        for (size_t p = 0; p < DIRECT_PAGES; ++p)
            low_flags[p] = direct_page_flags_[p].load(std::memory_order_relaxed);
        high_mappings = high_mappings_;
        allocations = allocations_;
        free_ranges = free_ranges_;
        mmap_next = mmap_next_;
        above_window_next = above_window_next_;
        child_pie_base = pie_base_;
        child_stack_top = stack_top_;
        child_total_pages = total_pages_.load(std::memory_order_relaxed);
    }
    if (child->direct_window_)
        (void)child->set_direct_prot(0, DIRECT_WINDOW_SIZE,
                                     PROT_READ | PROT_WRITE);
    for (const auto& snap : snapshots) {
        if (child->in_direct_window(snap.addr)) {
            std::memcpy(child->direct_window_ + snap.addr,
                        snap.data.data(), snap.data.size());
        } else {
            // Write to the sparse pages_ map.
            child->map_range(snap.addr, snap.data.size());
            child->write(snap.addr, snap.data.data(), snap.data.size());
        }
    }
    // Copy allocator and permission state after page contents so RO and
    // PROT_NONE mappings are writable while the snapshot is restored.
    {
        std::unique_lock<std::shared_mutex> g(child->mu_);
        child->mmap_next_ = mmap_next;
        child->above_window_next_ = above_window_next;
        child->pie_base_ = child_pie_base;
        child->stack_top_ = child_stack_top;
        child->allocations_ = std::move(allocations);
        child->free_ranges_ = std::move(free_ranges);
        child->high_mappings_ = std::move(high_mappings);
        child->total_pages_.store(child_total_pages, std::memory_order_relaxed);
        for (size_t p = 0; p < DIRECT_PAGES; ++p)
            child->direct_page_flags_[p].store(low_flags[p],
                                                std::memory_order_relaxed);
        if (child->direct_window_) {
            size_t p = 0;
            while (p < DIRECT_PAGES) {
                const uint8_t flags = low_flags[p];
                const int host_prot = (flags & PAGE_MAPPED) ? (flags & 7) : PROT_NONE;
                size_t q = p + 1;
                while (q < DIRECT_PAGES) {
                    const uint8_t next = low_flags[q];
                    const int next_prot = (next & PAGE_MAPPED) ? (next & 7) : PROT_NONE;
                    if (next_prot != host_prot) break;
                    ++q;
                }
                (void)child->set_direct_prot(p * PAGE_SIZE, q * PAGE_SIZE, host_prot);
                p = q;
            }
        }
    }
    return child;
}
} // namespace arm64emu
