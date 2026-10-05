#pragma once
#include "bifrost/android_window_plugin.h"
#include <memory>
#include <string>
namespace frost {
struct AndroidWindowBinding { uint64_t window = 0; void *display = nullptr; };
using AndroidWindowEvent = bifrost_android_window_event_v2;
// Host-only window lifecycle. The backend owns its window and input source.
// Calls run on the display thread. No guest CPU/Memory access or SDL objects.
class AndroidWindowBackend {
public:
  virtual ~AndroidWindowBackend() = default;
  virtual const char *name() const noexcept = 0;
  virtual int open(const char *title, int32_t width, int32_t height) = 0;
  virtual void close() noexcept = 0;
  virtual int resize(int32_t width, int32_t height, bool resize_host) = 0;
  virtual AndroidWindowBinding binding() = 0;
  virtual int poll(AndroidWindowEvent &event) = 0;
};
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend();
// Explicit trusted host path; module lives until close/destroy complete.
std::unique_ptr<AndroidWindowBackend>
load_android_window_backend(const std::string &path, std::string &error);
} // namespace frost
