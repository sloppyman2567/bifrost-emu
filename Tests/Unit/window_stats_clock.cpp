#include "System/Graphics/Display/WindowStats.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
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
  assert(title.find(" | CPU ") == std::string::npos);
  assert(title.find(" | GPU ") == std::string::npos);
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.find("model name") != 0) continue;
    const auto start = line.find_first_not_of(" \t", line.find(':') + 1);
    assert(title.find(" | " + line.substr(start) + " | ") != std::string::npos);
    break;
  }
  assert(title.find(" | VRAM ") != std::string::npos);
  forget(&key);
  assert(update_native(&key, info).empty());
  note_present(&key, "Android EGL");
  assert(update_native(&key, info).find("frame 1") != std::string::npos);
  forget(&key);

  using namespace arm64emu::window_stats::detail;
  std::istringstream fdinfo("drm-client-id: 7\ndrm-pdev: 0000:01:00.0\n"
      "drm-memory-vram: 9 MiB\ndrm-resident-vram: 2 MiB\n"
      "drm-resident-vram0: 1024 KiB\ndrm-resident-vram1: 512\n"
      "drm-resident-memory: 80 MiB\ndrm-resident-gtt: 10 MiB\n");
  auto client = read_drm_client(fdinfo);
  assert(client.known && client.resident == 3 * 1048576 + 512);
  uint64_t bytes = 0;
  assert(sum_drm_clients({client, client}, "", bytes));
  assert(bytes == client.resident); // Duplicate descriptors, not double usage.
  auto second = client;
  second.id = "8";
  assert(sum_drm_clients({client, second}, "", bytes) && bytes == 2 * client.resident);
  second.device = "0000:02:00.0";
  assert(!sum_drm_clients({client, second}, "", bytes));
  assert(sum_drm_clients({client, second}, client.device, bytes) && bytes == client.resident);
  second.device = client.device; second.known = false;
  assert(!sum_drm_clients({client, second}, client.device, bytes));
  std::istringstream zero("drm-resident-vram: 0 KiB\n");
  assert(read_drm_client(zero).known);
  std::istringstream invalid("drm-resident-vram: -1 KiB\n"
      "drm-memory-vram0: 18446744073709551615 MiB\n"
      "drm-resident-vram1: 32 bananas\n");
  assert(!read_drm_client(invalid).known);
  assert(vram_text(400000000, 8000000000, true) == "400/8000 MB");
  assert(vram_text(0, 8000000000, true) == "0/8000 MB");
  assert(vram_text(0, 8000000000, false) == "unavailable/8000 MB");
  assert(vram_text(0, 0, false) == "unavailable/shared");
}
