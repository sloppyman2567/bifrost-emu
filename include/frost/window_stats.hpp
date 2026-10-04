#pragma once
#include <cstdint>
#include <string>

namespace arm64emu::window_stats {
// Monotonic presentation clock, independent of guest time and instruction rate.
struct Clock {
  uint64_t frames = 0, sample_frames = 0, sample_ns = 0, last_ns = 0;
  double fps = 0;
  void present(uint64_t now);
  double active_fps(uint64_t now) const;
};
void sdl_present(void *window, const char *backend);
void forget(void *window);
void api(const std::string &name, const uint64_t *args, uint64_t result);
void vk_present(uint64_t swapchain);
// Private re-exec entry point; owns its SDL event queue, never the guest's.
int ui_main(int fd);
} // namespace arm64emu::window_stats
