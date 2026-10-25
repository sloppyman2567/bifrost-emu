#include "System/Graphics/Display/StatsOverlay.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arm64emu::window_stats {
namespace {
constexpr uint32_t kPanel = 0xA80C1018;   // about 66% opaque: the game stays visible behind it
constexpr uint32_t kAccent = 0xFF7FD4FF;
constexpr uint32_t kText = 0xFFEAF0F7;
constexpr uint32_t kDim = 0xFF8A97A8;
constexpr uint32_t kTrack = 0xFF2A3342;   // the empty part of a meter
constexpr uint32_t kRule = 0xFF3A4556;
constexpr uint32_t kWarn = 0xFFFFC857;
constexpr uint32_t kHot = 0xFFFF6B5E;
constexpr int kColumns = 31;  // characters per line, at most
constexpr int kMeterCells = 8;

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
std::string pad_left(std::string text, size_t width) {
  return text.size() < width ? std::string(width - text.size(), ' ') + text : text;
}
std::string pad_right(std::string text, size_t width) {
  return text.size() < width ? text + std::string(width - text.size(), ' ') : text;
}
// Load and fullness turn amber from 80 percent and red from 95; temperatures at 80 and 90 degrees.
uint32_t load_color(double value) { return value >= 95 ? kHot : value >= 80 ? kWarn : kAccent; }
uint32_t heat_color(double value) { return value >= 90 ? kHot : value >= 80 ? kWarn : kText; }
// One aligned row: label, percentage, a meter, then a detail such as a temperature or a size.
OverlayLine meter_row(const char *label, double percent_value, const std::string &detail, uint32_t detail_color) {
  const bool known = percent_value >= 0;
  const uint32_t color = known ? load_color(percent_value) : kDim;
  return {{pad_right(label, 4), kDim},
          {pad_left(percent(percent_value), 4), known && percent_value >= 80 ? color : kText},
          {" ", kText},
          {std::string(kMeterCells, ' '), color, known ? std::min(1.0, percent_value / 100.0) : 0.0},
          {"  ", kText},
          {detail, detail_color}};
}
double fraction_percent(uint64_t used, uint64_t total) {
  return total ? 100.0 * static_cast<double>(used) / static_cast<double>(total) : -1;
}
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
  OverlayLine rate;
  if (!s.active) rate.push_back({"IDLE", kWarn});
  else if (s.fps > 0) rate.push_back({format("%.0f FPS  %.1f MS", s.fps, 1000.0 / s.fps), kText});
  else rate.push_back({"0 FPS", kText});
  rate.push_back({"  " + trim(overlay_text(s.backend), 12), kDim});
  lines.push_back(std::move(rate));
  lines.push_back({{"", kRule, -1, true}});
  const auto &r = s.sensors;
  lines.push_back(meter_row("CPU", r.cpu_percent, degrees(r.cpu_temp), heat_color(r.cpu_temp)));
  lines.push_back(meter_row("GPU", r.gpu_percent, degrees(r.gpu_temp), heat_color(r.gpu_temp)));
  const bool vram = s.vram_known && s.vram_total;
  lines.push_back(meter_row("VRAM", vram ? fraction_percent(s.vram_used, s.vram_total) : -1,
                            vram ? gigabytes(s.vram_used) + "/" + gigabytes(s.vram_total) + " GB" : "--", kText));
  lines.push_back(meter_row("RAM", r.ram_known ? fraction_percent(r.ram_used, r.ram_total) : -1,
                            r.ram_known ? gigabytes(r.ram_used) + "/" + gigabytes(r.ram_total) + " GB" : "--", kText));
  lines.push_back({{"", kRule, -1, true}});
  lines.push_back({{"EMU " + percent(r.process_percent) + " CPU  " +
                        (s.process_rss ? gigabytes(s.process_rss) + " GB" : std::string("--")), kDim}});
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
  const int pad_x = 4 * scale, pad_y = 3 * scale, advance = 6 * scale, pitch = 9 * scale, rule_pitch = 5 * scale;
  auto height_of = [&](const OverlayLine &line) { return !line.empty() && line[0].rule ? rule_pitch : pitch; };
  int content = 0;
  for (const auto &line : lines) content += height_of(line);
  OverlayImage image;
  image.w = static_cast<int>(columns) * advance - scale + 2 * pad_x;
  image.h = content - 2 * scale + 2 * pad_y;  // the last line has no gap below it
  image.argb.assign(static_cast<size_t>(image.w) * image.h, kPanel);
  auto fill = [&](int x, int y, int w, int h, uint32_t color) {
    for (int row = std::max(0, y); row < std::min(image.h, y + h); ++row)
      for (int column = std::max(0, x); column < std::min(image.w, x + w); ++column)
        image.argb[static_cast<size_t>(row) * image.w + column] = color;
  };
  // Rounded corners: pixels outside a quarter circle of radius 3 * scale stay transparent.
  const int radius = 3 * scale;
  for (int y = 0; y < radius; ++y)
    for (int x = 0; x < radius; ++x) {
      const double dx = radius - x - 0.5, dy = radius - y - 0.5;
      if (dx * dx + dy * dy <= double(radius) * radius) continue;
      for (int flip_x = 0; flip_x < 2; ++flip_x)
        for (int flip_y = 0; flip_y < 2; ++flip_y)
          image.argb[static_cast<size_t>(flip_y ? image.h - 1 - y : y) * image.w +
                     (flip_x ? image.w - 1 - x : x)] = 0;
    }
  int top = pad_y;
  for (const auto &line : lines) {
    int left = pad_x;
    for (const auto &run : line) {
      if (run.rule) {
        fill(pad_x, top + 2 * scale, image.w - 2 * pad_x, std::max(1, scale / 2 + 1), run.color);
        continue;
      }
      if (run.meter >= 0) {
        const int width = static_cast<int>(glyph_count(run.text)) * advance - scale;
        const int bar_top = top + 2 * scale, bar_height = 3 * scale;
        fill(left, bar_top, width, bar_height, kTrack);
        fill(left, bar_top, static_cast<int>(std::lround(width * std::min(1.0, run.meter))), bar_height, run.color);
        left += static_cast<int>(glyph_count(run.text)) * advance;
        continue;
      }
      for (size_t i = 0; i < run.text.size();) {
        const uint8_t *columns_bits = glyph(run.text, i);
        for (int column = 0; column < 5; ++column)
          for (int row = 0; row < 7; ++row)
            if (columns_bits[column] & (1 << row))
              fill(left + column * scale, top + row * scale, scale, scale, run.color);
        left += advance;
      }
    }
    top += height_of(line);
  }
  return image;
}
}  // namespace arm64emu::window_stats
