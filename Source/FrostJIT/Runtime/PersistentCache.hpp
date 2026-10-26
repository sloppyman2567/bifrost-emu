// PersistentCache.hpp — the experimental cross-launch store of Tier-1
// translations (BIFROST_JIT_DISK_CACHE=1, or `verify` to recompile and
// compare every hit).
//
// Records are opaque to this store: a checksummed header that names the
// translated code by the file it came from (Memory::code_origin), and a body
// that CacheRecording.cpp writes and relocates. One directory per
// application and codegen fingerprint holds a compacted main.bin, with a
// sorted index read in place, and the segments that processes append to
// while they run. Once the segments are large enough, the next process to
// start merges them in the background, keeping the most recently used
// records within the application's budget (BIFROST_JIT_DISK_CACHE_MB,
// default 512).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace arm64emu {

struct CacheRecordHeader {
    static constexpr uint32_t kMagic = 0x31524a42;  // "BJR1"
    uint32_t magic;
    uint32_t size;          // whole record, a multiple of 8
    uint64_t file_key;      // Memory::CodeOrigin
    uint64_t file_offset;
    uint64_t start_pc;      // guest address when recorded
    uint32_t checksum;      // FNV-1a over the record with this field zero
    uint32_t generation;    // compaction generation of its last use
    uint8_t  mt;            // multithreaded-mode translation
    uint8_t  kind;          // CacheRecordKind
    uint16_t reserved;
    uint32_t reserved2;
};
static_assert(sizeof(CacheRecordHeader) == 48);
// A touch record's body lists records used since the last compaction.
enum CacheRecordKind : uint8_t { kRecordNative = 0, kRecordTouch = 2 };

class PersistentCache {
public:
    enum class Mode { Off, On, Verify };
    static Mode configured_mode();  // BIFROST_JIT_DISK_CACHE
    // The process's cache for a codegen fingerprint, opened on first use;
    // nullptr when the cache is off or its directory cannot be used.
    // `program` names the cache directory unless BIFROST_JIT_CACHE_APP does.
    static PersistentCache* open(uint64_t fingerprint_hi, uint64_t fingerprint_lo, const std::string& program);
    Mode mode() const { return mode_; }
    // The newest stored record for this code, or nullptr. Its checksum is
    // checked by valid(), only for records that are actually used.
    const CacheRecordHeader* find(uint64_t file_key, uint64_t file_offset, bool mt) const;
    static bool valid(const CacheRecordHeader* record);
    // Appends a record (header completed here: magic, size, checksum).
    void submit(std::vector<uint8_t> record);
    // A stored record was used: keep it through the next compaction.
    void note_use(const CacheRecordHeader* used);
    void flush();
    static void flush_all();  // exit paths
    std::atomic<uint64_t> hits{0}, misses{0}, rejected{0}, recorded{0}, unrecordable{0};
    std::atomic<uint64_t> verify_mismatches{0}, verify_other_shape{0};
    std::atomic<uint64_t> recorded_bytes{0}, dropped_over_budget{0};
    // Why blocks were not recorded (CacheRecording.cpp's RecordFailure).
    static constexpr int kFailureKinds = 12;
    std::atomic<uint64_t> unrecordable_by[kFailureKinds] = {};
    void dump_stats() const;
    static uint32_t checksum(const uint8_t* record, size_t size);

private:
    PersistentCache() = default;
    bool init(uint64_t fingerprint_hi, uint64_t fingerprint_lo, const std::string& program);
    void load_index();
    void compact();
    void writer_loop();
    void start_writer_locked();
    bool open_segment_locked();
    void append_touches_locked();
    bool would_exceed_budget_locked(size_t bytes) const;
    static void after_fork_child();
    struct IndexEntry { uint64_t key; uint64_t offset; };            // main.bin's table
    struct SegmentEntry { uint64_t key; const CacheRecordHeader* record; };
    static uint64_t index_key(uint64_t file_key, uint64_t file_offset, bool mt);
    Mode mode_ = Mode::Off;
    uint64_t fp_hi_ = 0, fp_lo_ = 0;
    std::string app_dir_, dir_;
    uint64_t budget_ = 512ull << 20;
    // The application's bytes on disk besides this process's segment (mu_):
    // measured at open and again after each merge this process runs.
    uint64_t usage_base_ = 0;
    // main.bin, mapped for the process's lifetime, and its index.
    const uint8_t* main_ = nullptr;
    size_t main_size_ = 0;
    const IndexEntry* main_index_ = nullptr;
    size_t main_count_ = 0;
    uint32_t main_generation_ = 0;
    // Records in segments not yet merged, newest per key; they win over main.bin.
    std::vector<SegmentEntry> segment_index_;
    uint64_t segment_bytes_ = 0;
    // Writer: records queue here and a background thread appends them to
    // this process's segment, which it holds with a shared flock.
    std::mutex mu_;
    std::condition_variable wake_;
    std::vector<uint8_t> pending_;
    std::vector<uint8_t> pending_touches_;  // 24-byte entries
    std::thread writer_;
    bool writer_started_ = false, stop_ = false;
    int segment_fd_ = -1;
    uint64_t written_ = 0;
    int pid_ = 0;
};
} // namespace arm64emu
