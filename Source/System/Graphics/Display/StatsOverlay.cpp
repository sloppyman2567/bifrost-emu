#include "System/Graphics/Display/StatsOverlay.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace arm64emu::window_stats {
namespace {
constexpr uint32_t kPanel = 0xD80B0F16;  // about 85% opaque: the game shows through without washing out the text
constexpr uint32_t kEdge = 0xE8384354;   // the outline that separates the panel from a dark scene
constexpr uint32_t kAccent = 0xFF7FD4FF;
constexpr uint32_t kText = 0xFFEAF0F7;
constexpr uint32_t kDim = 0xFF8D9AAB;
constexpr uint32_t kTrack = 0xFF283140;  // the empty part of a meter
constexpr uint32_t kRule = 0xFF3A4557;
constexpr uint32_t kWarn = 0xFFFFC857;
constexpr uint32_t kHot = 0xFFFF6B5E;
constexpr int kColumns = 31;  // characters per line, at most
constexpr int kMeterCells = 8;
constexpr int kCap = 7;    // capital height in font pixels
constexpr int kLine = 10;  // line pitch: capitals, two descender rows and one blank row

// 5x7 glyphs for printable ASCII, one word per column with bit 0 at the top row; descenders (g, j, p, q, y) use
// rows 7 and 8. Digits and capitals are the classic 5x7 matrix.
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
// The non-ASCII characters the overlay draws, by their UTF-8 sequence.
struct SpecialGlyph {
  const char *utf8;
  uint16_t columns[5];
};
constexpr SpecialGlyph kSpecial[] = {
    {"\xC2\xB0", {0x00, 0x07, 0x05, 0x07, 0x00}},      // degree sign
    {"\xC2\xB7", {0x00, 0x00, 0x08, 0x00, 0x00}},      // middle dot
    {"\xE2\x80\xA6", {0x40, 0x00, 0x40, 0x00, 0x40}},  // ellipsis
};
constexpr const char *kEllipsis = "\xE2\x80\xA6";
// U+00C0..U+00FF without their accents ("Pok\xC3\xA9mon" prints as "Pokemon"), and U+2010..U+201F as ASCII.
constexpr char kLatin1Letters[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
constexpr char kPunctuation[] = "------|_'','\"\"\"\"";
static_assert(sizeof(kLatin1Letters) == 0x40 + 1 && sizeof(kPunctuation) == 0x10 + 1);

const SpecialGlyph *special_at(const std::string &text, size_t index) {
  for (const auto &special : kSpecial)
    if (text.compare(index, strlen(special.utf8), special.utf8) == 0) return &special;
  return nullptr;
}
const uint16_t *glyph(const std::string &text, size_t &index) {
  if (const auto *special = special_at(text, index)) {
    index += strlen(special->utf8);
    return special->columns;
  }
  const unsigned char c = text[index++];
  if (c >= 0x20 && c < 0x7F) return kFont[c - 0x20];
  if (c >= 0x80)  // one mark for a whole multi-byte sequence
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) ++index;
  return kFont['?' - 0x20];
}
size_t glyph_count(const std::string &text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++count) glyph(text, i);
  return count;
}
// At most `limit` glyphs; a longer text is cut and ends in an ellipsis.
std::string fit(const std::string &text, size_t limit) {
  if (glyph_count(text) <= limit) return text;
  size_t end = 0;
  for (size_t n = 1; n < limit; ++n) glyph(text, end);
  std::string result = text.substr(0, end);
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result + kEllipsis;
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
OverlayRun right_run(std::string text, uint32_t color) {
  OverlayRun run{std::move(text), color};
  run.right = true;
  return run;
}
OverlayRun big_run(std::string text, uint32_t color) {
  OverlayRun run{std::move(text), color};
  run.size = 2;
  return run;
}
// Load and fullness turn amber from 80 percent and red from 95; temperatures at 80 and 90 degrees.
uint32_t load_color(double value) { return value >= 95 ? kHot : value >= 80 ? kWarn : kAccent; }
uint32_t heat_color(double value) { return value >= 90 ? kHot : value >= 80 ? kWarn : kText; }
// One aligned row: label, percentage, a meter, then a detail such as a temperature or a size at the right edge.
OverlayLine meter_row(const char *label, double percent_value, const std::string &detail, uint32_t detail_color) {
  const bool known = percent_value >= 0;
  const uint32_t color = known ? load_color(percent_value) : kDim;
  return {{pad_right(label, 4), kDim},
          {pad_left(percent(percent_value), 4), known && percent_value >= 80 ? color : kText},
          {" ", kText},
          {std::string(kMeterCells, ' '), color, known ? std::min(1.0, percent_value / 100.0) : 0.0},
          right_run(detail, detail_color)};
}
double fraction_percent(uint64_t used, uint64_t total) {
  return total ? 100.0 * static_cast<double>(used) / static_cast<double>(total) : -1;
}
uint32_t with_coverage(uint32_t color, double coverage) {
  const auto alpha = static_cast<uint32_t>(std::lround((color >> 24) * std::clamp(coverage, 0.0, 1.0)));
  return alpha ? alpha << 24 | (color & 0xFFFFFF) : 0;  // a clear pixel is plain zero
}
}  // namespace

namespace detail {
const uint16_t *glyph_columns(unsigned char c) {
  const std::string text(1, static_cast<char>(c));
  size_t index = 0;
  return glyph(text, index);
}
std::string overlay_text(const std::string &text) {
  std::string result;
  for (size_t i = 0; i < text.size();) {
    if (const auto *special = special_at(text, i)) {
      result += special->utf8;
      i += strlen(special->utf8);
      continue;
    }
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

int overlay_scale(int window_height) {
  return std::clamp(static_cast<int>(std::lround(window_height / 720.0)), 2, 5);
}

std::vector<OverlayLine> overlay_lines(const OverlayStats &s) {
  using detail::overlay_text;
  std::vector<OverlayLine> lines;
  lines.push_back({{"BIFROST-EMU", kAccent}, right_run(fit(overlay_text(s.version), 16), kDim)});
  // A title with nothing legible in the built-in font (all CJK, say) leaves no line of question marks.
  const std::string app = overlay_text(s.app);
  if (std::any_of(app.begin(), app.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)); }))
    lines.push_back({{fit(app, kColumns), kText}});
  // The frame rate is the figure people look for: twice the size of the rest.
  OverlayLine rate;
  if (!s.active) {
    rate.push_back(big_run("IDLE", kWarn));
  } else {
    rate.push_back(big_run(format("%.0f", s.fps), kText));
    rate.push_back({" FPS", kDim});
    if (s.fps > 0) rate.push_back({format("  %.1f ms", 1000.0 / s.fps), kText});
  }
  rate.push_back(right_run(fit(overlay_text(s.backend), 12), kDim));
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
  lines.push_back({{"Emulator " + percent(r.process_percent) + " CPU \xC2\xB7 " +
                        (s.emulator_ram ? gigabytes(s.emulator_ram) + " GB" : std::string("--")), kDim}});
  if (!s.gpu_name.empty())
    lines.push_back({{fit(overlay_text(detail::short_gpu_name(s.gpu_name)), kColumns), kDim}});
  return lines;
}

OverlayImage render_overlay(const std::vector<OverlayLine> &lines, int scale) {
  scale = std::max(1, scale);
  const int pad_x = 5 * scale, pad_y = 4 * scale, advance = 6 * scale, gutter = 2 * advance;
  auto is_rule = [](const OverlayLine &line) { return !line.empty() && line[0].rule; };
  auto run_width = [&](const OverlayRun &run) {
    return static_cast<int>(glyph_count(run.text)) * advance * std::max(1, run.size);
  };
  auto line_size = [](const OverlayLine &line) {
    int size = 1;
    for (const auto &run : line) size = std::max(size, run.size);
    return size;
  };
  // A bigger run makes its line taller by the extra capital height only: the descender and gap rows stay.
  auto height_of = [&](const OverlayLine &line) {
    return is_rule(line) ? 6 * scale : (kLine + (line_size(line) - 1) * kCap) * scale;
  };
  int content_w = 1, content_h = 0;
  for (const auto &line : lines) {
    int left = 0, right = 0;
    for (const auto &run : line) (run.right ? right : left) += run_width(run);
    content_w = std::max(content_w, left + right + (left && right ? gutter : 0) - scale);  // no gap after the last glyph
    content_h += height_of(line);
  }
  OverlayImage image;
  image.w = content_w + 2 * pad_x;
  // The last line's blank row is not needed, and its descender rows share the bottom padding.
  image.h = std::max(1, content_h - 3 * scale) + 2 * pad_y;
  image.argb.assign(static_cast<size_t>(image.w) * image.h, 0);
  // A rounded panel with a thin outline; the corner pixels are blended by how much of them the curve covers.
  const double radius = 4.0 * scale, edge = std::max(1, scale / 2);
  for (int y = 0; y < image.h; ++y)
    for (int x = 0; x < image.w; ++x) {
      const double px = x + 0.5, py = y + 0.5;
      const double cx = std::clamp(px, radius, image.w - radius), cy = std::clamp(py, radius, image.h - radius);
      // Distance from the pixel centre to the panel's outline, positive inside.
      const double inside = cx != px && cy != py ? radius - std::hypot(px - cx, py - cy)
                                                 : std::min({px, image.w - px, py, image.h - py});
      image.argb[static_cast<size_t>(y) * image.w + x] = with_coverage(inside < edge ? kEdge : kPanel, inside + 0.5);
    }
  auto fill = [&](int x, int y, int w, int h, uint32_t color) {
    for (int row = std::max(0, y); row < std::min(image.h, y + h); ++row)
      for (int column = std::max(0, x); column < std::min(image.w, x + w); ++column)
        image.argb[static_cast<size_t>(row) * image.w + column] = color;
  };
  int top = pad_y;
  for (const auto &line : lines) {
    if (is_rule(line)) {
      fill(pad_x, top + 2 * scale, image.w - 2 * pad_x, std::max(1, scale / 2), line[0].color);
      top += height_of(line);
      continue;
    }
    const int baseline = top + line_size(line) * kCap * scale;
    int right_width = 0;
    for (const auto &run : line)
      if (run.right) right_width += run_width(run);
    int left = pad_x;
    bool at_right = false;
    for (const auto &run : line) {
      if (run.right && !at_right) {
        at_right = true;
        left = image.w - pad_x - right_width + scale;
      }
      const int unit = scale * std::max(1, run.size);
      if (run.meter >= 0) {
        // The bar spans capital rows 2..4, so it sits level with the digits beside it.
        const int width = run_width(run) - scale, bar_top = baseline - 5 * scale, bar_height = 3 * scale;
        fill(left, bar_top, width, bar_height, kTrack);
        fill(left, bar_top, static_cast<int>(std::lround(width * std::min(1.0, run.meter))), bar_height, run.color);
        left += run_width(run);
        continue;
      }
      const int glyph_top = baseline - kCap * unit;
      for (size_t i = 0; i < run.text.size();) {
        const uint16_t *columns = glyph(run.text, i);
        for (int column = 0; column < 5; ++column)
          for (int row = 0; row < 9; ++row)
            if (columns[column] & (1 << row))
              fill(left + column * unit, glyph_top + row * unit, unit, unit, run.color);
        left += advance * std::max(1, run.size);
      }
    }
    top += height_of(line);
  }
  return image;
}
}  // namespace arm64emu::window_stats
