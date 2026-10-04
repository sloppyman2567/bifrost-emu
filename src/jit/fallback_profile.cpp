#include "jit/fallback_profile.hpp"
#include "debug_flags.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>
namespace arm64emu {
namespace {
struct Key {
    uint64_t pc;
    uint32_t opcode;
    int tid;
    bool operator==(const Key& other) const {
        return pc == other.pc && opcode == other.opcode && tid == other.tid;
    }
};
struct Hash {
    size_t operator()(const Key& k) const {
        return std::hash<uint64_t>{}(k.pc ^ (uint64_t(k.opcode) << 32) ^ uint32_t(k.tid));
    }
};
struct Entry {
    Key key;
    long host_tid;
    unsigned instruction_class;
    std::atomic<uint64_t> count{0};
    Entry(Key k, long host, unsigned cls) : key(k), host_tid(host), instruction_class(cls) {}
};
struct Registry {
    std::mutex mutex, output_mutex;
    std::vector<std::unique_ptr<Entry>> entries;
    Registry() { std::atexit(dump_interpreted_profile); }
};
Registry& registry() {
    // Process lifetime: host thread-local caches and exit snapshots may outlive
    // normal static destructors. Only allocated when profiling is enabled.
    static Registry* r = new Registry;
    return *r;
}
struct ThreadCache {
    long host_tid = ::syscall(SYS_gettid);
    std::unordered_map<Key, Entry*, Hash> entries;
    Entry* last = nullptr;
};
}
void record_interpreted_instruction(int tid, uint64_t pc, uint32_t op, unsigned cls) {
    thread_local ThreadCache cache;
    Key key{pc, op, tid};
    Entry* entry = cache.last;
    if (!entry || !(entry->key == key)) {
        auto found = cache.entries.find(key);
        if (found != cache.entries.end()) entry = found->second;
        else {
            auto fresh = std::make_unique<Entry>(key, cache.host_tid, cls);
            entry = fresh.get();
            auto& r = registry();
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                r.entries.push_back(std::move(fresh));
            }
            cache.entries.emplace(key, entry);
        }
        cache.last = entry;
    }
    // No global lock in the counting path. Atomic counts allow the reporter
    // to snapshot while any guest thread runs; thread exit loses no batches.
    entry->count.fetch_add(1, std::memory_order_relaxed);
}
void dump_interpreted_profile() {
    const auto& path = dbg().fallback_profile;
    if (path.empty()) return;
    auto& r = registry();
    std::lock_guard<std::mutex> writer(r.output_mutex);
    struct Row { Key key; long host; unsigned cls; uint64_t count; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        rows.reserve(r.entries.size());
        for (const auto& e : r.entries)
            rows.push_back({e->key, e->host_tid, e->instruction_class,
                            e->count.load(std::memory_order_relaxed)});
    }
    std::string temp = path + ".tmp." + std::to_string(::getpid());
    FILE* out = std::fopen(temp.c_str(), "w");
    if (!out) { std::perror("fallback profile"); return; }
    std::fprintf(out, "guest_tid\thost_tid\tpc\topcode\tclass\tcount\n");
    for (const auto& e : rows)
        std::fprintf(out, "%d\t%ld\t0x%llx\t%08x\t%u\t%llu\n", e.key.tid, e.host,
                     static_cast<unsigned long long>(e.key.pc), e.key.opcode, e.cls,
                     static_cast<unsigned long long>(e.count));
    bool good = !std::ferror(out);
    if (std::fclose(out)) good = false;
    if (good) {
        if (std::rename(temp.c_str(), path.c_str())) std::perror("fallback profile rename");
    } else std::perror("fallback profile write");
}
}
