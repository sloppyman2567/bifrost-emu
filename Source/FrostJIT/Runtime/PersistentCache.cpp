// PersistentCache.cpp — storage of the experimental cross-launch JIT cache.
//
// <cache>/<app>/<fingerprint>/main.bin holds compacted records followed by a
// key-sorted index that lookups binary-search in place. Each process appends
// what it records (and batched touches of records it used) to its own
// seg-<pid>-<n>.bin, holding a shared flock on it while it lives. A starting
// process scans only the segments; once they reach kCompactBytes or an
// eighth of main.bin it merges the ones no process holds (a process locks
// its segment before writing the header) into a new main.bin
// in the background (one compactor at a time: an exclusive flock on `lock`).
// Merging keeps the newest record per code location and evicts by
// generation (the last compaction that saw a record used) to stay within the
// application's budget; directories of other fingerprints (older builds) are
// removed first, when a process opens the cache and after a merge. Record
// checksums are checked when a record is used (valid) and when it is merged.
// The cache stays off unless its directories belong to the user, are
// writable by nobody else and support flock.
#include "FrostJIT/Runtime/PersistentCache.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
namespace arm64emu {
namespace {
struct CacheFileHeader {
    static constexpr uint32_t kMagic = 0x31434a42;  // "BJC1"
    static constexpr uint32_t kVersion = 3;
    uint32_t magic, version;
    uint64_t fingerprint_hi, fingerprint_lo;
    uint32_t generation, reserved;
    uint64_t index_offset, index_count;  // main.bin only
};
static_assert(sizeof(CacheFileHeader) == 48);
struct TouchEntry { uint64_t file_key, file_offset; uint8_t mt, pad[7]; };
static_assert(sizeof(TouchEntry) == 24);
constexpr uint64_t kCompactBytes = 4ull << 20;

std::mutex g_open_mu;
PersistentCache* g_cache = nullptr;
bool g_open_failed = false;
uint64_t g_fp_hi = 0, g_fp_lo = 0;
std::mutex g_io_mu;  // serializes segment writes; held across fork
// A segment smaller than its header may be one a process has just created
// and not yet locked; only one this old is abandoned.
constexpr int64_t kAbandonedSegmentNs = 60ll * 1000000000;

std::string sanitize(const std::string& name) {
    std::string out;
    for (char c : name) out += (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') ? c : '_';
    if (out.size() > 96) out.resize(96);
    return out.empty() ? std::string("app") : out;
}
bool make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); ++at) {
        if (at != path.size() && path[at] != '/') continue;
        const std::string part = path.substr(0, at);
        if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}
bool write_all(int fd, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= static_cast<size_t>(n);
    }
    return true;
}
uint64_t tree_bytes(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return 0;
    uint64_t total = 0;
    while (dirent* e = readdir(dir)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string child = path + "/" + e->d_name;
        struct stat st{};
        if (lstat(child.c_str(), &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? tree_bytes(child) : static_cast<uint64_t>(st.st_size);
    }
    closedir(dir);
    return total;
}
void remove_tree(const std::string& path) {
    if (DIR* dir = opendir(path.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            const std::string child = path + "/" + e->d_name;
            struct stat st{};
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child.c_str());
        }
        closedir(dir);
    }
    rmdir(path.c_str());
}
// The cache holds code this process executes, so every directory on the way
// to it must be the user's own and writable by nobody else. A symlinked base
// (a cache on another disk) is judged by the directory it names.
bool private_dir(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
           !(st.st_mode & (S_IWGRP | S_IWOTH));
}
bool header_matches(const uint8_t* data, size_t size, uint64_t fp_hi, uint64_t fp_lo) {
    if (size < sizeof(CacheFileHeader)) return false;
    const auto* h = reinterpret_cast<const CacheFileHeader*>(data);
    return h->magic == CacheFileHeader::kMagic && h->version == CacheFileHeader::kVersion &&
           h->fingerprint_hi == fp_hi && h->fingerprint_lo == fp_lo;
}
// Records in [begin, end) of a mapped file, in order, until the first that
// is malformed (a crash mid-append, a damaged file). `checked` also
// verifies checksums; lookups check them only for records they use.
template <class F> void for_each_record(const uint8_t* data, size_t begin, size_t end, bool checked, F&& visit) {
    size_t at = begin;
    while (at + sizeof(CacheRecordHeader) <= end) {
        const auto* r = reinterpret_cast<const CacheRecordHeader*>(data + at);
        if (r->magic != CacheRecordHeader::kMagic || r->size < sizeof(CacheRecordHeader) || r->size % 8 ||
            r->size > end - at || (checked && !PersistentCache::valid(r)))
            return;
        visit(r);
        at += r->size;
    }
}
struct Mapped {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int fd = -1;
};
Mapped map_readonly(const std::string& path, int lock_mode) {
    Mapped m;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return m;
    if (lock_mode && flock(fd, lock_mode) != 0) { ::close(fd); return m; }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_size < static_cast<off_t>(sizeof(CacheFileHeader))) { ::close(fd); return m; }
    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return m; }
    m.data = static_cast<const uint8_t*>(p);
    m.size = static_cast<size_t>(st.st_size);
    m.fd = fd;
    return m;
}
// Removes the directories of other codegen fingerprints (older builds),
// oldest first, until the application uses at most `limit` bytes.
void prune_other_fingerprints(const std::string& app_dir, const std::string& own_dir, uint64_t limit) {
    if (tree_bytes(app_dir) <= limit) return;
    std::vector<std::pair<int64_t, std::string>> others;
    if (DIR* dir = opendir(app_dir.c_str())) {
        while (dirent* e = readdir(dir)) {
            const std::string path = app_dir + "/" + e->d_name;
            if (e->d_name[0] == '.' || path == own_dir) continue;
            struct stat st{};
            if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                others.emplace_back(static_cast<int64_t>(st.st_mtim.tv_sec), path);
        }
        closedir(dir);
    }
    std::sort(others.begin(), others.end());
    for (const auto& o : others) {
        if (tree_bytes(app_dir) <= limit) break;
        remove_tree(o.second);
    }
}
std::vector<std::pair<int64_t, std::string>> segments_by_age(const std::string& dir_path) {
    std::vector<std::pair<int64_t, std::string>> out;
    if (DIR* dir = opendir(dir_path.c_str())) {
        while (dirent* e = readdir(dir)) {
            const std::string name = e->d_name;
            if (name.rfind("seg-", 0) != 0 || name.size() < 9 || name.compare(name.size() - 4, 4, ".bin")) continue;
            struct stat st{};
            if (stat((dir_path + "/" + name).c_str(), &st) == 0)
                out.emplace_back(static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec, name);
        }
        closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

PersistentCache::Mode PersistentCache::configured_mode() {
    static const Mode mode = [] {
        const char* v = getenv("BIFROST_JIT_DISK_CACHE");
        if (!v || !*v) return Mode::Off;
        if (!strcmp(v, "verify")) return Mode::Verify;
        if (!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false")) return Mode::Off;
        return Mode::On;
    }();
    return mode;
}

uint32_t PersistentCache::checksum(const uint8_t* record, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        uint8_t b = record[i];
        if (i >= offsetof(CacheRecordHeader, checksum) && i < offsetof(CacheRecordHeader, checksum) + 4) b = 0;
        h = (h ^ b) * 16777619u;
    }
    return h;
}
bool PersistentCache::valid(const CacheRecordHeader* r) {
    return checksum(reinterpret_cast<const uint8_t*>(r), r->size) == r->checksum;
}

uint64_t PersistentCache::index_key(uint64_t file_key, uint64_t file_offset, bool mt) {
    uint64_t k = file_key ^ (file_offset * 0x9e3779b97f4a7c15ULL) ^ (mt ? 0x5bd1e995ULL : 0);
    k ^= k >> 31; k *= 0xbf58476d1ce4e5b9ULL; k ^= k >> 29;
    return k;
}

PersistentCache* PersistentCache::open(uint64_t fp_hi, uint64_t fp_lo, const std::string& program) {
    if (configured_mode() == Mode::Off) return nullptr;
    std::lock_guard<std::mutex> lock(g_open_mu);
    if (g_cache) return g_fp_hi == fp_hi && g_fp_lo == fp_lo ? g_cache : nullptr;
    if (g_open_failed) return nullptr;
    auto* cache = new PersistentCache();  // lives with the process (exit flush, forks)
    if (!cache->init(fp_hi, fp_lo, program)) {
        delete cache;
        g_open_failed = true;
        return nullptr;
    }
    g_fp_hi = fp_hi; g_fp_lo = fp_lo;
    g_cache = cache;
    std::atexit(flush_all);
    pthread_atfork([] { g_io_mu.lock(); if (g_cache) g_cache->mu_.lock(); },
                   [] { if (g_cache) g_cache->mu_.unlock(); g_io_mu.unlock(); },
                   [] { if (g_cache) { g_cache->mu_.unlock(); after_fork_child(); } g_io_mu.unlock(); });
    return cache;
}

bool PersistentCache::init(uint64_t fp_hi, uint64_t fp_lo, const std::string& program) {
    mode_ = configured_mode();
    fp_hi_ = fp_hi; fp_lo_ = fp_lo;
    std::string base;
    if (const char* d = getenv("BIFROST_JIT_DISK_CACHE_DIR"); d && d[0] == '/') base = d;
    else if (const char* x = getenv("XDG_CACHE_HOME"); x && x[0] == '/') base = std::string(x) + "/bifrost/jit";
    else if (const char* h = getenv("HOME"); h && h[0] == '/') base = std::string(h) + "/.cache/bifrost/jit";
    else return false;
    if (const char* mb = getenv("BIFROST_JIT_DISK_CACHE_MB")) {
        const unsigned long long v = strtoull(mb, nullptr, 10);
        if (v >= 1 && v <= 1u << 20) budget_ = v << 20;
    }
    // Finer than a test fixture's whole recording, which is under 1 MiB.
    if (const char* kb = getenv("BIFROST_JIT_DISK_CACHE_KB")) {
        const unsigned long long v = strtoull(kb, nullptr, 10);
        if (v >= 1 && v <= 1u << 30) budget_ = v << 10;
    }
    // The application: the Android package (Asgard sets it for every process
    // of a session), else the guest program's name and a hash of its path.
    std::string app;
    if (const char* a = getenv("BIFROST_JIT_CACHE_APP"); a && *a) app = a;
    else if (!program.empty()) {
        uint32_t h = 2166136261u;
        for (unsigned char c : program) h = (h ^ c) * 16777619u;
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "-%08x", h);
        app = program.substr(program.find_last_of('/') + 1) + suffix;
    } else app = "default";
    app_dir_ = base + "/" + sanitize(app);
    char fp[40];
    snprintf(fp, sizeof(fp), "%016llx%016llx", (unsigned long long)fp_hi, (unsigned long long)fp_lo);
    dir_ = app_dir_ + "/" + fp;
    if (!make_dirs(dir_)) {
        fprintf(stderr, "[JIT] disk cache: cannot create %s: %s\n", dir_.c_str(), strerror(errno));
        return false;
    }
    for (const std::string* path : {&base, &app_dir_, &dir_}) {
        if (private_dir(*path)) continue;
        fprintf(stderr, "[JIT] disk cache: off (%s must be owned by this user and not writable by others)\n",
                path->c_str());
        return false;
    }
    // Segments and merging rely on flock; some network filesystems lack it.
    {
        const int fd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const bool locks = fd >= 0 && (flock(fd, LOCK_SH | LOCK_NB) == 0 || errno == EWOULDBLOCK);
        if (fd >= 0) ::close(fd);
        if (!locks) {
            fprintf(stderr, "[JIT] disk cache: off (no file locking in %s)\n", dir_.c_str());
            return false;
        }
    }
    pid_ = getpid();
    // Other builds' directories go before this process records anything:
    // left until the background merge, they would count against the budget
    // and drop what this run records. Like a merge, leave a quarter free.
    prune_other_fingerprints(app_dir_, dir_, budget_ - budget_ / 4);
    usage_base_ = tree_bytes(app_dir_);
    load_index();
    // A process that exits mid-merge wastes it, so only processes that run a
    // while (games, not each command a script starts) merge.
    if (segment_bytes_ >= kCompactBytes || (segment_bytes_ && segment_bytes_ >= main_size_ / 8) ||
        usage_base_ > budget_) {
        const char* delay = getenv("BIFROST_JIT_DISK_CACHE_COMPACT_DELAY");
        const unsigned seconds = delay ? static_cast<unsigned>(strtoul(delay, nullptr, 10)) : 15;
        std::thread([this, seconds] { if (seconds) sleep(seconds); compact(); }).detach();
    }
    return true;
}

void PersistentCache::load_index() {
    Mapped main = map_readonly(dir_ + "/main.bin", 0);
    if (main.data) ::close(main.fd);
    if (main.data && header_matches(main.data, main.size, fp_hi_, fp_lo_)) {
        const auto* h = reinterpret_cast<const CacheFileHeader*>(main.data);
        if (h->index_offset >= sizeof(CacheFileHeader) && h->index_offset <= main.size &&
            h->index_count <= (main.size - h->index_offset) / sizeof(IndexEntry)) {
            main_ = main.data;
            main_size_ = main.size;
            main_index_ = reinterpret_cast<const IndexEntry*>(main.data + h->index_offset);
            main_count_ = h->index_count;
            main_generation_ = h->generation;
        }
    } else if (main.data) {
        munmap(const_cast<uint8_t*>(main.data), main.size);
    }
    std::vector<std::pair<SegmentEntry, size_t>> entries;  // with arrival order: later wins
    for (const auto& s : segments_by_age(dir_)) {
        Mapped m = map_readonly(dir_ + "/" + s.second, 0);
        if (!m.data) continue;
        ::close(m.fd);
        if (!header_matches(m.data, m.size, fp_hi_, fp_lo_)) { munmap(const_cast<uint8_t*>(m.data), m.size); continue; }
        segment_bytes_ += m.size;  // stays mapped: the index points into it
        for_each_record(m.data, sizeof(CacheFileHeader), m.size, false, [&](const CacheRecordHeader* r) {
            if (r->kind == kRecordNative)
                entries.push_back({{index_key(r->file_key, r->file_offset, r->mt != 0), r}, entries.size()});
        });
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.first.key != b.first.key ? a.first.key < b.first.key : a.second < b.second;
    });
    for (size_t i = 0; i < entries.size(); ++i)
        if (i + 1 == entries.size() || entries[i + 1].first.key != entries[i].first.key)
            segment_index_.push_back(entries[i].first);
}

const CacheRecordHeader* PersistentCache::find(uint64_t file_key, uint64_t file_offset, bool mt) const {
    const uint64_t key = index_key(file_key, file_offset, mt);
    const CacheRecordHeader* r = nullptr;
    auto seg = std::lower_bound(segment_index_.begin(), segment_index_.end(), key,
                                [](const SegmentEntry& e, uint64_t k) { return e.key < k; });
    if (seg != segment_index_.end() && seg->key == key) r = seg->record;
    else if (main_count_) {
        const IndexEntry* e = std::lower_bound(main_index_, main_index_ + main_count_, key,
                                               [](const IndexEntry& x, uint64_t k) { return x.key < k; });
        if (e != main_index_ + main_count_ && e->key == key &&
            e->offset >= sizeof(CacheFileHeader) && e->offset + sizeof(CacheRecordHeader) <= main_size_) {
            const auto* candidate = reinterpret_cast<const CacheRecordHeader*>(main_ + e->offset);
            if (candidate->size >= sizeof(CacheRecordHeader) && candidate->size <= main_size_ - e->offset)
                r = candidate;
        }
    }
    if (!r || r->file_key != file_key || r->file_offset != file_offset || (r->mt != 0) != mt) return nullptr;
    return r;
}

void PersistentCache::submit(std::vector<uint8_t> record) {
    if (record.size() < sizeof(CacheRecordHeader) || record.size() % 8) return;
    auto* h = reinterpret_cast<CacheRecordHeader*>(record.data());
    h->magic = CacheRecordHeader::kMagic;
    h->size = static_cast<uint32_t>(record.size());
    h->checksum = 0;
    h->checksum = checksum(record.data(), record.size());
    std::lock_guard<std::mutex> lock(mu_);
    if (would_exceed_budget_locked(record.size())) {
        dropped_over_budget.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    recorded.fetch_add(1, std::memory_order_relaxed);
    recorded_bytes.fetch_add(record.size(), std::memory_order_relaxed);
    pending_.insert(pending_.end(), record.begin(), record.end());
    start_writer_locked();
    if (pending_.size() >= (256u << 10)) wake_.notify_one();
}

// Only records older than main.bin's generation need a touch: everything the
// last compaction kept as recently used already carries it. A record is used
// once per process unless its code is invalidated, so duplicates are rare
// and harmless; no per-process set of used records is kept.
void PersistentCache::note_use(const CacheRecordHeader* used) {
    if (main_generation_ && used->generation >= main_generation_) return;
    std::lock_guard<std::mutex> lock(mu_);
    TouchEntry t{used->file_key, used->file_offset, used->mt, {}};
    const auto* p = reinterpret_cast<const uint8_t*>(&t);
    pending_touches_.insert(pending_touches_.end(), p, p + sizeof t);
    start_writer_locked();
}

// The segment's header counts too, before the segment exists.
bool PersistentCache::would_exceed_budget_locked(size_t bytes) const {
    const uint64_t header = segment_fd_ < 0 ? sizeof(CacheFileHeader) : 0;
    return usage_base_ + written_ + header + pending_.size() + bytes > budget_;
}

void PersistentCache::append_touches_locked() {
    if (pending_touches_.empty()) return;
    std::vector<uint8_t> record(sizeof(CacheRecordHeader));
    record.insert(record.end(), pending_touches_.begin(), pending_touches_.end());
    pending_touches_.clear();
    auto* h = reinterpret_cast<CacheRecordHeader*>(record.data());
    *h = CacheRecordHeader{};
    h->magic = CacheRecordHeader::kMagic;
    h->kind = kRecordTouch;
    h->size = static_cast<uint32_t>(record.size());
    h->checksum = checksum(record.data(), record.size());
    if (would_exceed_budget_locked(record.size())) return;
    pending_.insert(pending_.end(), record.begin(), record.end());
}

void PersistentCache::start_writer_locked() {
    if (writer_started_) return;
    writer_started_ = true;
    stop_ = false;
    writer_ = std::thread([this] { writer_loop(); });
    writer_.detach();  // flush_all writes what is left at exit
}

bool PersistentCache::open_segment_locked() {
    if (segment_fd_ >= 0) return true;
    for (unsigned n = 0; n < 1000; ++n) {
        const std::string path = dir_ + "/seg-" + std::to_string(pid_) + "-" + std::to_string(n) + ".bin";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return false; }
        // A live segment: compaction leaves it alone. Locked before its
        // header is written, so a segment with a header is never unlocked.
        if (flock(fd, LOCK_SH) != 0) { ::close(fd); unlink(path.c_str()); return false; }
        CacheFileHeader h{CacheFileHeader::kMagic, CacheFileHeader::kVersion, fp_hi_, fp_lo_, 0, 0, 0, 0};
        if (!write_all(fd, &h, sizeof h)) { ::close(fd); unlink(path.c_str()); return false; }
        segment_fd_ = fd;
        written_ += sizeof h;
        return true;
    }
    return false;
}

void PersistentCache::writer_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stop_ || pending_.size() >= (256u << 10); });
            if (stop_) return;
        }
        flush();
    }
}

void PersistentCache::flush() {
    std::lock_guard<std::mutex> io(g_io_mu);
    std::vector<uint8_t> batch;
    {
        std::lock_guard<std::mutex> lock(mu_);
        append_touches_locked();
        batch.swap(pending_);
        if (batch.empty() || !open_segment_locked()) return;
    }
    if (write_all(segment_fd_, batch.data(), batch.size())) {
        std::lock_guard<std::mutex> lock(mu_);
        written_ += batch.size();
    }
}

void PersistentCache::flush_all() {
    if (g_cache) g_cache->flush();
}

// The child has no writer thread and must not append to the parent's
// segment: it opens its own if it records anything (after exec, typically).
void PersistentCache::after_fork_child() {
    PersistentCache* c = g_cache;
    if (c->segment_fd_ >= 0) ::close(c->segment_fd_);
    c->segment_fd_ = -1;
    c->pending_.clear();
    c->pending_touches_.clear();
    c->usage_base_ += c->written_;  // the parent's segment is on disk
    c->written_ = 0;
    c->writer_started_ = false;
    c->pid_ = getpid();
}

void PersistentCache::compact() {
    const int lock_fd = ::open((dir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) return;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) { ::close(lock_fd); return; }
    struct Source { Mapped file; std::string path; bool main; };
    std::vector<Source> sources;
    if (Mapped m = map_readonly(dir_ + "/main.bin", 0); m.data) sources.push_back({m, dir_ + "/main.bin", true});
    for (const auto& s : segments_by_age(dir_)) {
        const std::string path = dir_ + "/" + s.second;
        // A segment that a live process holds is still growing.
        Mapped m = map_readonly(path, LOCK_EX | LOCK_NB);
        if (m.data) { sources.push_back({m, path, false}); continue; }
        struct stat st{};
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 && fstat(fd, &st) == 0 &&
            st.st_size < static_cast<off_t>(sizeof(CacheFileHeader)) &&
            now - (static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec) > kAbandonedSegmentNs)
            unlink(path.c_str());  // empty and abandoned
        if (fd >= 0) ::close(fd);
    }
    uint32_t generation = 0;
    bool merged_segment = false;
    for (const auto& s : sources) {
        if (!header_matches(s.file.data, s.file.size, fp_hi_, fp_lo_)) continue;
        if (s.main) generation = reinterpret_cast<const CacheFileHeader*>(s.file.data)->generation;
        else merged_segment = true;
    }
    uint64_t usage;
    {
        std::lock_guard<std::mutex> lock(mu_);
        usage = usage_base_ + written_;
    }
    if (merged_segment || usage > budget_) {
        const uint32_t next = generation + 1;
        struct Kept { const CacheRecordHeader* record; uint32_t generation; };
        std::unordered_map<uint64_t, Kept> newest;
        std::unordered_map<uint64_t, bool> touched;
        for (const auto& s : sources) {
            if (!header_matches(s.file.data, s.file.size, fp_hi_, fp_lo_)) continue;
            const auto* h = reinterpret_cast<const CacheFileHeader*>(s.file.data);
            const size_t end = s.main && h->index_offset >= sizeof *h && h->index_offset <= s.file.size
                ? h->index_offset : s.file.size;
            for_each_record(s.file.data, sizeof(CacheFileHeader), end, true, [&](const CacheRecordHeader* r) {
                if (r->kind == kRecordTouch) {
                    const size_t n = (r->size - sizeof *r) / sizeof(TouchEntry);
                    const auto* t = reinterpret_cast<const TouchEntry*>(r + 1);
                    for (size_t i = 0; i < n; ++i) touched[index_key(t[i].file_key, t[i].file_offset, t[i].mt != 0)] = true;
                    return;
                }
                newest[index_key(r->file_key, r->file_offset, r->mt != 0)] = Kept{r, s.main ? r->generation : next};
            });
        }
        std::vector<std::pair<uint64_t, Kept>> order(newest.begin(), newest.end());
        for (auto& item : order) if (touched.count(item.first)) item.second.generation = next;
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.second.generation != b.second.generation ? a.second.generation > b.second.generation
                                                              : a.first < b.first;
        });
        // Leave a quarter of the budget for what the next processes record.
        const uint64_t keep_bytes = budget_ - budget_ / 4;
        const std::string tmp = dir_ + "/main.bin.tmp";
        const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        bool ok = out >= 0;
        std::vector<IndexEntry> index;
        if (ok) {
            CacheFileHeader h{CacheFileHeader::kMagic, CacheFileHeader::kVersion, fp_hi_, fp_lo_, next, 0, 0, 0};
            ok = write_all(out, &h, sizeof h);
            uint64_t at = sizeof h;
            std::vector<uint8_t> copy;
            for (const auto& item : order) {
                const CacheRecordHeader* r = item.second.record;
                if (!ok || at + r->size + (index.size() + 1) * sizeof(IndexEntry) > keep_bytes) break;
                copy.assign(reinterpret_cast<const uint8_t*>(r), reinterpret_cast<const uint8_t*>(r) + r->size);
                auto* w = reinterpret_cast<CacheRecordHeader*>(copy.data());
                w->generation = item.second.generation;
                w->checksum = 0;
                w->checksum = checksum(copy.data(), copy.size());
                ok = write_all(out, copy.data(), copy.size());
                index.push_back({item.first, at});
                at += r->size;
            }
            std::sort(index.begin(), index.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.key < b.key; });
            h.index_offset = at;
            h.index_count = index.size();
            ok = ok && write_all(out, index.data(), index.size() * sizeof(IndexEntry)) &&
                 pwrite(out, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h) && fsync(out) == 0;
            ::close(out);
        }
        if (ok && rename(tmp.c_str(), (dir_ + "/main.bin").c_str()) == 0) {
            for (const auto& s : sources) if (!s.main) unlink(s.path.c_str());
        } else {
            unlink(tmp.c_str());
        }
    }
    for (const auto& s : sources) { munmap(const_cast<uint8_t*>(s.file.data), s.file.size); ::close(s.file.fd); }
    prune_other_fingerprints(app_dir_, dir_, budget_);
    // What is on disk now, less this process's own segment, which written_ counts.
    {
        const uint64_t on_disk = tree_bytes(app_dir_);
        std::lock_guard<std::mutex> lock(mu_);
        usage_base_ = on_disk > written_ ? on_disk - written_ : 0;
    }
    flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
}

void PersistentCache::dump_stats() const {
    static const char* const kinds[kFailureKinds] = {"capture", "span", "bias", "shape", "field", "host-address",
                                                     "bytes", "edges", "faults", "deps", "entry", "other"};
    std::string why;
    for (int i = 0; i < kFailureKinds; ++i)
        if (const uint64_t n = unrecordable_by[i].load(std::memory_order_relaxed))
            why += std::string(why.empty() ? "" : " ") + kinds[i] + "=" + std::to_string(n);
    if (!why.empty()) fprintf(stderr, "[JIT] disk cache unrecordable: %s\n", why.c_str());
    fprintf(stderr, "[JIT] disk cache: hits=%llu misses=%llu rejected=%llu recorded=%llu (%.1f MiB) "
                    "unrecordable=%llu verify_mismatches=%llu verify_other_shape=%llu over_budget=%llu "
                    "indexed=%zu+%zu dir=%s\n",
            (unsigned long long)hits.load(), (unsigned long long)misses.load(),
            (unsigned long long)rejected.load(), (unsigned long long)recorded.load(),
            double(recorded_bytes.load()) / (1024.0 * 1024.0), (unsigned long long)unrecordable.load(),
            (unsigned long long)verify_mismatches.load(), (unsigned long long)verify_other_shape.load(),
            (unsigned long long)dropped_over_budget.load(), main_count_, segment_index_.size(), dir_.c_str());
}
} // namespace arm64emu
