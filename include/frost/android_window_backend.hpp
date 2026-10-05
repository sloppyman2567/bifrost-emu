#pragma once
#include <cstdint>
#include <memory>

namespace frost {
// Host-only capabilities. Neither member is a guest pointer; only the EGL
// interception may pass these to host EGL. No plugin receives Memory or CPU.
struct AndroidWindowBinding {
  uint64_t window = 0;
  void *display = nullptr;
};

// Built-in plugin interface, not an ABI for loading third-party shared objects.
// The caller owns the host window and must detach before destroying it. Attach,
// resize and binding calls run on the display thread. Install before first use.
class AndroidWindowBackend {
public:
  virtual ~AndroidWindowBackend() = default;
  virtual const char *name() const noexcept = 0;
  virtual void attach_host_window(void *borrowed_window) = 0;
  virtual void resize(int32_t width, int32_t height, bool resize_host) = 0;
  // Resolve the window and display together: Wayland EGL requires both to
  // belong to the same connection. An empty binding means unavailable.
  virtual AndroidWindowBinding binding() = 0;
};

std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend();
} // namespace frost
