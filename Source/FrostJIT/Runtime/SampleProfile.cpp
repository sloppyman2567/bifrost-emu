// SampleProfile.cpp — see SampleProfile.hpp.
//
// The signal handler only stores a host pc into the calling thread's ring. Everything else runs outside the handler,
// under one mutex: rings are drained into running totals (so a long run is not limited by the ring), and the blocks
// registered by the translator are searched by address. The state is never destroyed, because a JIT can be destroyed
// during process exit, after other statics are gone.
#include "FrostJIT/Runtime/SampleProfile.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <sys/syscall.h>
#include <unistd.h>

namespace arm64emu {
namespace {

constexpr size_t RING = 16384;  // samples per thread (two and a half minutes at 100 Hz); a power of two
constexpr int MAX_THREADS = 256;
static_assert((RING & (RING - 1)) == 0, "ring size must be a power of two");

struct Ring {
    std::atomic<uint32_t> head{0};  // written by the thread's signal handler, read when draining
    uint64_t rips[RING];
    // Below: used with State::mutex held only.
    uint32_t read = 0;
    int host_tid = 0;
    bool in_use = false;
};

struct ThreadTotals {
    ProfSectionCounts counts;
    bool exited = false;
};

constexpr size_t MAX_RANGES = 16u << 20;  // 256 MiB of table: a game translates hundreds of thousands of blocks

struct StoredRange {
    uint64_t begin;
    uint32_t length;
    uint8_t op;
    bool spill;
};

struct State {
    std::mutex mutex;
    std::vector<StoredRange> ranges;
    bool ranges_sorted = true;
    uint64_t body_op[PROF_OP_KINDS] = {};
    uint64_t body_spill = 0;
    uint64_t ranges_dropped = 0;
    Ring rings[MAX_THREADS];  // static storage: a thread that exits leaves nothing dangling
    std::vector<ProfBlock> blocks;
    bool sorted = true;
    ProfSectionCounts total;
    uint64_t lost = 0;
    std::atomic<uint64_t> unclaimed{0};  // samples from a thread that has no ring
    std::unordered_map<uint64_t, ProfSectionCounts> by_guest_pc;
    std::unordered_map<int, ThreadTotals> by_thread;
};

State& state() {
    static State* s = new State;  // never destroyed
    return *s;
}

thread_local int tls_slot = -1;  // a plain int: the signal handler reads it
thread_local bool tls_claimed = false;

void sort_blocks_locked(State& s) {
    if (!s.sorted) {
        std::sort(s.blocks.begin(), s.blocks.end(), [](const ProfBlock& a, const ProfBlock& b) { return a.begin < b.begin; });
        s.sorted = true;
    }
    if (!s.ranges_sorted) {
        std::sort(s.ranges.begin(), s.ranges.end(), [](const StoredRange& a, const StoredRange& b) { return a.begin < b.begin; });
        s.ranges_sorted = true;
    }
}

// A body sample: what the operation whose code it fell in does, or PROF_OTHER between operations.
void classify_body_locked(State& s, uint64_t rip) {
    const auto above = std::upper_bound(s.ranges.begin(), s.ranges.end(), rip,
                                        [](uint64_t value, const StoredRange& r) { return value < r.begin; });
    if (above == s.ranges.begin() || rip >= (above - 1)->begin + (above - 1)->length) {
        ++s.body_op[PROF_OTHER];
        return;
    }
    ++s.body_op[(above - 1)->op];
    s.body_spill += (above - 1)->spill;
}

// Adds one sample to the totals of its thread and, inside a registered block, of that block's guest pc.
void classify_locked(State& s, uint64_t rip, ThreadTotals& thread) {
    const auto above = std::upper_bound(s.blocks.begin(), s.blocks.end(), rip,
                                        [](uint64_t value, const ProfBlock& b) { return value < b.begin; });
    ProfSectionCounts one;
    if (above == s.blocks.begin() || rip >= (above - 1)->end) {
        one.shared = 1;
    } else {
        const ProfBlock& b = *(above - 1);
        (rip < b.body ? one.entry : rip < b.exit ? one.body : rip < b.cold ? one.exit : one.cold) = 1;
        if (one.body) classify_body_locked(s, rip);
        s.by_guest_pc[b.guest_pc] += one;
    }
    s.total += one;
    thread.counts += one;
}

void drain_locked(State& s, int slot) {
    Ring& ring = s.rings[slot];
    sort_blocks_locked(s);
    const uint32_t head = ring.head.load(std::memory_order_acquire);
    uint32_t pending = head - ring.read;  // wraps correctly
    if (pending > RING) {
        s.lost += pending - RING;
        ring.read = head - RING;
        pending = RING;
    }
    ThreadTotals& thread = s.by_thread[ring.host_tid];
    for (uint32_t i = 0; i < pending; ++i) classify_locked(s, ring.rips[(ring.read + i) & (RING - 1)], thread);
    ring.read = head;
}

void drain_all_locked(State& s) {
    for (int i = 0; i < MAX_THREADS; ++i)
        if (s.rings[i].in_use) drain_locked(s, i);
}

// Runs when a thread ends: its last samples are resolved, and the ring goes to the next thread.
struct SlotOwner {
    ~SlotOwner() {
        const int slot = tls_slot;
        if (slot < 0) return;
        tls_slot = -1;  // before draining: a late signal must not write to a ring that is being released
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        drain_locked(s, slot);
        s.by_thread[s.rings[slot].host_tid].exited = true;
        s.rings[slot].in_use = false;
    }
};

double percent(uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; }

void print_counts(std::FILE* out, const ProfSectionCounts& c) {
    const uint64_t n = c.total();
    std::fprintf(out, "entry %.1f%% | body %.1f%% | exit %.1f%% | cold %.1f%% | shared stubs %.1f%%",
                 percent(c.entry, n), percent(c.body, n), percent(c.exit, n), percent(c.cold, n), percent(c.shared, n));
}

}  // namespace

void prof_note_block(const ProfBlock& block) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.blocks.empty() && block.begin < s.blocks.back().begin) s.sorted = false;
    s.blocks.push_back(block);
}

void prof_note_ranges(const ProfRange* ranges, size_t count) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (size_t i = 0; i < count; ++i) {
        if (s.ranges.size() >= MAX_RANGES) {
            s.ranges_dropped += count - i;
            return;
        }
        if (ranges[i].end <= ranges[i].begin) continue;
        if (!s.ranges.empty() && ranges[i].begin < s.ranges.back().begin) s.ranges_sorted = false;
        s.ranges.push_back({ranges[i].begin, uint32_t(ranges[i].end - ranges[i].begin), ranges[i].op, ranges[i].spill});
    }
}

void prof_release_code(const uint8_t* base, size_t size) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    drain_all_locked(s);  // samples taken in this buffer are resolved while its blocks are still known
    const uint64_t first = reinterpret_cast<uint64_t>(base), last = first + size;
    s.blocks.erase(std::remove_if(s.blocks.begin(), s.blocks.end(),
                                  [&](const ProfBlock& b) { return b.begin >= first && b.begin < last; }),
                   s.blocks.end());
    s.ranges.erase(std::remove_if(s.ranges.begin(), s.ranges.end(),
                                  [&](const StoredRange& r) { return r.begin >= first && r.begin < last; }),
                   s.ranges.end());
}

void prof_claim_thread() {
    if (tls_claimed) return;
    tls_claimed = true;
    static thread_local SlotOwner owner;  // gives the ring back when the thread ends
    (void)owner;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (int i = 0; i < MAX_THREADS; ++i) {
        Ring& ring = s.rings[i];
        if (ring.in_use) continue;
        ring.in_use = true;
        ring.read = ring.head.load(std::memory_order_relaxed);
        ring.host_tid = static_cast<int>(syscall(SYS_gettid));
        s.by_thread.try_emplace(ring.host_tid);
        tls_slot = i;
        return;
    }
}

void prof_record_sample(uint64_t rip) {
    const int slot = tls_slot;
    if (slot < 0) {
        state().unclaimed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Ring& ring = state().rings[slot];
    const uint32_t head = ring.head.load(std::memory_order_relaxed);
    ring.rips[head & (RING - 1)] = rip;
    ring.head.store(head + 1, std::memory_order_release);
}

ProfReport prof_collect(size_t top_blocks) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    drain_all_locked(s);
    ProfReport report;
    report.total = s.total;
    report.lost = s.lost + s.unclaimed.load(std::memory_order_relaxed);
    std::copy(std::begin(s.body_op), std::end(s.body_op), std::begin(report.body_op));
    report.body_spill = s.body_spill;
    report.ranges_dropped = s.ranges_dropped;
    for (const auto& [tid, totals] : s.by_thread)
        if (totals.counts.total()) report.threads.push_back({tid, totals.exited, totals.counts});
    std::sort(report.threads.begin(), report.threads.end(),
              [](const ProfThreadReport& a, const ProfThreadReport& b) { return a.counts.total() > b.counts.total(); });
    if (top_blocks) {
        for (const auto& [pc, counts] : s.by_guest_pc) report.hottest.push_back({pc, counts});
        const size_t keep = std::min(top_blocks, report.hottest.size());
        std::partial_sort(report.hottest.begin(), report.hottest.begin() + keep, report.hottest.end(),
                          [](const ProfBlockReport& a, const ProfBlockReport& b) {
                              return a.counts.total() > b.counts.total();
                          });
        report.hottest.resize(keep);
    }
    return report;
}

void prof_print(std::FILE* out, const ProfReport& report, bool list_blocks) {
    const uint64_t n = report.total.total();
    if (!n && !report.lost) return;
    std::fprintf(out, "[%s] JIT samples: %llu on %zu thread(s), %llu lost: ", CODENAME, (unsigned long long)n,
                 report.threads.size(), (unsigned long long)report.lost);
    print_counts(out, report.total);
    std::fputc('\n', out);
    constexpr size_t SHOWN = 8;  // the busiest threads; a game can have a hundred
    for (size_t i = 0; i < report.threads.size() && i < SHOWN; ++i) {
        const ProfThreadReport& t = report.threads[i];
        std::fprintf(out, "[%s]   thread %d%s: %llu samples: ", CODENAME, t.host_tid, t.exited ? " (exited)" : "",
                     (unsigned long long)t.counts.total());
        print_counts(out, t.counts);
        std::fputc('\n', out);
    }
    if (report.threads.size() > SHOWN)
        std::fprintf(out, "[%s]   and %zu more threads\n", CODENAME, report.threads.size() - SHOWN);
    uint64_t body = 0;
    for (uint64_t n : report.body_op) body += n;
    if (body) {
        static const char* const names[PROF_OP_KINDS] = {"guest state moves", "memory (with guards)", "integer ALU", "flag setting",
                                                         "branches", "helper calls", "FP and SIMD", "between operations"};
        std::fprintf(out, "[%s] body time by operation:", CODENAME);
        for (int i = 0; i < PROF_OP_KINDS; ++i)
            if (report.body_op[i]) std::fprintf(out, " %s %.1f%% |", names[i], percent(report.body_op[i], body));
        std::fprintf(out, " in code with spills or reloads %.1f%%", percent(report.body_spill, body));
        if (report.ranges_dropped) std::fprintf(out, " (range table full: %llu dropped)", (unsigned long long)report.ranges_dropped);
        std::fputc('\n', out);
    }
    if (!list_blocks) return;
    std::fprintf(out, "[%s] hottest guest blocks (map a pc to a function with aarch64 objdump):\n", CODENAME);
    for (const ProfBlockReport& b : report.hottest) {
        std::fprintf(out, "  pc=0x%016llx %llu samples: ", (unsigned long long)b.guest_pc, (unsigned long long)b.counts.total());
        print_counts(out, b.counts);
        std::fputc('\n', out);
    }
}

}  // namespace arm64emu
