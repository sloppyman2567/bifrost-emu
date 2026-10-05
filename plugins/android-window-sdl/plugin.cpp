// Optional shared-object packaging of the default SDL/EGL window backend.
// Only the versioned C entry point is exported; no libbifrost dependency.
#include "bifrost/android_window_plugin.h"
#include "frost/android_window_backend.hpp"
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
int32_t attach(void *instance, void *window) {
  try { return static_cast<AndroidWindowBackend *>(instance)->attach_host_window(window); }
  catch (...) { return -EIO; }
}
void detach(void *instance) { static_cast<AndroidWindowBackend *>(instance)->detach_host_window(); }
int32_t resize(void *instance, int32_t w, int32_t h, uint32_t host) {
  try { return static_cast<AndroidWindowBackend *>(instance)->resize(w, h, host != 0); }
  catch (...) { return -EIO; }
}
int32_t binding(void *instance, bifrost_android_window_binding_v1 *out) {
  *out = {};
  try {
    auto value = static_cast<AndroidWindowBackend *>(instance)->binding();
    if (!value.window) return -ENODEV;
    *out = {value.window, value.display};
    return 0;
  } catch (...) { return -EIO; }
}
const bifrost_android_window_plugin_v1 api = {
    BIFROST_ANDROID_WINDOW_ABI_V1, sizeof(api), "sdl-egl-plugin",
    create, destroy, attach, detach, resize, binding};
}
extern "C" __attribute__((visibility("default")))
const bifrost_android_window_plugin_v1 *bifrost_android_window_plugin_get_api(uint32_t host_abi) {
  return host_abi == BIFROST_ANDROID_WINDOW_ABI_V1 ? &api : nullptr;
}
