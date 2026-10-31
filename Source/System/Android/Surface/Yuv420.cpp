// Yuv420.cpp — see Yuv420.hpp.
#include "System/Android/Surface/Yuv420.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <vector>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define BIFROST_YUV_AVX2 1
#endif

namespace arm64emu::android {
namespace {

struct Params {
    int rv, gu, gv, bu;  // 16.16 fixed point chroma-to-RGB weights
    int y_scale, c_scale, y_offset;
};

// The matrix's chroma-to-RGB weights: R = Y + 2(1-kr)Cr, B = Y + 2(1-kb)Cb, G from the other two. Limited range:
// Y 16..235 and chroma 16..240 are stretched to 0..255.
Params parameters(YuvMatrix matrix, bool full_range) {
    double kr = .299, kb = .114;
    if (matrix == YuvMatrix::Bt709) { kr = .2126; kb = .0722; }
    else if (matrix == YuvMatrix::Bt2020) { kr = .2627; kb = .0593; }
    const double kg = 1 - kr - kb;
    auto fixed = [](double value) { return static_cast<int>(std::lround(value * 65536.0)); };
    Params p;
    p.rv = fixed(2 * (1 - kr)); p.gu = fixed(2 * kb * (1 - kb) / kg); p.gv = fixed(2 * kr * (1 - kr) / kg);
    p.bu = fixed(2 * (1 - kb));
    p.y_scale = full_range ? 65536 : static_cast<int>(std::lround(255.0 / 219.0 * 65536.0));
    p.c_scale = full_range ? 65536 : static_cast<int>(std::lround(255.0 / 224.0 * 65536.0));
    p.y_offset = full_range ? 0 : 16;
    return p;
}

inline uint8_t clamp8(int value) { return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value); }

// The definition every path must reproduce exactly: one pixel from its three samples.
inline uint32_t pixel(const Params& p, int luma, int u_sample, int v_sample, bool rgba) {
    const int cb = u_sample - 128, cr = v_sample - 128;
    const int l = static_cast<int>(((static_cast<int64_t>(luma) - p.y_offset) * p.y_scale + 32768) >> 16);
    const int u = static_cast<int>((static_cast<int64_t>(cb) * p.c_scale + 32768) >> 16);
    const int v = static_cast<int>((static_cast<int64_t>(cr) * p.c_scale + 32768) >> 16);
    const uint32_t red = clamp8(l + ((p.rv * v + 32768) >> 16));
    const uint32_t blue = clamp8(l + ((p.bu * u + 32768) >> 16));
    const uint32_t green = clamp8(l - ((p.gu * u + p.gv * v + 32768) >> 16));
    return rgba ? 0xFF000000u | (blue << 16) | (green << 8) | red : 0xFF000000u | (red << 16) | (green << 8) | blue;
}

bool plane_ok(const Yuv420Plane& plane, int64_t max_row, int64_t max_column) {
    if (!plane.data || plane.row_stride <= 0 || plane.pixel_stride <= 0 || max_row < 0 || max_column < 0) return false;
    const int64_t last = max_row * plane.row_stride + max_column * plane.pixel_stride;
    return last >= 0 && static_cast<uint64_t>(last) < plane.size;
}

#ifdef BIFROST_YUV_AVX2
#define YUV_TARGET __attribute__((target("avx2")))

bool have_avx2() {
    static const bool supported = __builtin_cpu_supports("avx2");  // includes the OS's YMM state support
    return supported;
}

struct ChromaTerms { __m256i r, g, b; };  // each 8 lanes: the red/green/blue offsets of 8 chroma samples

YUV_TARGET inline __m256i mul(__m256i a, int k) { return _mm256_mullo_epi32(a, _mm256_set1_epi32(k)); }

// pixel()'s chroma half for 8 samples held as 32-bit lanes.
YUV_TARGET inline ChromaTerms chroma_terms(const Params& p, __m256i u_samples, __m256i v_samples) {
    const __m256i half = _mm256_set1_epi32(32768), bias = _mm256_set1_epi32(128);
    const __m256i u = _mm256_srai_epi32(_mm256_add_epi32(mul(_mm256_sub_epi32(u_samples, bias), p.c_scale), half), 16);
    const __m256i v = _mm256_srai_epi32(_mm256_add_epi32(mul(_mm256_sub_epi32(v_samples, bias), p.c_scale), half), 16);
    ChromaTerms t;
    t.r = _mm256_srai_epi32(_mm256_add_epi32(mul(v, p.rv), half), 16);
    t.b = _mm256_srai_epi32(_mm256_add_epi32(mul(u, p.bu), half), 16);
    t.g = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(mul(u, p.gu), mul(v, p.gv)), half), 16);
    return t;
}

YUV_TARGET inline __m256i luma_term(const Params& p, __m256i samples) {
    return _mm256_srai_epi32(
        _mm256_add_epi32(mul(_mm256_sub_epi32(samples, _mm256_set1_epi32(p.y_offset)), p.y_scale), _mm256_set1_epi32(32768)), 16);
}

// Chroma sample k of a half-resolution run covers pixels 2k and 2k+1: turns the 8 values of samples 0..7 into the
// values of pixels 0..7 and 8..15.
YUV_TARGET inline void double_up(__m256i samples, __m256i& pixels_0_7, __m256i& pixels_8_15) {
    const __m256i a = _mm256_unpacklo_epi32(samples, samples);  // c0 c0 c1 c1 | c4 c4 c5 c5
    const __m256i b = _mm256_unpackhi_epi32(samples, samples);  // c2 c2 c3 c3 | c6 c6 c7 c7
    pixels_0_7 = _mm256_permute2x128_si256(a, b, 0x20);
    pixels_8_15 = _mm256_permute2x128_si256(a, b, 0x31);
}

// 16 pixels from their red/green/blue values (clamped here), as four bytes each in memory order.
template <bool Rgba>
YUV_TARGET inline void store16(uint32_t* out, __m256i r0, __m256i g0, __m256i b0, __m256i r1, __m256i g1, __m256i b1) {
    // packs_epi32 works per 128-bit lane: lane 0 holds pixels 0-3 and 8-11, lane 1 holds 4-7 and 12-15.
    const __m256i r = _mm256_packs_epi32(r0, r1), g = _mm256_packs_epi32(g0, g1), b = _mm256_packs_epi32(b0, b1);
    const __m256i alpha = _mm256_set1_epi16(255);
    const __m256i first = Rgba ? r : b, third = Rgba ? b : r;  // memory order: B,G,R,A or R,G,B,A
    const __m256i t0 = _mm256_packus_epi16(first, third), t1 = _mm256_packus_epi16(g, alpha);
    const __m256i lo = _mm256_unpacklo_epi8(t0, t1), hi = _mm256_unpackhi_epi8(t0, t1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), _mm256_unpacklo_epi16(lo, hi));        // pixels 0-7
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), _mm256_unpackhi_epi16(lo, hi));    // pixels 8-15
}

// Converts whole blocks of 16 pixels while every byte a block reads lies inside its plane (`*_avail` bytes from the
// pointer); returns how many pixels were done. The caller finishes the rest with pixel(). `Half`: chroma is at half
// resolution, `ChromaStride` bytes apart (1 planar, 2 semi-planar); otherwise one sample per pixel, stride 1. Luma
// is always stride 1. The first pixel's chroma sample is `u`/`v` itself (the run starts on an even column).
template <int ChromaStride, bool Half, bool Rgba>
YUV_TARGET int64_t convert_blocks(const Params& p, const uint8_t* y, size_t y_avail, const uint8_t* u, size_t u_avail,
                                  const uint8_t* v, size_t v_avail, int64_t count, uint32_t* out) {
    constexpr size_t y_bytes = 16;
    constexpr size_t c_bytes = Half ? (ChromaStride == 1 ? 8 : 16) : 16;  // bytes one block loads per chroma plane
    constexpr size_t c_step = Half ? 8 * ChromaStride : 16;                // bytes one block advances
    int64_t done = 0;
    for (; done + 16 <= count; done += 16) {
        const size_t yo = static_cast<size_t>(done), co = static_cast<size_t>(done / 16) * c_step;
        if (yo + y_bytes > y_avail || co + c_bytes > u_avail || co + c_bytes > v_avail) break;
        const __m256i l0 = luma_term(p, _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y + yo))));
        const __m256i l1 = luma_term(p, _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y + yo + 8))));
        ChromaTerms c0, c1;
        if (Half) {
            __m256i us, vs;
            if (ChromaStride == 1) {
                us = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + co)));
                vs = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + co)));
            } else {  // the wanted sample is the even byte of each 16-bit pair
                const __m256i mask = _mm256_set1_epi32(0xFF);
                us = _mm256_and_si256(_mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(u + co))), mask);
                vs = _mm256_and_si256(_mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v + co))), mask);
            }
            const ChromaTerms t = chroma_terms(p, us, vs);
            double_up(t.r, c0.r, c1.r); double_up(t.g, c0.g, c1.g); double_up(t.b, c0.b, c1.b);
        } else {
            c0 = chroma_terms(p, _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + co))),
                              _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + co))));
            c1 = chroma_terms(p, _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + co + 8))),
                              _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + co + 8))));
        }
        store16<Rgba>(out + done, _mm256_add_epi32(l0, c0.r), _mm256_sub_epi32(l0, c0.g), _mm256_add_epi32(l0, c0.b),
                      _mm256_add_epi32(l1, c1.r), _mm256_sub_epi32(l1, c1.g), _mm256_add_epi32(l1, c1.b));
    }
    return done;
}

template <bool Rgba>
int64_t direct_blocks(const Params& p, int chroma_stride, const uint8_t* y, size_t ya, const uint8_t* u, size_t ua,
                      const uint8_t* v, size_t va, int64_t count, uint32_t* out) {
    return chroma_stride == 1 ? convert_blocks<1, true, Rgba>(p, y, ya, u, ua, v, va, count, out)
                              : convert_blocks<2, true, Rgba>(p, y, ya, u, ua, v, va, count, out);
}
#endif  // BIFROST_YUV_AVX2

}  // namespace

int yuv420_to_argb(const Yuv420Source& s, const Yuv420Region& r, YuvMatrix matrix, bool full_range, uint32_t* out,
                   const Yuv420Options& options) {
    if (!out || r.layer_width <= 0 || r.layer_height <= 0 || r.width <= 0 || r.height <= 0 || r.left < 0 || r.top < 0 ||
            r.left + r.width > r.layer_width || r.top + r.height > r.layer_height || s.crop_width <= 0 ||
            s.crop_height <= 0 || s.crop_left < 0 || s.crop_top < 0)
        return -EINVAL;
    // Every sample the scaling can reach: the crop's last luma row/column, and the chroma at half of them.
    const int64_t last_x = s.crop_left + s.crop_width - 1, last_y = s.crop_top + s.crop_height - 1;
    if (!plane_ok(s.y, last_y, last_x) || !plane_ok(s.u, last_y / 2, last_x / 2) || !plane_ok(s.v, last_y / 2, last_x / 2))
        return -EINVAL;

    const Params p = parameters(matrix, full_range);
    const bool rgba = options.order == YuvOrder::Rgba;
#ifdef BIFROST_YUV_AVX2
    const bool avx2 = !options.scalar_only && have_avx2();
#else
    const bool avx2 = false;
#endif
    const bool unscaled_x = s.crop_width == r.layer_width;
    // The kernel reads luma at stride 1 and both chroma planes at one stride of 1 or 2; any other layout is staged.
    const bool direct = unscaled_x && s.y.pixel_stride == 1 && s.u.pixel_stride == s.v.pixel_stride &&
                        (s.u.pixel_stride == 1 || s.u.pixel_stride == 2);

    // Scaled (or unusual) rows: each output column's source column, worked out once for every row (32-bit: the table
    // stays in L1), and a row of samples gathered per output row that the same kernel then converts at one chroma
    // sample per pixel.
    std::vector<uint32_t> source_columns;
    std::vector<uint8_t> staged;
    if (!direct) {
        source_columns.resize(static_cast<size_t>(r.width));
        for (int64_t x = 0; x < r.width; ++x)
            source_columns[static_cast<size_t>(x)] =
                static_cast<uint32_t>(s.crop_left + (r.left + x) * s.crop_width / r.layer_width);
        staged.resize(static_cast<size_t>(r.width) * 3);
    }
    int64_t previous_row = -1;  // the source row the last output row came from: an upscale repeats it

    for (int64_t y = 0; y < r.height; ++y) {
        const int64_t sy = s.crop_top + (r.top + y) * s.crop_height / r.layer_height;
        const uint8_t* y_row = s.y.data + sy * s.y.row_stride;
        const uint8_t* u_row = s.u.data + (sy / 2) * s.u.row_stride;
        const uint8_t* v_row = s.v.data + (sy / 2) * s.v.row_stride;
        uint32_t* dest = out + y * r.width;
        int64_t x = 0;
        if (!direct) {
            if (sy == previous_row) {  // the same source row gives the same pixels
                std::copy(dest - r.width, dest, dest);
                continue;
            }
            previous_row = sy;
        }
        if (direct) {
            const int64_t sx0 = s.crop_left + r.left;
            if ((sx0 & 1) && x < r.width) {  // start on an even column: the kernel pairs pixels by chroma sample
                dest[0] = pixel(p, y_row[sx0], u_row[(sx0 / 2) * s.u.pixel_stride], v_row[(sx0 / 2) * s.v.pixel_stride], rgba);
                x = 1;
            }
#ifdef BIFROST_YUV_AVX2
            if (avx2) {
                const int64_t sx = sx0 + x;
                const uint8_t* yp = y_row + sx;
                const uint8_t* up = u_row + (sx / 2) * s.u.pixel_stride;
                const uint8_t* vp = v_row + (sx / 2) * s.v.pixel_stride;
                const size_t ya = s.y.size - static_cast<size_t>(yp - s.y.data);
                const size_t ua = s.u.size - static_cast<size_t>(up - s.u.data);
                const size_t va = s.v.size - static_cast<size_t>(vp - s.v.data);
                x += rgba ? direct_blocks<true>(p, static_cast<int>(s.u.pixel_stride), yp, ya, up, ua, vp, va, r.width - x, dest + x)
                          : direct_blocks<false>(p, static_cast<int>(s.u.pixel_stride), yp, ya, up, ua, vp, va, r.width - x, dest + x);
            }
#endif
            for (; x < r.width; ++x) {
                const int64_t sx = sx0 + x;
                dest[x] = pixel(p, y_row[sx], u_row[(sx / 2) * s.u.pixel_stride], v_row[(sx / 2) * s.v.pixel_stride], rgba);
            }
            continue;
        }
        uint8_t* ys = staged.data();
        uint8_t* us = ys + r.width;
        uint8_t* vs = us + r.width;
        const int64_t y_step = s.y.pixel_stride, u_step = s.u.pixel_stride, v_step = s.v.pixel_stride;
        const uint32_t* __restrict columns = source_columns.data();  // the staged bytes never alias the table
        const size_t width = static_cast<size_t>(r.width);
        for (size_t i = 0; i < width; ++i) {
            const int64_t sx = columns[i];
            ys[i] = y_row[sx * y_step];
            us[i] = u_row[(sx >> 1) * u_step];
            vs[i] = v_row[(sx >> 1) * v_step];
        }
#ifdef BIFROST_YUV_AVX2
        if (avx2) {
            const size_t n = static_cast<size_t>(r.width);
            x = rgba ? convert_blocks<1, false, true>(p, ys, n, us, n, vs, n, r.width, dest)
                     : convert_blocks<1, false, false>(p, ys, n, us, n, vs, n, r.width, dest);
        }
#endif
        for (; x < r.width; ++x) dest[x] = pixel(p, ys[x], us[x], vs[x], rgba);
    }
    return 0;
}

}  // namespace arm64emu::android
