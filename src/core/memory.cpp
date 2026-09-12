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
    // Allocate a 4 GiB direct-access window for the JIT. This is a lazy
    // mmap — Linux only allocates physical pages on first access (demand
    // paging). The window is PROT_READ|PROT_WRITE.
    void* p = mmap(nullptr, DIRECT_WINDOW_SIZE,
                   PROT_READ | PROT_WRITE,
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
void Memory::map_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t start = addr & ~PAGE_MASK;
    uint64_t end = addr + size;
    size_t missing = 0;
    for (uint64_t s = start; s < end; s += PAGE_SIZE) {
        if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
        if (pages_.find(s / PAGE_SIZE) == pages_.end()) missing++;
    }
    if (missing && would_exceed_page_limit(missing)) return;
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
            total_pages_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
// TEMP DEBUG: trace guest reads/writes in the failing memalign chunk header.
namespace {
inline bool chunk_trace_on() {
    static const bool on = getenv("BIFROST_CHUNK_TRACE") != nullptr;
    return on;
}
inline bool in_chunk_region(uint64_t addr, size_t n) {
    return addr + n > 0x500034a000ULL && addr < 0x5000350000ULL;
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
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE &&
        n <= DIRECT_WINDOW_SIZE - addr) {
        memcpy(direct_window_ + addr, src, n);
        if (chunk_trace_on() && in_chunk_region(addr, n))
            fprintf(stderr, "[chunk] DW write 0x%llx n=%zu\n", (unsigned long long)addr, n);
        return;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    uint64_t cur = addr;
    size_t remaining = n;
    while (remaining > 0) {
        uint64_t pn = cur / PAGE_SIZE;
        uint64_t off = cur & PAGE_MASK;
        size_t take = std::min<size_t>(PAGE_SIZE - off, remaining);
        if (chunk_trace_on() && in_chunk_region(cur, take))
            fprintf(stderr, "[chunk] W 0x%llx n=%zu pcache=%d\n",
                    (unsigned long long)cur, take,
                    pc && pn == pc->write_page);
        if (pc && __builtin_expect(pn == pc->write_page &&
                                    pc->write_epoch == page_epoch_.load(std::memory_order_relaxed), 1)) {
            memcpy(pc->write_ptr + off, p, take);
        } else {
            std::vector<uint8_t>* page = nullptr;
            {
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
                page = &it->second;
            }
            memcpy(page->data() + off, p, take);
            if (pc) {
                pc->write_page = pn;
                pc->write_ptr = page->data();
                pc->write_epoch = page_epoch_.load(std::memory_order_relaxed);
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
        if (chunk_trace_on() && in_chunk_region(cur, take)) {
            uint8_t tmp[8] = {0};
            size_t td = take > 8 ? 8 : take;
            if (pc && pn == pc->read_page) {
                memcpy(tmp, pc->read_ptr + off, td);
                fprintf(stderr, "[chunk] R(cached) 0x%llx n=%zu -> %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        (unsigned long long)cur, take,
                        tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7]);
            } else {
                auto it = pages_.find(pn);
                if (it != pages_.end()) {
                    memcpy(tmp, it->second.data() + off, td);
                    fprintf(stderr, "[chunk] R(page) 0x%llx n=%zu -> %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            (unsigned long long)cur, take,
                            tmp[0], tmp[1], tmp[2], tmp[3], tmp[4], tmp[5], tmp[6], tmp[7]);
                } else {
                    fprintf(stderr, "[chunk] R(alloc-zero) 0x%llx n=%zu\n", (unsigned long long)cur, take);
                }
            }
        }
        if (pc && __builtin_expect(pn == pc->read_page &&
                                   pc->read_epoch == page_epoch_.load(std::memory_order_relaxed), 1)) {
            memcpy(p, pc->read_ptr + off, take);
        } else {
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
            const std::vector<uint8_t>* page = nullptr;
            {
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
                page = &it->second;
            }
            memcpy(p, page->data() + off, take);
            if (pc) {
                pc->read_page = pn;
                pc->read_ptr = page->data();
                pc->read_epoch = page_epoch_.load(std::memory_order_relaxed);
            }
        }
        p += take;
        cur += take;
        remaining -= take;
    }
}
uint64_t Memory::mmap_alloc(uint64_t size, uint64_t hint, bool noreserve) {
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
    if (reused && direct_window_ && start < DIRECT_WINDOW_SIZE) {
        uint64_t wend = std::min(end, static_cast<uint64_t>(DIRECT_WINDOW_SIZE));
        if (wend > start) std::memset(direct_window_ + start, 0, wend - start);
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
            // Reserve the guard VA so the bump allocator never reuses it.
            if (gpage + PAGE_SIZE > mmap_next_) mmap_next_ = gpage + PAGE_SIZE;
        }
    }
    allocations_[base] = aligned_size;
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    if (alloc_trace_)
        fprintf(stderr, " [0x%llx..0x%llx)\n",
                (unsigned long long)base,
                (unsigned long long)(base + aligned_size));
    return base;
}
uint64_t Memory::mmap_fixed_replace(uint64_t addr, uint64_t size, bool noreserve,
                                    bool zero_contents) {
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
        if (wend > lo) std::memset(direct_window_ + lo, 0, wend - lo);
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
    mmap_next_ = std::max(mmap_next_, hi);
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
    }
    return base + P;  // usable base; sp starts at base + P + usable
}
uint64_t Memory::mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size) {
    if (new_size == 0) new_size = PAGE_SIZE;
    uint64_t old_aligned = (old_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t new_aligned = (new_size + PAGE_MASK) & ~PAGE_MASK;
    // Shrink: keep the same address, reclaim the tail pages + address
    // range so repeated realloc-shrink doesn't march the page cap.
    if (new_aligned <= old_aligned) {
        std::unique_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(old_addr);
        if (it != allocations_.end()) {
            uint64_t freed = it->second > new_aligned ? it->second - new_aligned : 0;
            it->second = new_aligned;
            if (freed > 0) {
                uint64_t fstart = old_addr + new_aligned;
                uint64_t fend = fstart + freed;
                for (uint64_t s = fstart; s < fend; s += PAGE_SIZE) {
                    if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
                    auto pit = pages_.find(s / PAGE_SIZE);
                    if (pit != pages_.end()) {
                        pages_.erase(pit);
                        total_pages_.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
                add_free_range(fstart, freed);
                page_epoch_.fetch_add(1, std::memory_order_relaxed);
            }
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
    // The stack region is a fixed mapping, not allocator space — refuse
    // an in-place grow that would extend into it (the caller falls back
    // to move; mmap_alloc's bump skips over the stack the same way).
    if (extra_start < stack_top_ && extra_end > stack_top_ - STACK_SIZE)
        can_grow_in_place = false;
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
        // OOM check first: count above-window pages that would be added.
        // (Window pages need no pages_ entry and don't count.)
        size_t need = 0;
        for (uint64_t s = extra_start; s < extra_end; s += PAGE_SIZE) {
            if (direct_window_ && s < DIRECT_WINDOW_SIZE) continue;
            if (pages_.find(s / PAGE_SIZE) == pages_.end()) need++;
        }
        if (would_exceed_page_limit(need)) return 0;  // keep old mapping
        // Inline the map_range logic so we keep holding the unique lock
        // (map_range would otherwise re-acquire it → deadlock).
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
            }
            // else: preserve existing page data — mremap_grow is
            // extending the mapping, not zeroing it. musl's realloc
            // expects the old data to be preserved at the start.
        }
        total_pages_.fetch_add(added, std::memory_order_relaxed);
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
        // The grown region now belongs to this allocation — remove any
        // reclaimed free range it overlaps (mremap grows into space that
        // a prior munmap may have returned to the pool).
        remove_free_range(extra_start, extra_end - extra_start);
        return old_addr;
    }
    // Collision detected: allocate a fresh region, copy the data, and
    // return the new address. We must release the unique lock here
    // because mmap_alloc/read/write all acquire it themselves.
    g.unlock();
    uint64_t new_addr = mmap_alloc(new_size, 0);
    if (new_addr == 0) return 0;  // oom: keep old mapping intact
    if (old_size > 0) {
        std::vector<uint8_t> buf(old_size);
        read(old_addr, buf.data(), old_size);
        write(new_addr, buf.data(), old_size);
    }
    // Reclaim the old range (free its pages + hand the address space
    // back to the free list) instead of just erasing the tracking entry.
    untrack_allocation(old_addr, old_aligned);
    return new_addr;
}
void Memory::untrack_allocation(uint64_t addr, uint64_t size) {
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
    const uint64_t hi = (addr + size + PAGE_MASK) & ~PAGE_MASK;
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
    // Any pages_ erase invalidates cached raw pointers held by per-cpu
    // PageCache entries. Bump the epoch so stale entries miss instead
    // of uaf into freed vector storage.
    page_epoch_.fetch_add(1, std::memory_order_relaxed);
}
void Memory::madvise_dontneed(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    // Saturate the end against overflow (cf. is_mapped's end guard).
    uint64_t end = addr + len;
    if (end < addr) end = UINT64_MAX;
    // Direct-window part: the window IS the storage — memset zeros.
    // Skip PROT_NONE callback-stack guard pages (host would SIGSEGV).
    if (direct_window_ && addr < DIRECT_WINDOW_SIZE) {
        uint64_t wend = std::min(end, static_cast<uint64_t>(DIRECT_WINDOW_SIZE));
        {
            std::shared_lock<std::shared_mutex> g(mu_);
            for (uint64_t s = addr & ~PAGE_MASK; s < wend; s += PAGE_SIZE) {
                if (guard_pages_.count(s / PAGE_SIZE)) continue;
                uint64_t lo = (s > addr) ? s : addr;
                uint64_t hi = std::min(s + PAGE_SIZE, wend);
                if (hi > lo) std::memset(direct_window_ + lo, 0, hi - lo);
            }
        }
        if (end <= DIRECT_WINDOW_SIZE) return;
        addr = DIRECT_WINDOW_SIZE;
    }
    // Sparse pages_: zero covered bytes in place. Never erase entries —
    // a cached PageCache pointer into an erased vector would dangle
    // (same reason untrack's page reclaim is munmap-only). Absent pages
    // already read as zero (demand paging), so nothing to do for them.
    // Linux ignores unmapped holes inside the range; so do we.
    std::unique_lock<std::shared_mutex> g(mu_);
    for (uint64_t pb = addr & ~PAGE_MASK; pb < end; pb += PAGE_SIZE) {
        uint64_t page_end = pb + PAGE_SIZE;
        if (page_end <= pb) break;  // overflow guard (pb near UINT64_MAX)
        uint64_t lo = (pb > addr) ? pb : addr;
        uint64_t hi = (page_end < end) ? page_end : end;
        if (hi <= lo) continue;
        auto it = pages_.find(pb / PAGE_SIZE);
        if (it != pages_.end())
            std::memset(it->second.data() + (lo - pb), 0, hi - lo);
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
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
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
    for (int i = 0; i < 4; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        if (it == pages_.end()) {
            it = pages_.emplace(a / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
        }
        it->second[a & PAGE_MASK] = static_cast<uint8_t>(desired >> (i * 8));
    }
    return true;
}
bool Memory::atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired) {
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
            it = pages_.emplace(addr / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
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
    for (int i = 0; i < 8; i++) {
        uint64_t a = addr + i;
        auto it = pages_.find(a / PAGE_SIZE);
        if (it == pages_.end()) {
            it = pages_.emplace(a / PAGE_SIZE,
                                std::vector<uint8_t>(PAGE_SIZE, 0)).first;
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
        for (const auto& [base, size] : allocations_) {
            if (base >= DIRECT_WINDOW_SIZE) continue;
            uint64_t end = base + size;
            if (end < base || end > DIRECT_WINDOW_SIZE) end = DIRECT_WINDOW_SIZE;
            for (uint64_t a = base & ~PAGE_MASK; a < end; a += PAGE_SIZE) {
                uint64_t pn = a / PAGE_SIZE;
                if (guard_pages_.count(pn)) continue;
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
            if (guard_pages_.count(p)) continue;
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
    auto child = std::make_unique<Memory>(false);
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
    // free_ranges_ is copied too (same address-space reuse). guard_pages_
    // is intentionally NOT copied: the child gets a fresh RW window with
    // no mprotect'd guards, so those pages are readable there.
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        child->mmap_next_ = mmap_next_;
        child->above_window_next_ = above_window_next_;
        child->pie_base_ = pie_base_;
        child->stack_top_ = stack_top_;
        for (const auto& [base, size] : allocations_) {
            child->allocations_[base] = size;
        }
        // BUGFIX: copy the reclaimed free ranges too, so the child's
        // mmap_alloc reuses the same address space the parent would
        // (keeps the child heap inside the direct window after fork).
        child->free_ranges_ = free_ranges_;
        // 1.5.4-alpha: copy total page count for OOM tracking.
        child->total_pages_.store(total_pages_.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
    return child;
}
} // namespace arm64emu
