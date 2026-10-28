// Yuv420.cpp — see Yuv420.hpp.
#include "System/Android/Surface/Yuv420.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <vector>

namespace arm64emu::android {
namespace {

struct Coefficients { int rv, gu, gv, bu; };  // 16.16 fixed point

// The matrix's chroma-to-RGB weights: R = Y + 2(1-kr)Cr, B = Y + 2(1-kb)Cb, G from the other two.
Coefficients coefficients(YuvMatrix matrix) {
    double kr = .299, kb = .114;
    if (matrix == YuvMatrix::Bt709) { kr = .2126; kb = .0722; }
    else if (matrix == YuvMatrix::Bt2020) { kr = .2627; kb = .0593; }
    const double kg = 1 - kr - kb;
    auto fixed = [](double value) { return static_cast<int>(std::lround(value * 65536.0)); };
    return {fixed(2 * (1 - kr)), fixed(2 * kb * (1 - kb) / kg), fixed(2 * kr * (1 - kr) / kg), fixed(2 * (1 - kb))};
}

inline uint8_t clamp8(int value) { return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value); }

bool plane_ok(const Yuv420Plane& plane, int64_t max_row, int64_t max_column) {
    if (!plane.data || plane.row_stride <= 0 || plane.pixel_stride <= 0 || max_row < 0 || max_column < 0) return false;
    const int64_t last = max_row * plane.row_stride + max_column * plane.pixel_stride;
    return last >= 0 && static_cast<uint64_t>(last) < plane.size;
}

}  // namespace

int yuv420_to_argb(const Yuv420Source& s, const Yuv420Region& r, YuvMatrix matrix, bool full_range, uint32_t* out) {
    if (!out || r.layer_width <= 0 || r.layer_height <= 0 || r.width <= 0 || r.height <= 0 || r.left < 0 || r.top < 0 ||
            r.left + r.width > r.layer_width || r.top + r.height > r.layer_height || s.crop_width <= 0 ||
            s.crop_height <= 0 || s.crop_left < 0 || s.crop_top < 0)
        return -EINVAL;
    // Every sample the scaling can reach: the crop's last luma row/column, and the chroma at half of them.
    const int64_t last_x = s.crop_left + s.crop_width - 1, last_y = s.crop_top + s.crop_height - 1;
    if (!plane_ok(s.y, last_y, last_x) || !plane_ok(s.u, last_y / 2, last_x / 2) || !plane_ok(s.v, last_y / 2, last_x / 2))
        return -EINVAL;

    const Coefficients k = coefficients(matrix);
    // Limited range: Y 16..235 and chroma 16..240 are stretched to 0..255.
    const int y_scale = full_range ? 65536 : static_cast<int>(std::lround(255.0 / 219.0 * 65536.0));
    const int c_scale = full_range ? 65536 : static_cast<int>(std::lround(255.0 / 224.0 * 65536.0));
    const int y_offset = full_range ? 0 : 16;

    // One source column per output column, reused by every row.
    std::vector<int64_t> columns(static_cast<size_t>(r.width));
    for (int64_t x = 0; x < r.width; ++x)
        columns[static_cast<size_t>(x)] = s.crop_left + (r.left + x) * s.crop_width / r.layer_width;
    const bool unscaled = s.crop_width == r.layer_width && s.crop_height == r.layer_height;

    for (int64_t y = 0; y < r.height; ++y) {
        const int64_t sy = s.crop_top + (r.top + y) * s.crop_height / r.layer_height;
        const uint8_t* y_row = s.y.data + sy * s.y.row_stride;
        const uint8_t* u_row = s.u.data + (sy / 2) * s.u.row_stride;
        const uint8_t* v_row = s.v.data + (sy / 2) * s.v.row_stride;
        uint32_t* dest = out + y * r.width;
        for (int64_t x = 0; x < r.width; ++x) {
            const int64_t sx = unscaled ? s.crop_left + r.left + x : columns[static_cast<size_t>(x)];
            const int luma = y_row[sx * s.y.pixel_stride];
            const int cb = u_row[(sx / 2) * s.u.pixel_stride] - 128, cr = v_row[(sx / 2) * s.v.pixel_stride] - 128;
            const int l = static_cast<int>(((static_cast<int64_t>(luma) - y_offset) * y_scale + 32768) >> 16);
            const int u = static_cast<int>((static_cast<int64_t>(cb) * c_scale + 32768) >> 16);
            const int v = static_cast<int>((static_cast<int64_t>(cr) * c_scale + 32768) >> 16);
            const int red = l + ((k.rv * v + 32768) >> 16);
            const int blue = l + ((k.bu * u + 32768) >> 16);
            const int green = l - ((k.gu * u + k.gv * v + 32768) >> 16);
            dest[x] = 0xFF000000u | (static_cast<uint32_t>(clamp8(red)) << 16) | (static_cast<uint32_t>(clamp8(green)) << 8) |
                      clamp8(blue);
        }
    }
    return 0;
}

}  // namespace arm64emu::android
