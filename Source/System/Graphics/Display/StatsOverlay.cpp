#include "System/Graphics/Display/StatsOverlay.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arm64emu::window_stats {
namespace {
constexpr uint32_t kPanel = 0xA80C1018;   // about 66% opaque: the game stays visible behind it
constexpr uint32_t kAccent = 0xFF7FD4FF;
constexpr uint32_t kText = 0xFFEAF0F7;
constexpr uint32_t kDim = 0xFF9AA7B8;
constexpr uint32_t kWarn = 0xFFFFC857;
constexpr uint32_t kHot = 0xFFFF6B5E;
constexpr int kColumns = 38;  // characters per line, at most

// 5x7 glyphs, one byte per column with bit 0 at the top row: ASCII 0x20..0x5F (lower case prints as
// upper case), and the degree sign. The digit and letter shapes are the classic 5x7 matrix.
constexpr uint8_t kFont[64][5] = {
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
    {0x40, 0x40, 0x40, 0x40, 0x40}};
constexpr uint8_t kDegree[5] = {0x00, 0x07, 0x05, 0x07, 0x00};

const uint8_t *glyph(const std::string &text, size_t &index) {
  const unsigned char c = text[index];
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xB0) {
    index += 2;
    return kDegree;
  }
  ++index;
  if (c >= 'a' && c <= 'z') return kFont[c - 'a' + 'A' - 0x20];
  if (c >= 0x20 && c < 0x60) return kFont[c - 0x20];
  if (c >= 0x80)  // one mark for a whole multi-byte sequence
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) ++index;
  return kFont['?' - 0x20];
}
size_t glyph_count(const std::string &text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++count) glyph(text, i);
  return count;
}
std::string format(const char *pattern, double a = 0, double b = 0) {
  char text[96];
  snprintf(text, sizeof(text), pattern, a, b);
  return text;
}
std::string percent(double value) { return value < 0 ? "--%" : format("%.0f%%", value); }
std::string degrees(double value) { return value < 0 ? "--\xC2\xB0" "C" : format("%.0f\xC2\xB0" "C", value); }
std::string gigabytes(uint64_t bytes) { return format("%.1f", bytes / 1e9); }
uint32_t load_color(double value) { return value >= 95 ? kWarn : kText; }
uint32_t heat_color(double value) { return value >= 90 ? kHot : value >= 80 ? kWarn : kText; }
}  // namespace

namespace detail {
const uint8_t *glyph_columns(unsigned char c) {
  const std::string text(1, static_cast<char>(c));
  size_t index = 0;
  return glyph(text, index);
}
std::string overlay_text(const std::string &text) {
  std::string result;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = text[i];
    if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xB0) {
      result += "\xC2\xB0";
      i += 2;
    } else if (c < 0x80) {
      result += c < 0x20 || c == 0x7F ? ' ' : static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
      ++i;
    } else {
      result += '?';  // one mark for a whole multi-byte sequence
      ++i;
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

int overlay_scale(int window_height) {
  return std::clamp(static_cast<int>(std::lround(window_height / 720.0)), 2, 5);
}

std::vector<OverlayLine> overlay_lines(const OverlayStats &s) {
  using detail::overlay_text;
  auto trim = [](std::string text, size_t limit) {
    if (text.size() > limit) text.resize(limit);
    return text;
  };
  std::vector<OverlayLine> lines;
  lines.push_back({{"BIFROST-EMU " + overlay_text(s.version), kAccent}});
  if (!s.app.empty()) lines.push_back({{trim(overlay_text(s.app), kColumns), kText}});
  std::string rate = s.active ? (s.fps > 0 ? format("%.0f FPS  %.1f MS", s.fps, 1000.0 / s.fps) : "0 FPS") : "IDLE";
  lines.push_back({{trim(overlay_text(s.backend), 14) + "  " + rate, kText}});
  lines.push_back({{format("%.0f", s.width) + "X" + format("%.0f", s.height) + "  FRAME " +
                        format("%.0f", static_cast<double>(s.frames)), kDim}});
  const auto &r = s.sensors;
  lines.push_back({{"CPU ", kDim}, {percent(r.cpu_percent), load_color(r.cpu_percent)}, {"  ", kText},
                   {degrees(r.cpu_temp), heat_color(r.cpu_temp)},
                   {"  EMU " + percent(r.process_percent), kDim}});
  OverlayLine gpu = {{"GPU ", kDim}, {percent(r.gpu_percent), load_color(r.gpu_percent)}, {"  ", kText},
                     {degrees(r.gpu_temp), heat_color(r.gpu_temp)}};
  gpu.push_back({s.vram_known && s.vram_total
                     ? "  VRAM " + gigabytes(s.vram_used) + "/" + gigabytes(s.vram_total) + " GB"
                     : "  VRAM --", kDim});
  lines.push_back(std::move(gpu));
  lines.push_back({{"RAM ", kDim},
                   {r.ram_known ? gigabytes(r.ram_used) + "/" + gigabytes(r.ram_total) + " GB" : "--", kText},
                   {s.process_rss ? "  EMU " + gigabytes(s.process_rss) + " GB" : "", kDim}});
  if (!s.cpu_name.empty()) lines.push_back({{trim(overlay_text(detail::short_cpu_name(s.cpu_name)), kColumns), kDim}});
  if (!s.gpu_name.empty()) lines.push_back({{trim(overlay_text(detail::short_gpu_name(s.gpu_name)), kColumns), kDim}});
  return lines;
}

OverlayImage render_overlay(const std::vector<OverlayLine> &lines, int scale) {
  scale = std::max(1, scale);
  size_t columns = 1;
  for (const auto &line : lines) {
    size_t width = 0;
    for (const auto &run : line) width += glyph_count(run.text);
    columns = std::max(columns, width);
  }
  const int pad = 3 * scale, advance = 6 * scale, pitch = 9 * scale;
  OverlayImage image;
  image.w = static_cast<int>(columns) * advance - scale + 2 * pad;
  image.h = static_cast<int>(lines.size()) * pitch - 2 * scale + 2 * pad;
  image.argb.assign(static_cast<size_t>(image.w) * image.h, kPanel);
  // Rounded corners: pixels outside a quarter circle of radius 2 * scale stay transparent.
  const int radius = 2 * scale;
  for (int y = 0; y < radius; ++y)
    for (int x = 0; x < radius; ++x) {
      const double dx = radius - x - 0.5, dy = radius - y - 0.5;
      if (dx * dx + dy * dy <= double(radius) * radius) continue;
      for (int flip_x = 0; flip_x < 2; ++flip_x)
        for (int flip_y = 0; flip_y < 2; ++flip_y)
          image.argb[static_cast<size_t>(flip_y ? image.h - 1 - y : y) * image.w +
                     (flip_x ? image.w - 1 - x : x)] = 0;
    }
  int top = pad;
  for (const auto &line : lines) {
    int left = pad;
    for (const auto &run : line) {
      for (size_t i = 0; i < run.text.size();) {
        const uint8_t *columns_bits = glyph(run.text, i);
        for (int column = 0; column < 5; ++column)
          for (int row = 0; row < 7; ++row) {
            if (!(columns_bits[column] & (1 << row))) continue;
            for (int dy = 0; dy < scale; ++dy)
              for (int dx = 0; dx < scale; ++dx)
                image.argb[static_cast<size_t>(top + row * scale + dy) * image.w +
                           (left + column * scale + dx)] = run.color;
          }
        left += advance;
      }
    }
    top += pitch;
  }
  return image;
}
}  // namespace arm64emu::window_stats
