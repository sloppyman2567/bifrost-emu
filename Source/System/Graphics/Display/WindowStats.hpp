#pragma once
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace arm64emu::window_stats {
namespace detail {
// Host DRM accounting helpers, shared with the focused regression.
struct DrmClient {
  std::string device, id;
  uint64_t resident = 0;
  bool known = false;
};
DrmClient read_drm_client(std::istream &input);
bool sum_drm_clients(const std::vector<DrmClient> &clients,
                     const std::string &device, uint64_t &bytes);
std::string vram_text(uint64_t used, uint64_t total, bool known);
}
// Monotonic presentation clock, independent of guest time and instruction rate.
struct Clock {
  uint64_t frames = 0, sample_frames = 0, sample_ns = 0, last_ns = 0;
  double fps = 0;
  void present(uint64_t now);
  double active_fps(uint64_t now) const;
};
void sdl_present(void *window, const char *backend);
// Backend-neutral path: rendering threads only record completed presents.
// The window owner supplies a snapshot and applies a returned title while
// pumping events, so SDL3 windows never pass through the SDL2 ABI.
struct WindowInfo {
  std::string title;
  int x = 0, y = 0, w = 0, h = 0;
  bool visible = false;
  uint64_t xwindow = 0;
};
void note_present(void *key, const char *backend);
std::string update_native(void *key, const WindowInfo &info);
void forget(void *window);
void api(const std::string &name, const uint64_t *args, uint64_t result);
void vk_present(uint64_t swapchain);
// Private re-exec entry point; owns its SDL event queue, never the guest's.
int ui_main(int fd);
} // namespace arm64emu::window_stats
