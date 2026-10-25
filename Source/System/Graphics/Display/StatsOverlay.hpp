#pragma once
#include "System/Graphics/Display/HostSensors.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace arm64emu::window_stats {
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
  uint64_t process_rss = 0;
  uint64_t vram_used = 0, vram_total = 0;
  bool vram_known = false;
};
struct OverlayRun {
  std::string text;
  uint32_t color;
  double meter = -1;  // 0..1: a usage bar as wide as `text` (spaces) instead of glyphs
  bool rule = false;  // a thin divider across the panel instead of text
};
using OverlayLine = std::vector<OverlayRun>;
std::vector<OverlayLine> overlay_lines(const OverlayStats &stats);
// Pixel multiplier of the 5x7 font for a window this many pixels tall (small, at least 2).
int overlay_scale(int window_height);
OverlayImage render_overlay(const std::vector<OverlayLine> &lines, int scale);
namespace detail {
// The five column bytes of a glyph (bit 0 is the top row); lower case maps to upper, unknown bytes to '?'.
const uint8_t *glyph_columns(unsigned char c);
// "AMD Ryzen 7 5800X3D 8-Core Processor" -> "AMD Ryzen 7 5800X3D"; "Radeon RX 7600 (radeonsi, ...)" -> "Radeon RX 7600".
std::string short_cpu_name(const std::string &name);
std::string short_gpu_name(const std::string &name);
// Text as the overlay prints it: upper case, UTF-8 degree sign kept, other non-ASCII as '?'.
std::string overlay_text(const std::string &text);
}  // namespace detail
}  // namespace arm64emu::window_stats
