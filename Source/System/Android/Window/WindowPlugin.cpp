#include "System/Android/Window/WindowBackend.hpp"
#include "Bifrost/AndroidWindowPlugin.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <utility>

namespace frost {
namespace {
struct CloseModule {
  void operator()(void *module) const noexcept { if (module) dlclose(module); }
};
using Module = std::unique_ptr<void, CloseModule>;

int status(int32_t result) {
  return result > 0 || result < -4095 ? -EPROTO : result;
}

class PluginWindowBackend final : public AndroidWindowBackend {
public:
  // Copies only the bytes the plugin declared; absent optional callbacks stay null.
  PluginWindowBackend(Module module, const bifrost_android_window_plugin_v2 &api)
      : module_(std::move(module)), name_(api.name) {
    std::memcpy(&api_, &api, std::min<size_t>(api.struct_size, sizeof(api_)));
    if (!api_.show_popup || !api_.remove_popup) {
      api_.show_popup = nullptr;
      api_.remove_popup = nullptr;
    }
  }
  ~PluginWindowBackend() override {
    if (instance_) {
      if (created_) api_.close(instance_);
      api_.destroy(instance_);
    }
    // module_ is closed after the plugin's last callback returns.
  }
  int create() {
    int result = status(api_.create(&instance_));
    created_ = result == 0 && instance_ != nullptr;
    return result;
  }
  bool has_instance() const { return instance_ != nullptr; }
  const char *name() const noexcept override { return name_.c_str(); }
  int open(const char *title, int32_t width, int32_t height) override {
    if (!title || width <= 0 || height <= 0) return -EINVAL;
    return status(api_.open(instance_, title, width, height));
  }
  void close() noexcept override { api_.close(instance_); }
  int resize(int32_t width, int32_t height, bool resize_host) override {
    if (width <= 0 || height <= 0) return -EINVAL;
    return status(api_.resize(instance_, width, height, resize_host ? 1u : 0u));
  }
  AndroidWindowBinding binding() override {
    bifrost_android_window_binding_v2 out{};
    if (status(api_.binding(instance_, &out)) || !out.window) return {};
    return {out.window, out.display};
  }

  int show_popup(uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                 const uint32_t *argb) override {
    if (!api_.show_popup) return -ENOTSUP;
    if (w <= 0 || h <= 0 || !argb) return -EINVAL;
    return status(api_.show_popup(instance_, id, x, y, w, h, argb));
  }
  void remove_popup(uint32_t id) override {
    if (api_.remove_popup) api_.remove_popup(instance_, id);
  }

  int poll(AndroidWindowEvent &event) override {
    event = {};
    int32_t result = api_.poll(instance_, &event);
    if (result == 1) {
      const bool bad_key = event.type == BIFROST_WINDOW_KEY &&
          (event.action < 0 || event.action > 1 || event.key_code <= 0 || event.repeat_count < 0);
      const bool bad_pointer = event.type == BIFROST_WINDOW_POINTER &&
          ((event.action != 0 && event.action != 1 && event.action != 2 && event.action != 7) ||
           event.pointer_id < 0 || !std::isfinite(event.x) || !std::isfinite(event.y) ||
           !std::isfinite(event.pressure) || event.pressure < 0);
      if (event.type < BIFROST_WINDOW_CLOSE || event.type > BIFROST_WINDOW_POINTER ||
          event.time_ms < 0 || bad_key || bad_pointer ||
          (event.type == BIFROST_WINDOW_RESIZE && (event.width <= 0 || event.height <= 0))) {
        event = {};
        return -EPROTO;
      }
      return 1;
    }
    event = {};
    return status(result);
  }

private:
  Module module_;
  // Copy the validated v2 prefix: callbacks cannot change under the host.
  bifrost_android_window_plugin_v2 api_{};
  std::string name_;
  void *instance_ = nullptr;
  bool created_ = false;
};
} // namespace

std::unique_ptr<AndroidWindowBackend>
load_android_window_backend(const std::string &path, std::string &error) {
  error.clear();
  if (path.empty() || path.find('/') == std::string::npos) {
    error = "window plugin requires an explicit host path containing '/'";
    return nullptr;
  }
  Module module(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL));
  if (!module) {
    const char *detail = dlerror();
    error = "cannot load window plugin: " + std::string(detail ? detail : path.c_str());
    return nullptr;
  }
  dlerror();
  auto entry = reinterpret_cast<bifrost_android_window_plugin_get_api_fn>(
      dlsym(module.get(), BIFROST_ANDROID_WINDOW_PLUGIN_ENTRY));
  const char *detail = dlerror();
  if (detail || !entry) {
    error = "window plugin has no " BIFROST_ANDROID_WINDOW_PLUGIN_ENTRY " entry point";
    return nullptr;
  }
  const auto *api = entry(BIFROST_ANDROID_WINDOW_ABI_V2);
  if (!api || api->abi_version != BIFROST_ANDROID_WINDOW_ABI_V2 ||
      api->struct_size < BIFROST_ANDROID_WINDOW_PLUGIN_V2_MIN_SIZE) {
    error = "window plugin has an incompatible ABI version or descriptor size";
    return nullptr;
  }
  if (!api->name || !api->name[0] || strnlen(api->name, 129) > 128 ||
      !api->create || !api->destroy || !api->open || !api->close ||
      !api->resize || !api->binding || !api->poll) {
    error = "window plugin descriptor has an invalid name or missing callback";
    return nullptr;
  }
  auto backend = std::make_unique<PluginWindowBackend>(std::move(module), *api);
  int result = backend->create();
  if (result || !backend->has_instance()) {
    error = "window plugin instance creation failed: " +
            std::string(result ? strerror(-result) : "NULL instance");
    return nullptr;
  }
  return backend;
}
} // namespace frost
