#pragma once
// The Resolution setting (BIFROST_ANDROID_RESOLUTION): a fixed-size app's panel, scaled so its short side is a chosen
// number of pixels (1080 gives 1080x1920 or 1920x1080). The host window scales the panel to its own size, so the app
// draws at that resolution however large the window is. SDL-free, so `make test-android-panel-resolution` covers it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace frost {
namespace panel_resolution {
constexpr int32_t kMin = 240, kMax = 4320;  // a panel's short side in pixels; anything else is ignored

// The setting's value from its environment text: the short side in pixels, or 0 (follow the window) for none, text that
// is not a plain number and a number out of range.
inline int32_t parse(const char *text) {
  if (!text || *text < '0' || *text > '9') return 0;
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  return (*end || value < kMin || value > kMax) ? 0 : int32_t(value);
}

// Scale the panel in place to that short side, keeping its shape. A short side of 0 leaves it as it is.
inline void scale(int32_t &width, int32_t &height, int32_t short_side) {
  if (short_side <= 0 || width <= 0 || height <= 0) return;
  const double factor = double(short_side) / std::min(width, height);
  width = std::max<int32_t>(1, int32_t(std::lround(width * factor)));
  height = std::max<int32_t>(1, int32_t(std::lround(height * factor)));
}
}  // namespace panel_resolution
}  // namespace frost
