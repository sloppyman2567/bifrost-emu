// AstcToBc7.cpp — see AstcToBc7.hpp.
#include "System/Graphics/TextureCodec/AstcToBc7.hpp"
#include "System/Graphics/TextureCodec/AstcDecode.hpp"
#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>
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
    for (unsigned i = 1; i < count; i++) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    return true;
}

}  // namespace arm64emu::texcodec
