#pragma once
// The size a game window had when it last closed, kept beside the app's launcher settings so the next launch opens in it
// (BIFROST_ANDROID_WINDOW_STATE names the file). SDL-free, so `make test-android-window-size` covers it without a window.
// The file is one line, "WIDTH HEIGHT", in desktop units.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace frost {
namespace remembered_size {
constexpr int32_t kMin = 160, kMax = 32768;  // anything outside is a damaged file, not a window

// False for a missing file or any other content, so a stale or edited file never gives a window of a strange size.
inline bool load(const std::string &path, int32_t &width, int32_t &height) {
  std::FILE *file = std::fopen(path.c_str(), "r");
  if (!file) return false;
  char line[64];
  const bool read = std::fgets(line, sizeof line, file) != nullptr;
  std::fclose(file);
  long w = 0, h = 0;  // wide enough that an oversized number is out of range, not wrapped back into it
  int used = 0;
  if (!read || std::sscanf(line, "%ld %ld%n", &w, &h, &used) != 2) return false;
  while (line[used] == ' ' || line[used] == '\n' || line[used] == '\r') ++used;
  if (line[used] || w < kMin || h < kMin || w > kMax || h > kMax) return false;
  width = int32_t(w);
  height = int32_t(h);
  return true;
}

// Replaces the file whole (a temporary file, then rename), so a window closed during a write leaves the old size.
inline bool store(const std::string &path, int32_t width, int32_t height) {
  if (width < kMin || height < kMin || width > kMax || height > kMax) return false;
  const std::string temporary = path + ".tmp";
  std::FILE *file = std::fopen(temporary.c_str(), "w");
  if (!file) return false;
  const bool written = std::fprintf(file, "%d %d\n", width, height) > 0;
  const bool closed = std::fclose(file) == 0;
  if (written && closed && std::rename(temporary.c_str(), path.c_str()) == 0) return true;
  ::unlink(temporary.c_str());
  return false;
}

// The window to open into: the remembered size, held inside the room the desktop gave (a display smaller than the one the
// size was saved on, or the work area a compositor clamped the first window to). A bound that is not positive is none.
inline void fit(int32_t &width, int32_t &height, int32_t room_width, int32_t room_height) {
  if (room_width > 0) width = std::min(width, room_width);
  if (room_height > 0) height = std::min(height, room_height);
  width = std::max(width, kMin);
  height = std::max(height, kMin);
}

// A window that must keep its shape (an app that asked for an orientation): the largest window of the current shape inside
// the remembered one. The remembered size is a box, not a shape.
inline void fit_shape(int32_t &width, int32_t &height, int32_t box_width, int32_t box_height) {
  if (width <= 0 || height <= 0) return;
  const double scale = std::min(double(box_width) / width, double(box_height) / height);
  width = std::max(1, int32_t(std::lround(width * scale)));
  height = std::max(1, int32_t(std::lround(height * scale)));
}
}  // namespace remembered_size
}  // namespace frost
