#include "frost/window_stats.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
using arm64emu::window_stats::Clock;
int main() {
  setenv("BIFROST_WINDOW_STATS_TITLE", "1", 1);
  setenv("BIFROST_WINDOW_STATS_WINDOW", "0", 1);
  setenv("BIFROST_WINDOW_STATS_OVERLAY", "0", 1);
  Clock c;
  assert(c.active_fps(0) == 0);
  c.present(1000000000);
  assert(c.fps == 0 && c.frames == 1);
  for (int i = 1; i <= 60; i++)
    c.present(1000000000ULL + i * 1000000000ULL / 60);
  assert(c.frames == 61 && std::abs(c.fps - 60) < 0.001);
  assert(c.active_fps(5000000000ULL) == 0);
  c.present(6000000000ULL);
  assert(c.fps == 0);
  c.present(7000000000ULL);
  assert(c.fps == 1);
  // Native backends record on a renderer thread, then update on the window
  // thread. A forgotten/recreated window must start a fresh frame count.
  using namespace arm64emu::window_stats;
  int key;
  WindowInfo info;
  info.title = "Android app";
  info.w = 1280; info.h = 720;
  assert(update_native(&key, info).empty());
  note_present(&key, "Android EGL");
  auto title = update_native(&key, info);
  assert(title.find("Android app | bifrost-emu") == 0);
  assert(title.find("Android EGL 0.0 FPS | 1280x720 | frame 1") != std::string::npos);
  assert(title.find(" | RAM ") != std::string::npos);
  assert(title.find(" | CPU ") != std::string::npos);
  assert(title.find(" | GPU ") != std::string::npos);
  assert(title.find(" | VRAM ") != std::string::npos);
  forget(&key);
  assert(update_native(&key, info).empty());
  note_present(&key, "Android EGL");
  assert(update_native(&key, info).find("frame 1") != std::string::npos);
  forget(&key);
}
