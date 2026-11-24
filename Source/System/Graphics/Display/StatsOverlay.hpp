#pragma once
#include "System/Graphics/Display/HostSensors.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arm64emu::window_stats {
// How many frame times the panel's graph shows, newest last.
constexpr size_t kFrameHistory = 120;
// The stats panel that is drawn inside a game window: straight-alpha 0xAARRGGBB
// rows, as the Android window backends take for popup layers.
struct OverlayImage {
  int w = 0, h = 0;
  std::vector<uint32_t> argb;
};
struct OverlayStats {
  std::string version, app, backend, gpu_name, cpu_name;
  double fps = 0;
  bool active = false;  // false: no frame for two seconds
  int width = 0, height = 0;
  uint64_t frames = 0;
  SensorReading sensors;
  uint64_t emulator_ram = 0;
  uint64_t vram_used = 0, vram_total = 0;
  bool vram_known = false;
  std::vector<float> frame_ms;  // time between the last presented frames, oldest first
};
// One cell of a table row. The first column is a left-aligned label; in the others the text is a figure, aligned on
// its right edge, and the unit follows it in a small size level with the top of the figure, as MangoHud does.
struct OverlayCell {
  std::string text;
  std::string unit;
  uint32_t color = 0xFFFFFFFF;
  // Room the column keeps for a figure and a unit this wide, so the panel does not resize as values come and go.
  std::string reserve, reserve_unit;
};
struct OverlayRow {
  enum Kind {
    Table,  // cells in aligned columns
    Note,   // small text: cells[0] at the left (cut to fit) and an optional cells[1] at the right edge
    Graph,  // the frame-time line over `samples`, in `color`
    Gap     // a little vertical space
  };
  Kind kind = Table;
  std::vector<OverlayCell> cells;
  std::vector<float> samples;
  uint32_t color = 0xFFFFFFFF;
};
std::vector<OverlayRow> overlay_rows(const OverlayStats &stats);
// Font size in pixels for a window this many pixels tall (16 to 40).
int overlay_font_px(int window_height);
// The rows as one image whose text is `font_px` pixels tall (the font's em).
OverlayImage render_overlay(const std::vector<OverlayRow> &rows, int font_px);
namespace detail {
// The five columns of a glyph of the 5x7 font the separate stats window draws with: bit 0 is the top row, capitals
// fill rows 0..6 and descenders reach row 8. Printable ASCII has its own glyph; other bytes draw as '?'.
const uint16_t *glyph_columns(unsigned char c);
// "AMD Ryzen 7 5800X3D 8-Core Processor" -> "AMD Ryzen 7 5800X3D"; "Radeon RX 7600 (radeonsi, ...)" -> "Radeon RX 7600".
std::string short_cpu_name(const std::string &name);
std::string short_gpu_name(const std::string &name);
// Text as the overlay prints it: printable ASCII and the UTF-8 degree sign, middle dot and ellipsis kept, accented
// Latin letters without the accent, dashes and curly quotes as ASCII, control characters as spaces, other non-ASCII
// as '?'.
std::string overlay_text(const std::string &text);
// Width in pixels of `text` set at `px` (the font's em), without kerning.
double text_width(const std::string &text, double px);
}  // namespace detail
}  // namespace arm64emu::window_stats
