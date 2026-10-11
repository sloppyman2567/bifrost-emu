// Optional shared-object packaging of the default SDL/EGL window backend.
// Only the versioned C entry point is exported; no libbifrost dependency.
#include "Bifrost/AndroidWindowPlugin.h"
#include "System/Android/Window/WindowBackend.hpp"
#include <cerrno>
#include <new>

namespace {
using frost::AndroidWindowBackend;
int32_t create(void **out) {
  *out = nullptr;
  try {
    *out = frost::make_sdl_android_window_backend().release();
    return *out ? 0 : -ENOMEM;
  } catch (const std::bad_alloc &) { return -ENOMEM; }
  catch (...) { return -EIO; }
}
void destroy(void *instance) { delete static_cast<AndroidWindowBackend *>(instance); }
int32_t open(void *instance, const char *title, int32_t w, int32_t h) {
  try { return static_cast<AndroidWindowBackend *>(instance)->open(title, w, h); }
  catch (...) { return -EIO; }
}
void close(void *instance) { static_cast<AndroidWindowBackend *>(instance)->close(); }
int32_t resize(void *instance, int32_t w, int32_t h, uint32_t host) {
  try { return static_cast<AndroidWindowBackend *>(instance)->resize(w, h, host != 0); }
  catch (...) { return -EIO; }
}
int32_t binding(void *instance, bifrost_android_window_binding_v2 *out) {
  *out = {};
  try {
    auto value = static_cast<AndroidWindowBackend *>(instance)->binding();
    if (!value.window) return -ENODEV;
    *out = {value.window, value.display};
    return 0;
  } catch (...) { return -EIO; }
}
int32_t poll(void *instance, bifrost_android_window_event_v2 *out) {
  *out = {};
  try { return static_cast<AndroidWindowBackend *>(instance)->poll(*out); }
  catch (...) { *out = {}; return -EIO; }
}
const bifrost_android_window_plugin_v2 api = {
    BIFROST_ANDROID_WINDOW_ABI_V2, sizeof(api), "sdl3-egl-plugin",
    create, destroy, open, close, resize, binding, poll};
}
extern "C" __attribute__((visibility("default")))
const bifrost_android_window_plugin_v2 *bifrost_android_window_plugin_get_api(uint32_t host_abi) {
  return host_abi == BIFROST_ANDROID_WINDOW_ABI_V2 ? &api : nullptr;
}
