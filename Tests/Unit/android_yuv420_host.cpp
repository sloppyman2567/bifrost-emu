// android_yuv420_host.cpp — the native YUV_420_888 to ARGB converter a SurfaceView's video layer uses.
//
// Checks it against a plain floating-point reference (the formula the managed side used before the host took over)
// on random planes: planar and semi-planar chroma (pixel stride 1 and 2), padded rows, a crop scaled to a larger and a
// smaller layer, a region inside the layer, all three matrices and both ranges. Fixed-point rounding may differ from
// the reference by a couple of levels. Out-of-range geometry must be refused, never read past a plane.
#include "System/Android/Surface/Yuv420.hpp"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace arm64emu::android;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static uint8_t ref_clamp(double v) { return static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(std::lround(v))))); }

static uint32_t reference(const Yuv420Source& s, const Yuv420Region& r, YuvMatrix matrix, bool full, int64_t x, int64_t y) {
    double kr = .299, kb = .114;
    if (matrix == YuvMatrix::Bt709) { kr = .2126; kb = .0722; }
    if (matrix == YuvMatrix::Bt2020) { kr = .2627; kb = .0593; }
    const int64_t sx = s.crop_left + (r.left + x) * s.crop_width / r.layer_width;
    const int64_t sy = s.crop_top + (r.top + y) * s.crop_height / r.layer_height;
    const int yy = s.y.data[sy * s.y.row_stride + sx * s.y.pixel_stride];
    const int u = s.u.data[(sy / 2) * s.u.row_stride + (sx / 2) * s.u.pixel_stride];
    const int v = s.v.data[(sy / 2) * s.v.row_stride + (sx / 2) * s.v.pixel_stride];
    const double lum = full ? yy : (yy - 16) * (255.0 / 219);
    const double cb = (u - 128) * (full ? 1 : 255.0 / 224), cr = (v - 128) * (full ? 1 : 255.0 / 224);
    const uint8_t red = ref_clamp(lum + 2 * (1 - kr) * cr), blue = ref_clamp(lum + 2 * (1 - kb) * cb);
    const uint8_t green = ref_clamp(lum - 2 * kb * (1 - kb) / (1 - kr - kb) * cb - 2 * kr * (1 - kr) / (1 - kr - kb) * cr);
    return 0xFF000000u | (uint32_t(red) << 16) | (uint32_t(green) << 8) | blue;
}

static int worst(uint32_t a, uint32_t b) {
    int w = 0;
    for (int shift = 0; shift < 24; shift += 8) w = std::max(w, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
    return w;
}

int main() {
    std::mt19937 rng(7);
    struct Layout { int chroma_pixel_stride; int padding; };
    for (Layout layout : {Layout{1, 0}, Layout{2, 0}, Layout{1, 24}, Layout{2, 16}}) {
        const int width = 64, height = 48;
        std::vector<uint8_t> yp((width + layout.padding) * height), up(((width / 2) * layout.chroma_pixel_stride + layout.padding) * (height / 2) + 4),
                vp(up.size());
        for (auto* plane : {&yp, &up, &vp}) for (auto& b : *plane) b = static_cast<uint8_t>(rng());
        Yuv420Source s;
        s.y = {yp.data(), yp.size(), width + layout.padding, 1};
        s.u = {up.data(), up.size(), (width / 2) * layout.chroma_pixel_stride + layout.padding, layout.chroma_pixel_stride};
        s.v = {vp.data(), vp.size(), (width / 2) * layout.chroma_pixel_stride + layout.padding, layout.chroma_pixel_stride};
        struct Case { int64_t cl, ct, cw, ch, lw, lh, left, top, w, h; };
        for (Case c : {Case{0, 0, 64, 48, 64, 48, 0, 0, 64, 48},       // unscaled, whole
                       Case{8, 4, 40, 30, 80, 60, 10, 5, 50, 40},      // crop scaled up, a region
                       Case{0, 0, 64, 48, 32, 24, 0, 0, 32, 24},       // scaled down
                       Case{16, 8, 32, 32, 32, 32, 4, 4, 20, 20}})     // unscaled crop, a region
            for (YuvMatrix matrix : {YuvMatrix::Bt601, YuvMatrix::Bt709, YuvMatrix::Bt2020})
                for (bool full : {false, true}) {
                    s.crop_left = c.cl; s.crop_top = c.ct; s.crop_width = c.cw; s.crop_height = c.ch;
                    Yuv420Region region{c.lw, c.lh, c.left, c.top, c.w, c.h};
                    std::vector<uint32_t> out(static_cast<size_t>(c.w * c.h));
                    CHECK(yuv420_to_argb(s, region, matrix, full, out.data()) == 0);
                    int bad = 0;
                    for (int64_t y = 0; y < c.h; ++y) for (int64_t x = 0; x < c.w; ++x)
                        bad = std::max(bad, worst(out[y * c.w + x], reference(s, region, matrix, full, x, y)));
                    CHECK(bad <= 2);
                }
        // A region of a bigger layer is the same pixels as that part of the whole.
        s.crop_left = 0; s.crop_top = 0; s.crop_width = width; s.crop_height = height;
        std::vector<uint32_t> whole(static_cast<size_t>(width * height)), part(20 * 10);
        CHECK(yuv420_to_argb(s, {width, height, 0, 0, width, height}, YuvMatrix::Bt709, false, whole.data()) == 0);
        CHECK(yuv420_to_argb(s, {width, height, 12, 7, 20, 10}, YuvMatrix::Bt709, false, part.data()) == 0);
        bool same = true;
        for (int y = 0; y < 10; ++y) for (int x = 0; x < 20; ++x) same &= part[y * 20 + x] == whole[(y + 7) * width + x + 12];
        CHECK(same);
        // Refusals: a region outside the layer, a crop outside the planes, a missing plane, bad strides.
        std::vector<uint32_t> sink(static_cast<size_t>(width * height));
        CHECK(yuv420_to_argb(s, {width, height, 1, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
        Yuv420Source wide = s; wide.crop_width = 10000;
        CHECK(yuv420_to_argb(wide, {width, height, 0, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
        Yuv420Source tall = s; tall.crop_top = 1;
        CHECK(yuv420_to_argb(tall, {width, height, 0, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
        Yuv420Source missing = s; missing.v.data = nullptr;
        CHECK(yuv420_to_argb(missing, {width, height, 0, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
        Yuv420Source stride = s; stride.u.row_stride = 0;
        CHECK(yuv420_to_argb(stride, {width, height, 0, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
        Yuv420Source small = s; small.y.size = 10;
        CHECK(yuv420_to_argb(small, {width, height, 0, 0, width, height}, YuvMatrix::Bt601, true, sink.data()) == -EINVAL);
    }
    if (failures) { std::printf("android_yuv420_host: %d failures\n", failures); return 1; }
    std::printf("android_yuv420_host: checks passed\n");
    return 0;
}
