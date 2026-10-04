#include "frost/window_stats.hpp"
#include <cassert>
#include <cmath>
using arm64emu::window_stats::Clock;
int main() {
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
}
