// GrowingTables.hpp — containers for the JIT's tables. They reach hundreds of thousands of entries and grow under the
// exclusive table lock, where one std::unordered_map rehash or one std::vector copy stops every thread that translates for
// tens of milliseconds. These never move everything they hold at once.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arm64emu {

// An unordered_map or unordered_set keyed by uint64_t, split by the key's page: one insertion rehashes one shard.
template <class Table> class Sharded {
    static constexpr size_t kShards = 64;
    Table shards_[kShards];
    // By guest page: keys near each other stay near each other, as a block's neighbours are looked up with it.
    static size_t shard_of(uint64_t key) { return ((key >> 12) * 0x9E3779B97F4A7C15ull) >> 58; }
    // The shard a key is inserted in. Shards fill evenly and grow by doubling, so equal ones would all grow at once: each
    // starts at its own size between n and 2n, and keeps that ratio.
    Table& shard_to_fill(uint64_t key) {
        const size_t i = shard_of(key);
        if (shards_[i].bucket_count() <= 1) shards_[i].rehash(kShards + i);
        return shards_[i];
    }
    template <class Owner, class Inner> struct Iterator {
        Owner* owner = nullptr;
        size_t shard = kShards;
        Inner at{};
        auto& operator*() const { return *at; }
        auto* operator->() const { return &*at; }
        Iterator& operator++() { ++at; skip_empty(); return *this; }
        bool operator==(const Iterator& o) const { return shard == o.shard && (shard == kShards || at == o.at); }
        bool operator!=(const Iterator& o) const { return !(*this == o); }
        void skip_empty() {
            while (at == owner->shards_[shard].end()) {
                if (++shard == kShards) { at = Inner{}; return; }
                at = owner->shards_[shard].begin();
            }
        }
    };
    template <class Owner, class Inner> static Iterator<Owner, Inner> first(Owner* owner) {
        Iterator<Owner, Inner> it{owner, 0, owner->shards_[0].begin()};
        it.skip_empty();
        return it;
    }
    template <class Owner, class Inner> static Iterator<Owner, Inner> locate(Owner* owner, uint64_t key) {
        const size_t shard = shard_of(key);
        const Inner at = owner->shards_[shard].find(key);
        if (at == owner->shards_[shard].end()) return {};
        return {owner, shard, at};
    }
public:
    using value_type = typename Table::value_type;
    using iterator = Iterator<Sharded, typename Table::iterator>;
    using const_iterator = Iterator<const Sharded, typename Table::const_iterator>;
    iterator begin() { return first<Sharded, typename Table::iterator>(this); }
    const_iterator begin() const { return first<const Sharded, typename Table::const_iterator>(this); }
    iterator end() { return {}; }
    const_iterator end() const { return {}; }
    iterator find(uint64_t key) { return locate<Sharded, typename Table::iterator>(this, key); }
    const_iterator find(uint64_t key) const { return locate<const Sharded, typename Table::const_iterator>(this, key); }
    size_t count(uint64_t key) const { return shards_[shard_of(key)].count(key); }
    auto& operator[](uint64_t key) { return shard_to_fill(key)[key]; }
    auto& at(uint64_t key) const { return shards_[shard_of(key)].at(key); }
    void insert(uint64_t key) { shard_to_fill(key).insert(key); }
    // A map node made elsewhere, so that the insertion allocates and copies nothing; it replaces an entry that is there.
    template <class Node> auto& insert_node(Node&& node) {
        auto result = shard_to_fill(node.key()).insert(std::move(node));
        if (!result.inserted) result.position->second = std::move(result.node.mapped());
        return result.position->second;
    }
    size_t erase(uint64_t key) { return shards_[shard_of(key)].erase(key); }
    void erase(const iterator& it) { shards_[it.shard].erase(it.at); }
    void clear() { for (Table& shard : shards_) shard.clear(); }
    bool empty() const { return size() == 0; }
    size_t size() const { size_t n = 0; for (const Table& shard : shards_) n += shard.size(); return n; }
    size_t bucket_count() const { size_t n = 0; for (const Table& shard : shards_) n += shard.bucket_count(); return n; }
    void swap(Sharded& other) { std::swap_ranges(shards_, shards_ + kShards, other.shards_); }
};

// A map by guest pc whose values keep their address until clear(), split as Sharded is, each shard behind a lock of its own: a
// thread that translates looks a value up, or makes it, without the table lock.
template <class Value> class LockedMap {
    static constexpr size_t kShards = 64;
    struct alignas(64) Shard {
        std::mutex mutex;
        std::unordered_map<uint64_t, Value> map;
    };
    Shard shards_[kShards];
    Shard& shard_of(uint64_t key) { return shards_[((key >> 12) * 0x9E3779B97F4A7C15ull) >> 58]; }
public:
    using value_type = typename std::unordered_map<uint64_t, Value>::value_type;
    std::mutex& mutex_of(uint64_t key) { return shard_of(key).mutex; }
    Value& at(uint64_t key) {
        Shard& shard = shard_of(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.map[key];
    }
    // fn(value) under the shard's lock, if there is one.
    template <class Fn> void with(uint64_t key, Fn&& fn) {
        Shard& shard = shard_of(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it != shard.map.end()) fn(it->second);
    }
    // Empties every value; they keep their addresses.
    void reset() {
        for (Shard& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto& kv : shard.map) kv.second = Value();
        }
    }
    void clear() { for (Shard& shard : shards_) std::unordered_map<uint64_t, Value>().swap(shard.map); }
    void renew_locks() { for (Shard& shard : shards_) new (&shard.mutex) std::mutex(); }  // in a fork child
    size_t size() { size_t n = 0; for (Shard& s : shards_) { std::lock_guard<std::mutex> lock(s.mutex); n += s.map.size(); } return n; }
    size_t bucket_count() { size_t n = 0; for (Shard& s : shards_) { std::lock_guard<std::mutex> lock(s.mutex); n += s.map.bucket_count(); } return n; }
};

// An append-only array in segments: an element keeps its address and its index, and growing copies nothing.
template <class T, size_t kSegment = 1024> class SegmentedArray {
    std::vector<std::unique_ptr<T[]>> segments_;
    size_t size_ = 0;
    template <class Owner> struct Iterator {
        Owner* owner;
        size_t index;
        auto& operator*() const { return (*owner)[index]; }
        Iterator& operator++() { ++index; return *this; }
        bool operator!=(const Iterator& o) const { return index != o.index; }
    };
    void reach(size_t size) { while (segments_.size() * kSegment < size) segments_.emplace_back(new T[kSegment]); }
public:
    using value_type = T;
    static constexpr size_t kMaxRun = kSegment;
    T& operator[](size_t i) { return segments_[i / kSegment][i % kSegment]; }
    const T& operator[](size_t i) const { return segments_[i / kSegment][i % kSegment]; }
    Iterator<SegmentedArray> begin() { return {this, 0}; }
    Iterator<SegmentedArray> end() { return {this, size_}; }
    Iterator<const SegmentedArray> begin() const { return {this, 0}; }
    Iterator<const SegmentedArray> end() const { return {this, size_}; }
    size_t size() const { return size_; }
    size_t capacity() const { return segments_.size() * kSegment; }
    void push_back(T value) {
        reach(size_ + 1);
        (*this)[size_++] = std::move(value);
    }
    // Where a run of n elements (at most kMaxRun) that must lie side by side will start; the rest of a segment too short for it
    // stays unused.
    size_t run_start(size_t n) const { return size_ % kSegment + n > kSegment ? (size_ / kSegment + 1) * kSegment : size_; }
    // `source` need not be aligned. Returns the run's index.
    size_t append_run(const void* source, size_t n) {
        const size_t at = run_start(n);
        reach(at + n);
        if (n) std::memcpy(&(*this)[at], source, n * sizeof(T));
        size_ = at + n;
        return at;
    }
    void truncate(size_t size) {
        for (size_t i = size; i < size_; ++i) (*this)[i] = T();
        size_ = std::min(size_, size);
    }
    // Keeps the first segment: a compiler empties its own arrays after every block.
    void clear() {
        truncate(0);
        if (segments_.size() > 1) segments_.resize(1);
    }
    void swap(SegmentedArray& other) { segments_.swap(other.segments_); std::swap(size_, other.size_); }
};

}  // namespace arm64emu
