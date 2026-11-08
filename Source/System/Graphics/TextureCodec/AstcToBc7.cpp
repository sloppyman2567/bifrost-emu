// AstcToBc7.cpp — see AstcToBc7.hpp.
#include "System/Graphics/TextureCodec/AstcToBc7.hpp"
#include "System/Graphics/TextureCodec/AstcDecode.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <numeric>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace arm64emu::texcodec {

size_t bc7_image_bytes(int width, int height) {
    return static_cast<size_t>((width + 3) / 4) * static_cast<size_t>((height + 3) / 4) * 16;
}

namespace {

// One strip is lcm(4, block height) texel rows: whole ASTC block rows and whole BC7 block rows.
void transcode_strip(const AstcRegion& s, const Bc7Options& o, int strip, int strip_rows, int padded_w,
                     std::vector<uint8_t>& pixels, uint8_t* out) {
    const int y0 = strip * strip_rows;
    const int rows = std::min(strip_rows, s.height - y0);
    const int blocks_x = (s.width + s.block_w - 1) / s.block_w;
    const int first_block_row = y0 / s.block_h;
    const int last_block_row = (y0 + rows - 1) / s.block_h;
    uint8_t tile[ASTC_MAX_BLOCK_DIM * ASTC_MAX_BLOCK_DIM * 4];
    for (int br = first_block_row; br <= last_block_row; br++) {
        for (int bx = 0; bx < blocks_x; bx++) {
            astc_decode_block(s.blocks + br * s.row_pitch + static_cast<size_t>(bx) * 16, s.block_w, s.block_h, s.srgb, tile);
            for (int j = 0; j < s.block_h; j++) {
                const int y = br * s.block_h + j - y0;
                if (y < 0 || y >= strip_rows) continue;
                std::copy_n(tile + j * s.block_w * 4, s.block_w * 4, &pixels[(static_cast<size_t>(y) * padded_w + bx * s.block_w) * 4]);
            }
        }
    }
    // Texels past the image edge repeat the edge, so partial BC7 blocks do not fit colours that are not there.
    const int bc7_w = (s.width + 3) & ~3, bc7_rows = (rows + 3) & ~3;
    for (int y = 0; y < rows; y++) {
        uint8_t* row = &pixels[static_cast<size_t>(y) * padded_w * 4];
        for (int x = s.width; x < bc7_w; x++) std::copy_n(row + (s.width - 1) * 4, 4, row + x * 4);
    }
    for (int y = rows; y < bc7_rows; y++)
        std::copy_n(&pixels[static_cast<size_t>(rows - 1) * padded_w * 4], static_cast<size_t>(bc7_w) * 4,
                    &pixels[static_cast<size_t>(y) * padded_w * 4]);
    const int blocks4_x = bc7_w / 4;
    for (int r = 0; r < bc7_rows / 4; r++) {
        uint8_t* dst = out + static_cast<size_t>((y0 / 4 + r)) * blocks4_x * 16;
        for (int bx = 0; bx < blocks4_x; bx++) {
            uint8_t texels[64];
            for (int j = 0; j < 4; j++)
                std::copy_n(&pixels[(static_cast<size_t>(r * 4 + j) * padded_w + bx * 4) * 4], 16, texels + j * 16);
            bc7_encode_block(texels, o, dst + static_cast<size_t>(bx) * 16);
        }
    }
}

}  // namespace

bool transcode_astc_to_bc7(const AstcRegion& s, const Bc7Options& o, unsigned threads, uint8_t* out) {
    if (!astc_footprint_valid(s.block_w, s.block_h) || s.width <= 0 || s.height <= 0 || !s.blocks || !out) return false;
    const int blocks_x = (s.width + s.block_w - 1) / s.block_w;
    if (s.row_pitch < static_cast<size_t>(blocks_x) * 16) return false;
    const int strip_rows = std::lcm(4, s.block_h);
    const int strips = (s.height + strip_rows - 1) / strip_rows;
    const int padded_w = std::max(blocks_x * s.block_w, (s.width + 3) & ~3);
    std::atomic<int> next{0};
    auto worker = [&] {
        std::vector<uint8_t> pixels(static_cast<size_t>(padded_w) * strip_rows * 4);
        for (int strip = next++; strip < strips; strip = next++)
            transcode_strip(s, o, strip, strip_rows, padded_w, pixels, out);
    };
    // Threads cost more than a few strips of work.
    const unsigned useful = static_cast<unsigned>(std::min<long>(strips, static_cast<long>(blocks_x) * strips / 64 + 1));
    const unsigned count = std::min(std::max(threads, 1u), useful);
    std::vector<std::thread> pool;
    // Helpers take only idle cycles (nice 19 for this thread alone): the game's own threads always go first.
    for (unsigned i = 1; i < count; i++)
        pool.emplace_back([&worker] {
            setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
            worker();
        });
    worker();
    for (auto& t : pool) t.join();
    return true;
}

namespace detail {

struct CacheKey {
    uint64_t h1 = 0, h2 = 0;
    uint64_t shape = 0;  // footprint, colour space and size
    bool operator==(const CacheKey& o) const { return h1 == o.h1 && h2 == o.h2 && shape == o.shape; }
};
struct CacheKeyHash { size_t operator()(const CacheKey& k) const { return static_cast<size_t>(k.h1 ^ (k.h2 * 0x9E3779B97F4A7C15ull)); } };

// Two independent multiply-xorshift lanes over the blocks of each row (the row padding is not part of the content).
CacheKey make_key(const AstcRegion& s) {
    const int blocks_x = (s.width + s.block_w - 1) / s.block_w, rows = (s.height + s.block_h - 1) / s.block_h;
    CacheKey k;
    k.shape = (static_cast<uint64_t>(s.block_w) << 56) | (static_cast<uint64_t>(s.block_h) << 48) |
              (static_cast<uint64_t>(s.srgb) << 47) | (static_cast<uint64_t>(s.width) << 24) | static_cast<uint64_t>(s.height);
    uint64_t a = 0x243F6A8885A308D3ull ^ k.shape, b = 0x13198A2E03707344ull + k.shape;
    for (int r = 0; r < rows; r++) {
        const uint8_t* row = s.blocks + static_cast<size_t>(r) * s.row_pitch;
        for (int w = 0; w < blocks_x * 2; w++) {
            uint64_t v;
            std::memcpy(&v, row + static_cast<size_t>(w) * 8, 8);
            a = (a ^ v) * 0x9E3779B97F4A7C15ull; a ^= a >> 29;
            b = (b + v) * 0xC2B2AE3D27D4EB4Full; b ^= b >> 31;
        }
    }
    k.h1 = a; k.h2 = b;
    return k;
}

}  // namespace detail

using detail::CacheKey;
using detail::CacheKeyHash;
using detail::make_key;

struct Bc7Cache::Impl {
    struct Item {
        std::shared_ptr<const std::vector<uint8_t>> data;
        std::list<CacheKey>::iterator position;
    };
    mutable std::mutex mu;
    size_t capacity, used = 0;
    uint64_t hits = 0, misses = 0;
    std::list<CacheKey> order;  // most recently used first
    std::unordered_map<CacheKey, Item, CacheKeyHash> items;
    explicit Impl(size_t cap) : capacity(cap) {}

    std::shared_ptr<const std::vector<uint8_t>> find(const CacheKey& k) {
        std::lock_guard<std::mutex> g(mu);
        auto it = items.find(k);
        if (it == items.end()) { misses++; return nullptr; }
        hits++;
        order.splice(order.begin(), order, it->second.position);
        return it->second.data;
    }
    void insert(const CacheKey& k, const uint8_t* data, size_t bytes) {
        if (bytes == 0 || bytes > capacity / 4) return;  // one big texture must not push the rest out
        auto copy = std::make_shared<const std::vector<uint8_t>>(data, data + bytes);
        std::lock_guard<std::mutex> g(mu);
        if (items.count(k)) return;
        order.push_front(k);
        items[k] = {std::move(copy), order.begin()};
        used += bytes;
        while (used > capacity && !order.empty()) {
            auto victim = items.find(order.back());
            used -= victim->second.data->size();
            items.erase(victim);
            order.pop_back();
        }
    }
};

Bc7Cache::Bc7Cache(size_t capacity_bytes) : impl_(std::make_unique<Impl>(capacity_bytes)) {}
Bc7Cache::~Bc7Cache() = default;
Bc7Cache::Stats Bc7Cache::stats() const {
    std::lock_guard<std::mutex> g(impl_->mu);
    return {impl_->hits, impl_->misses, impl_->used, impl_->items.size()};
}

bool transcode_astc_to_bc7_cached(const AstcRegion& s, const Bc7Options& o, unsigned threads, Bc7Cache* cache, uint8_t* out) {
    if (!cache) return transcode_astc_to_bc7(s, o, threads, out);
    if (!astc_footprint_valid(s.block_w, s.block_h) || s.width <= 0 || s.height <= 0 || !s.blocks || !out) return false;
    const int blocks_x = (s.width + s.block_w - 1) / s.block_w;
    if (s.row_pitch < static_cast<size_t>(blocks_x) * 16) return false;
    const size_t bytes = bc7_image_bytes(s.width, s.height);
    const CacheKey key = make_key(s);
    if (auto hit = cache->impl_->find(key)) {
        if (hit->size() == bytes) { std::memcpy(out, hit->data(), bytes); return true; }
    }
    if (!transcode_astc_to_bc7(s, o, threads, out)) return false;
    cache->impl_->insert(key, out, bytes);
    return true;
}

}  // namespace arm64emu::texcodec
