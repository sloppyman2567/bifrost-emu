#include "System/Graphics/Display/StatsOverlay.hpp"
#include "Generated/OverlayFont.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

namespace arm64emu::window_stats {
namespace {
namespace font = overlay_font;

// The panel follows MangoHud's defaults (data/MangoHud.conf in github.com/flightlessmango/MangoHud): a 50% black
// background, white text with a black outline, a coloured label per metric, small units level with the top of the
// figures and a green frame-time line. Only the red is brighter than its B22222: that is hard to read on the panel.
constexpr uint32_t kPanel = 0x80020202;
constexpr uint32_t kText = 0xFFFFFFFF;
constexpr uint32_t kDim = 0xFFB4B4B4;  // the footer's figures
constexpr uint32_t kGpuLabel = 0xFF2E9762, kCpuLabel = 0xFF2E97CB, kVramLabel = 0xFFAD64C1, kRamLabel = 0xFFC26693;
constexpr uint32_t kEngineLabel = 0xFFEB5B5B;  // MangoHud names the graphics API here; this is the presentation backend
constexpr uint32_t kFrameLine = 0xFF00FF00;
constexpr uint32_t kWarn = 0xFFFDFD09, kHot = 0xFFFF4D4D;
constexpr double kUnitSize = 0.58;  // units, relative to the figures (MangoHud's small font is 0.55)
constexpr double kNoteSize = 0.64;  // the footer
constexpr double kGraphMaxMs = 50;  // the top of the frame-time graph, as in MangoHud
constexpr double kOutlineAlpha = 0.9;
constexpr int kBackendChars = 12;
constexpr int kSubRows = 16;  // vertical samples per pixel when a glyph is rasterized

// The separate stats window draws with this 5x7 font: one word per column with bit 0 at the top row; descenders
// (g, j, p, q, y) use rows 7 and 8. The in-window panel uses the outline font in Include/Generated/OverlayFont.hpp.
constexpr uint16_t kFont[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x00, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x44, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x44}, {0x38, 0x44, 0x44, 0x44, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x58}, {0x04, 0x7E, 0x05, 0x05, 0x00}, {0x138, 0x144, 0x144, 0x144, 0xFC},
    {0x7F, 0x04, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x80, 0x100, 0x104, 0xFD, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x04, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x1FC, 0x44, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x1FC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x3F, 0x44, 0x44, 0x20}, {0x3C, 0x40, 0x40, 0x40, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x13C, 0x140, 0x140, 0x140, 0xFC},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08}};

// The non-ASCII characters the panel draws, by their UTF-8 sequence.
struct Special {
  const char *utf8;
  uint32_t code;
};
constexpr Special kSpecial[] = {{"\xC2\xB0", 0xB0}, {"\xC2\xB7", 0xB7}, {"\xE2\x80\xA6", 0x2026}};  // degree, dot, ellipsis
constexpr const char *kEllipsis = "\xE2\x80\xA6";
// U+00C0..U+00FF without their accents ("Pok\xC3\xA9mon" prints as "Pokemon"), and U+2010..U+201F as ASCII.
constexpr char kLatin1Letters[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
constexpr char kPunctuation[] = "------|_'','\"\"\"\"";
static_assert(sizeof(kLatin1Letters) == 0x40 + 1 && sizeof(kPunctuation) == 0x10 + 1);

// The next character of `text` as a code the font has; a whole multi-byte sequence it does not have is one '?'.
uint32_t next_code(const std::string &text, size_t &index) {
  for (const auto &special : kSpecial) {
    const size_t length = strlen(special.utf8);
    if (text.compare(index, length, special.utf8) == 0) {
      index += length;
      return special.code;
    }
  }
  const unsigned char c = text[index++];
  if (c >= 0x20 && c < 0x7F) return c;
  if (c >= 0x80)
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) ++index;
  return '?';
}
const font::Glyph &glyph_for(uint32_t code) {
  const auto *begin = std::begin(font::kGlyphs), *end = std::end(font::kGlyphs);
  const auto find = [&](uint32_t wanted) {
    return std::lower_bound(begin, end, wanted, [](const font::Glyph &g, uint32_t c) { return g.code < c; });
  };
  const auto *it = find(code);
  if (it == end || it->code != code) it = find('?');
  return *it;
}
double width_of(const std::string &text, double px) {
  double units = 0;
  for (size_t i = 0; i < text.size();) units += glyph_for(next_code(text, i)).advance;
  return units * px / font::kUnitsPerEm;
}
size_t glyph_count(const std::string &text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++count) next_code(text, i);
  return count;
}
std::string trim_end(std::string text) {
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}
// At most `limit` glyphs; a longer text is cut and ends in an ellipsis.
std::string fit(const std::string &text, size_t limit) {
  if (glyph_count(text) <= limit) return text;
  size_t end = 0;
  for (size_t n = 1; n < limit; ++n) next_code(text, end);
  return trim_end(text.substr(0, end)) + kEllipsis;
}
// As wide as `limit` pixels at most; a longer text is cut and ends in an ellipsis.
std::string fit_width(const std::string &text, double px, double limit) {
  if (width_of(text, px) <= limit) return text;
  const double ellipsis = width_of(kEllipsis, px);
  size_t keep = 0;
  for (size_t i = 0; i < text.size();) {
    next_code(text, i);
    if (width_of(text.substr(0, i), px) + ellipsis > limit) break;
    keep = i;
  }
  return trim_end(text.substr(0, keep)) + kEllipsis;
}

// A glyph as coverage: `fill` is the glyph, `edge` the same shape grown by the outline's radius (its bitmap is
// `pad` pixels larger on every side). `left` and `top` place the fill's corner from the pen on the baseline.
struct GlyphBitmap {
  int left = 0, top = 0, w = 0, h = 0, pad = 0;
  std::vector<uint8_t> fill, edge;
};
struct Edge {
  double x0, y0, x1, y1;
};
// Flattens the outline into straight edges, in pixels with y down from the baseline, `phase` quarters of a pixel
// to the right.
std::vector<Edge> glyph_edges(const font::Glyph &g, double px, int phase) {
  const double scale = px / font::kUnitsPerEm, shift = phase / 4.0;
  const auto x_of = [&](int v) { return v * scale + shift; };
  const auto y_of = [&](int v) { return -v * scale; };
  std::vector<Edge> edges;
  double cx = 0, cy = 0, sx = 0, sy = 0;
  const auto line = [&](double x, double y) {
    edges.push_back({cx, cy, x, y});
    cx = x;
    cy = y;
  };
  const int16_t *p = font::kOutline + g.first, *end = p + g.count;
  while (p < end) {
    switch (*p++) {
      case font::MOVE:
        cx = sx = x_of(p[0]);
        cy = sy = y_of(p[1]);
        p += 2;
        break;
      case font::LINE:
        line(x_of(p[0]), y_of(p[1]));
        p += 2;
        break;
      case font::QUAD: {
        const double bx = x_of(p[0]), by = y_of(p[1]), ex = x_of(p[2]), ey = y_of(p[3]), ax = cx, ay = cy;
        p += 4;
        // A quadratic curve is within `tolerance` of n straight pieces when n*n >= |P0 - 2*P1 + P2| / (4*tolerance).
        const double bend = std::hypot(ax - 2 * bx + ex, ay - 2 * by + ey);
        const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend / 0.4))), 1, 32);
        for (int k = 1; k <= pieces; ++k) {
          const double t = static_cast<double>(k) / pieces, u = 1 - t;
          line(u * u * ax + 2 * u * t * bx + t * t * ex, u * u * ay + 2 * u * t * by + t * t * ey);
        }
        break;
      }
      case font::CLOSE:
        line(sx, sy);
        break;
    }
  }
  return edges;
}
GlyphBitmap rasterize(const font::Glyph &g, double px, int phase) {
  GlyphBitmap bitmap;
  const std::vector<Edge> edges = glyph_edges(g, px, phase);
  if (edges.empty()) return bitmap;
  double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
  for (const auto &e : edges)
    for (const auto [x, y] : {std::pair{e.x0, e.y0}, std::pair{e.x1, e.y1}}) {
      min_x = std::min(min_x, x), max_x = std::max(max_x, x);
      min_y = std::min(min_y, y), max_y = std::max(max_y, y);
    }
  bitmap.left = static_cast<int>(std::floor(min_x)) - 1;
  bitmap.top = static_cast<int>(std::floor(min_y)) - 1;
  bitmap.w = static_cast<int>(std::ceil(max_x)) + 1 - bitmap.left;
  bitmap.h = static_cast<int>(std::ceil(max_y)) + 1 - bitmap.top;
  std::vector<float> cover(static_cast<size_t>(bitmap.w) * bitmap.h, 0.0f);
  // Non-zero winding over kSubRows scanlines per pixel row, with exact horizontal coverage on each.
  std::vector<std::pair<double, int>> hits;
  for (int row = 0; row < bitmap.h; ++row) {
    float *line = &cover[static_cast<size_t>(row) * bitmap.w];
    for (int sub = 0; sub < kSubRows; ++sub) {
      const double y = bitmap.top + row + (sub + 0.5) / kSubRows;
      hits.clear();
      for (const auto &e : edges) {
        if (e.y0 == e.y1 || y < std::min(e.y0, e.y1) || y >= std::max(e.y0, e.y1)) continue;
        hits.push_back({e.x0 + (y - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.y1 > e.y0 ? 1 : -1});
      }
      std::sort(hits.begin(), hits.end());
      int winding = 0;
      for (size_t k = 0; k + 1 < hits.size(); ++k) {
        winding += hits[k].second;
        if (!winding) continue;
        const double from = std::clamp(hits[k].first - bitmap.left, 0.0, static_cast<double>(bitmap.w));
        const double to = std::clamp(hits[k + 1].first - bitmap.left, 0.0, static_cast<double>(bitmap.w));
        if (to <= from) continue;
        const int first = static_cast<int>(from), last = static_cast<int>(to);
        if (first == last) {
          line[first] += static_cast<float>((to - from) / kSubRows);
          continue;
        }
        line[first] += static_cast<float>((first + 1 - from) / kSubRows);
        for (int x = first + 1; x < last; ++x) line[x] += 1.0f / kSubRows;
        if (last < bitmap.w) line[last] += static_cast<float>((to - last) / kSubRows);
      }
    }
  }
  bitmap.fill.resize(cover.size());
  for (size_t i = 0; i < cover.size(); ++i)
    bitmap.fill[i] = static_cast<uint8_t>(std::lround(std::min(1.0f, cover[i]) * 255));
  // The outline: every pixel takes the strongest nearby coverage, fading over the last half pixel of the radius.
  const double radius = std::max(1.0, px / 16.0);
  const int reach = static_cast<int>(std::ceil(radius + 0.5));
  bitmap.pad = reach;
  const int edge_w = bitmap.w + 2 * reach, edge_h = bitmap.h + 2 * reach;
  bitmap.edge.assign(static_cast<size_t>(edge_w) * edge_h, 0);
  for (int y = 0; y < edge_h; ++y)
    for (int x = 0; x < edge_w; ++x) {
      double strongest = 0;
      for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
          const int sx = x - reach + dx, sy = y - reach + dy;
          if (sx < 0 || sy < 0 || sx >= bitmap.w || sy >= bitmap.h) continue;
          const double weight = std::clamp(radius + 0.5 - std::hypot(dx, dy), 0.0, 1.0);
          strongest = std::max(strongest, weight * bitmap.fill[static_cast<size_t>(sy) * bitmap.w + sx]);
        }
      bitmap.edge[static_cast<size_t>(y) * edge_w + x] = static_cast<uint8_t>(std::lround(strongest));
    }
  return bitmap;
}
// Rasterizing is the costly step and a panel reuses a few dozen glyphs, so keep them. Per thread: no lock, and a
// forked guest process cannot inherit one that is held.
std::shared_ptr<const GlyphBitmap> cached_glyph(const font::Glyph &g, double px, int phase) {
  thread_local std::map<std::tuple<uint32_t, int, int>, std::shared_ptr<const GlyphBitmap>> cache;
  const auto key = std::make_tuple(g.code, static_cast<int>(std::lround(px * 16)), phase);
  if (const auto it = cache.find(key); it != cache.end()) return it->second;
  if (cache.size() >= 2048) cache.clear();
  return cache.emplace(key, std::make_shared<const GlyphBitmap>(rasterize(g, px, phase))).first->second;
}

// Straight-alpha "over": `rgb` at `alpha` on top of what the pixel holds.
void blend(uint32_t &pixel, uint32_t rgb, double alpha) {
  alpha = std::min(alpha, 1.0);
  if (alpha <= 0) return;
  const double below = (pixel >> 24) / 255.0, result = alpha + below * (1 - alpha);
  uint32_t out = static_cast<uint32_t>(std::lround(result * 255)) << 24;
  for (int shift = 0; shift <= 16; shift += 8) {
    const double on = (rgb >> shift) & 0xFF, under = (pixel >> shift) & 0xFF;
    out |= static_cast<uint32_t>(std::lround((on * alpha + under * below * (1 - alpha)) / result)) << shift;
  }
  pixel = out;
}
struct Placed {
  std::shared_ptr<const GlyphBitmap> glyph;
  int x, y;  // where the fill's corner lands in the image
  uint32_t color;
};
// Lays `text` out from `pen` on `baseline`; glyphs sit at quarter-pixel positions. Returns where the pen ends.
double place(std::vector<Placed> &out, const std::string &text, double pen, int baseline, double px, uint32_t color) {
  for (size_t i = 0; i < text.size();) {
    const font::Glyph &g = glyph_for(next_code(text, i));
    const double whole = std::floor(pen);
    if (g.count) {
      const auto bitmap = cached_glyph(g, px, std::min(3, static_cast<int>((pen - whole) * 4)));
      if (bitmap->w) out.push_back({bitmap, static_cast<int>(whole) + bitmap->left, baseline + bitmap->top, color});
    }
    pen += g.advance * px / font::kUnitsPerEm;
  }
  return pen;
}
// The frame-time graph: a smooth line, newest frame at the right edge, 0 to kGraphMaxMs from bottom to top.
void draw_graph(OverlayImage &image, const std::vector<float> &samples, int x0, int y0, int w, int h, uint32_t color,
                double thickness) {
  const size_t count = std::min(samples.size(), kFrameHistory), first = samples.size() - count;
  if (count < 2 || w < 2 || h < 2) return;
  std::vector<float> cover(static_cast<size_t>(w) * h, 0.0f);
  const auto point = [&](size_t k) {
    const double x = (w - 1) * static_cast<double>(k + kFrameHistory - count) / (kFrameHistory - 1);
    const double value = std::clamp(samples[first + k] / kGraphMaxMs, 0.0, 1.0);
    return std::pair{x, (h - 1) * (1 - value)};
  };
  for (size_t k = 0; k + 1 < count; ++k) {
    const auto [ax, ay] = point(k);
    const auto [bx, by] = point(k + 1);
    const double reach = thickness / 2 + 1, length2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
    const int from_x = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - reach)));
    const int to_x = std::min(w - 1, static_cast<int>(std::ceil(std::max(ax, bx) + reach)));
    const int from_y = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - reach)));
    const int to_y = std::min(h - 1, static_cast<int>(std::ceil(std::max(ay, by) + reach)));
    for (int y = from_y; y <= to_y; ++y)
      for (int x = from_x; x <= to_x; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double t = length2 > 0 ? std::clamp(((px - ax) * (bx - ax) + (py - ay) * (by - ay)) / length2, 0.0, 1.0) : 0;
        const double distance = std::hypot(px - (ax + t * (bx - ax)), py - (ay + t * (by - ay)));
        float &c = cover[static_cast<size_t>(y) * w + x];
        c = std::max(c, static_cast<float>(std::clamp(thickness / 2 + 0.5 - distance, 0.0, 1.0)));
      }
  }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (const float c = cover[static_cast<size_t>(y) * w + x]; c > 0)
        blend(image.argb[static_cast<size_t>(y0 + y) * image.w + x0 + x], color & 0xFFFFFF, c);
}

std::string format(const char *pattern, double a = 0) {
  char text[96];
  snprintf(text, sizeof(text), pattern, a);
  return text;
}
std::string percent(double value) { return value < 0 ? "--%" : format("%.0f%%", value); }
std::string gigabytes(uint64_t bytes) { return format("%.1f", bytes / 1e9); }
// Load and fullness turn amber from 80 percent and red from 95; temperatures at 80 and 90 degrees.
uint32_t load_color(double value) { return value >= 95 ? kHot : value >= 80 ? kWarn : kText; }
uint32_t heat_color(double value) { return value >= 90 ? kHot : value >= 80 ? kWarn : kText; }
double fraction_percent(uint64_t used, uint64_t total) {
  return total ? 100.0 * static_cast<double>(used) / static_cast<double>(total) : -1;
}
// A reading with its unit; one that cannot be read is "--" with no unit. `reserve` and the unit keep its column the
// same width either way.
OverlayCell figure(double value, const char *unit, uint32_t color, const char *reserve) {
  return value < 0 ? OverlayCell{"--", "", kDim, reserve, unit}
                   : OverlayCell{format("%.0f", value), unit, color, reserve, unit};
}
OverlayCell memory(bool known, uint64_t bytes, double fullness) {
  return known ? OverlayCell{gigabytes(bytes), "GB", load_color(fullness), "88.8", "GB"}
               : OverlayCell{"--", "", kDim, "88.8", "GB"};
}
OverlayCell total_memory(bool known, uint64_t bytes) {
  return known ? OverlayCell{"/" + gigabytes(bytes), "GB", kDim, "/88.8", "GB"} : OverlayCell{"", "", kDim, "/88.8", "GB"};
}
OverlayRow table_row(std::vector<OverlayCell> cells) {
  OverlayRow row;
  row.cells = std::move(cells);
  return row;
}
OverlayRow note_row(const std::string &left, uint32_t left_color, const std::string &right = "",
                    uint32_t right_color = kDim) {
  OverlayRow row;
  row.kind = OverlayRow::Note;
  row.cells.push_back({left, "", left_color, "", ""});
  if (!right.empty()) row.cells.push_back({right, "", right_color, "", ""});
  return row;
}
}  // namespace

namespace detail {
const uint16_t *glyph_columns(unsigned char c) { return kFont[(c >= 0x20 && c < 0x7F ? c : '?') - 0x20]; }
double text_width(const std::string &text, double px) { return width_of(text, px); }
std::string overlay_text(const std::string &text) {
  std::string result;
  for (size_t i = 0; i < text.size();) {
    bool special = false;
    for (const auto &s : kSpecial) {
      const size_t length = strlen(s.utf8);
      if (text.compare(i, length, s.utf8) == 0) {
        result += s.utf8;
        i += length;
        special = true;
        break;
      }
    }
    if (special) continue;
    const unsigned char c = text[i++];
    const auto next = [&](size_t at) { return at < text.size() ? static_cast<unsigned char>(text[at]) : 0; };
    if (c < 0x80) {
      result += c < 0x20 || c == 0x7F ? ' ' : static_cast<char>(c);
    } else if (c == 0xC3 && next(i) >= 0x80 && next(i) <= 0xBF) {
      result += kLatin1Letters[next(i++) - 0x80];
    } else if (c == 0xE2 && next(i) == 0x80 && next(i + 1) >= 0x90 && next(i + 1) <= 0x9F) {
      result += kPunctuation[next(i + 1) - 0x90];  // dashes and curly quotes (U+2010..U+201F)
      i += 2;
    } else {
      result += '?';  // one mark for a whole multi-byte sequence
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    }
  }
  return result;
}
std::string short_cpu_name(const std::string &name) {
  std::string text = name;
  for (const char *drop : {"(R)", "(TM)", "(tm)", "CPU", "Processor", "with Radeon Graphics"}) {
    for (size_t at; (at = text.find(drop)) != std::string::npos;) text.erase(at, std::string(drop).size());
  }
  if (const auto at = text.find(" @ "); at != std::string::npos) text.resize(at);  // Intel's "@ 3.60GHz"
  // "8-Core" and "12-Core": the count is not part of the model.
  for (size_t at = text.find("-Core"); at != std::string::npos; at = text.find("-Core")) {
    size_t start = at;
    while (start > 0 && isdigit(static_cast<unsigned char>(text[start - 1]))) --start;
    text.erase(start, at + 5 - start);
  }
  std::string result;
  for (char c : text) {
    if (c == ' ' && (result.empty() || result.back() == ' ')) continue;
    result += c;
  }
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result.empty() ? name : result;
}
std::string short_gpu_name(const std::string &name) {
  std::string text = name;
  for (const char *cut : {" (", "/PCIe", "/SSE2"})
    if (const auto at = text.find(cut); at != std::string::npos) text.resize(at);
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text.empty() ? name : text;
}
}  // namespace detail

int overlay_font_px(int window_height) {
  return std::clamp(static_cast<int>(std::lround(window_height / 55.0)), 16, 40);
}

std::vector<OverlayRow> overlay_rows(const OverlayStats &s) {
  using detail::overlay_text;
  const auto &r = s.sensors;
  const std::string degrees = "\xC2\xB0" "C", dot = " \xC2\xB7 ";
  std::vector<OverlayRow> rows;
  // MangoHud's first row: the graphics path, the frame rate and the frame time.
  OverlayRow first = table_row({{fit(overlay_text(s.backend), kBackendChars), "", kEngineLabel, "", ""}});
  if (!s.active) {
    first.cells.push_back({"idle", "", kWarn, "000", "FPS"});
    first.cells.push_back({"", "", kText, "00.0", "ms"});
  } else {
    first.cells.push_back({format("%.0f", s.fps), "FPS", kText, "000", "FPS"});
    // From 100 ms the tenths go, so a slow game does not widen its column ("00.0" is the room kept for it).
    first.cells.push_back(s.fps > 0 ? OverlayCell{format(s.fps < 10 ? "%.0f" : "%.1f", 1000.0 / s.fps), "ms", kText, "00.0", "ms"}
                                    : OverlayCell{"--", "", kDim, "00.0", "ms"});
  }
  rows.push_back(std::move(first));
  rows.push_back(table_row({{"GPU", "", kGpuLabel, "", ""}, figure(r.gpu_percent, "%", load_color(r.gpu_percent), "100"),
                            figure(r.gpu_temp, degrees.c_str(), heat_color(r.gpu_temp), "100")}));
  rows.push_back(table_row({{"CPU", "", kCpuLabel, "", ""}, figure(r.cpu_percent, "%", load_color(r.cpu_percent), "100"),
                            figure(r.cpu_temp, degrees.c_str(), heat_color(r.cpu_temp), "100")}));
  const bool vram = s.vram_known && s.vram_total;
  rows.push_back(table_row({{"VRAM", "", kVramLabel, "", ""},
                            memory(vram, s.vram_used, fraction_percent(s.vram_used, s.vram_total)),
                            total_memory(vram, s.vram_total)}));
  rows.push_back(table_row({{"RAM", "", kRamLabel, "", ""},
                            memory(r.ram_known, r.ram_used, fraction_percent(r.ram_used, r.ram_total)),
                            total_memory(r.ram_known, r.ram_total)}));
  OverlayRow gap;
  gap.kind = OverlayRow::Gap;
  rows.push_back(gap);
  // The frame-time graph, with the range it covers.
  std::string range = "--";
  if (!s.frame_ms.empty()) {
    const auto [low, high] = std::minmax_element(s.frame_ms.begin(), s.frame_ms.end());
    range = format("min %.1f", *low) + dot + format("max %.1f", *high) + " ms";
  }
  rows.push_back(note_row("Frametime", kEngineLabel, range, kText));
  OverlayRow graph;
  graph.kind = OverlayRow::Graph;
  graph.color = kFrameLine;
  graph.samples = s.frame_ms;
  rows.push_back(std::move(graph));
  rows.push_back(gap);
  // A title with nothing legible in the font (all CJK, say) leaves no line of question marks.
  const std::string app = overlay_text(s.app);
  if (std::any_of(app.begin(), app.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)); }))
    rows.push_back(note_row(app, kText));
  rows.push_back(note_row("Emulator " + percent(r.process_percent) + " CPU" + dot +
                              (s.emulator_ram ? gigabytes(s.emulator_ram) + " GB" : std::string("--")), kDim));
  rows.push_back(note_row(s.gpu_name.empty() ? "" : overlay_text(detail::short_gpu_name(s.gpu_name)), kDim,
                          overlay_text(s.version)));
  return rows;
}

OverlayImage render_overlay(const std::vector<OverlayRow> &rows, int font_px) {
  const double S = std::max(8, font_px), U = S * kUnitSize, N = S * kNoteSize;
  const auto cap_of = [](double px) { return font::kCapHeight * px / font::kUnitsPerEm; };
  const double pad_x = std::round(0.5 * S), pad_y = std::round(0.4 * S);
  const double column_gap = std::round(0.7 * S), unit_gap = std::max(2.0, std::round(0.1 * S));
  const int table_pitch = static_cast<int>(std::lround(1.2 * S)), note_pitch = static_cast<int>(std::lround(1.35 * N));
  const int gap_height = static_cast<int>(std::lround(0.3 * S)), graph_height = static_cast<int>(std::lround(2.0 * S));
  // Table columns: the labels at the left, then the figures right-aligned with each unit left-aligned after them.
  std::vector<double> number_w, unit_w;
  for (const auto &row : rows) {
    if (row.kind != OverlayRow::Table) continue;
    number_w.resize(std::max(number_w.size(), row.cells.size()), 0);
    unit_w.resize(number_w.size(), 0);
    for (size_t c = 0; c < row.cells.size(); ++c) {
      number_w[c] = std::max({number_w[c], width_of(row.cells[c].text, S), width_of(row.cells[c].reserve, S)});
      unit_w[c] = std::max({unit_w[c], width_of(row.cells[c].unit, U), width_of(row.cells[c].reserve_unit, U)});
    }
  }
  std::vector<double> column_w(number_w.size());
  double table_w = 0;
  for (size_t c = 0; c < column_w.size(); ++c) {
    column_w[c] = number_w[c] + (unit_w[c] > 0 ? unit_gap + unit_w[c] : 0);
    table_w += column_w[c] + (c ? column_gap : 0);
  }
  const double inner_w = std::max(table_w, 9 * S);
  int content_h = 0;
  for (const auto &row : rows)
    content_h += row.kind == OverlayRow::Table ? table_pitch : row.kind == OverlayRow::Note ? note_pitch
               : row.kind == OverlayRow::Graph ? graph_height : gap_height;
  OverlayImage image;
  image.w = static_cast<int>(std::ceil(inner_w + 2 * pad_x));
  image.h = static_cast<int>(std::ceil(content_h + 2 * pad_y));
  image.argb.assign(static_cast<size_t>(image.w) * image.h, 0);
  // A rounded panel; the corner pixels are blended by how much of them the curve covers.
  const double radius = 0.3 * S;
  for (int y = 0; y < image.h; ++y)
    for (int x = 0; x < image.w; ++x) {
      const double px = x + 0.5, py = y + 0.5;
      const double cx = std::clamp(px, radius, image.w - radius), cy = std::clamp(py, radius, image.h - radius);
      const double inside = radius - std::hypot(px - cx, py - cy);  // positive inside the curve; the straight sides are inside
      const double coverage = std::clamp(inside + 0.5, 0.0, 1.0);
      const auto alpha = static_cast<uint32_t>(std::lround((kPanel >> 24) * coverage));
      image.argb[static_cast<size_t>(y) * image.w + x] = alpha ? alpha << 24 | (kPanel & 0xFFFFFF) : 0;
    }
  std::vector<Placed> placed;
  double top = pad_y;
  const double right_edge = pad_x + inner_w;
  for (const auto &row : rows) {
    switch (row.kind) {
      case OverlayRow::Gap:
        top += gap_height;
        break;
      case OverlayRow::Graph:
        draw_graph(image, row.samples, static_cast<int>(pad_x), static_cast<int>(top), static_cast<int>(inner_w),
                   graph_height, row.color, std::max(1.5, S / 14));
        top += graph_height;
        break;
      case OverlayRow::Note: {
        const int baseline = static_cast<int>(std::lround(top + (note_pitch + cap_of(N)) / 2));
        double room = inner_w;
        if (row.cells.size() > 1) {
          const double right_w = width_of(row.cells[1].text, N);
          room -= right_w + column_gap;
          place(placed, row.cells[1].text, right_edge - right_w, baseline, N, row.cells[1].color);
        }
        place(placed, fit_width(row.cells[0].text, N, room), pad_x, baseline, N, row.cells[0].color);
        top += note_pitch;
        break;
      }
      case OverlayRow::Table: {
        const int baseline = static_cast<int>(std::lround(top + (table_pitch + cap_of(S)) / 2));
        double column_x = pad_x;
        for (size_t c = 0; c < row.cells.size(); ++c) {
          const OverlayCell &cell = row.cells[c];
          if (c == 0) {
            place(placed, cell.text, column_x, baseline, S, cell.color);
          } else {
            const double number_right = column_x + number_w[c];
            place(placed, cell.text, number_right - width_of(cell.text, S), baseline, S, cell.color);
            // The unit's capitals start level with the figures' capitals, as a superscript.
            place(placed, cell.unit, number_right + unit_gap,
                  static_cast<int>(std::lround(baseline - cap_of(S) + cap_of(U))), U, cell.color);
          }
          column_x += column_w[c] + column_gap;
        }
        top += table_pitch;
        break;
      }
    }
  }
  const auto put = [&](const Placed &p, const std::vector<uint8_t> &mask, int pad, uint32_t rgb, double opacity) {
    const GlyphBitmap &g = *p.glyph;
    const int w = g.w + 2 * pad, h = g.h + 2 * pad;
    for (int y = 0; y < h; ++y) {
      const int py = p.y - pad + y;
      if (py < 0 || py >= image.h) continue;
      for (int x = 0; x < w; ++x) {
        const int px = p.x - pad + x;
        if (px < 0 || px >= image.w) continue;
        if (const uint8_t m = mask[static_cast<size_t>(y) * w + x])
          blend(image.argb[static_cast<size_t>(py) * image.w + px], rgb, m / 255.0 * opacity);
      }
    }
  };
  // Every outline goes down before any glyph, so one glyph's outline never cuts into its neighbour.
  for (const auto &p : placed) put(p, p.glyph->edge, p.glyph->pad, 0x000000, kOutlineAlpha);
  for (const auto &p : placed) put(p, p.glyph->fill, 0, p.color & 0xFFFFFF, (p.color >> 24) / 255.0);
  return image;
}
}  // namespace arm64emu::window_stats
