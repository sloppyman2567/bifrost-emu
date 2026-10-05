#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace frost {
// Host-only capabilities. Neither member is a guest pointer; only the EGL
// interception may pass these to host EGL. No plugin receives Memory or CPU.
struct AndroidWindowBinding {
  uint64_t window = 0;
  void *display = nullptr;
};

// Internal C++ interface. Loadable plugins use bifrost/android_window_plugin.h.
// The caller owns the host window and must detach before destroying it. Attach,
// resize and binding calls run on the display thread. Install before first use.
class AndroidWindowBackend {
public:
  virtual ~AndroidWindowBackend() = default;
  virtual const char *name() const noexcept = 0;
  // Status is 0 or negative errno. Failed operations preserve existing state.
  virtual int attach_host_window(void *borrowed_window) = 0;
  virtual void detach_host_window() noexcept = 0;
  virtual int resize(int32_t width, int32_t height, bool resize_host) = 0;
  // Resolve the window and display together: Wayland EGL requires both to
  // belong to the same connection. An empty binding means unavailable.
  virtual AndroidWindowBinding binding() = 0;
};

std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend();
// Explicit host filesystem path (must contain '/'); no guest soname lookup.
// Owns the module until detach/destroy complete. Returns nullptr with an error
// on load, ABI validation or instance creation failure.
std::unique_ptr<AndroidWindowBackend>
load_android_window_backend(const std::string &path, std::string &error);
} // namespace frost
