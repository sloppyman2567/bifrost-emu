#include "frost/android_window_backend.hpp"
#include "bifrost/android_window_plugin.h"
#include <cerrno>
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
  PluginWindowBackend(Module module, const bifrost_android_window_plugin_v1 &api)
      : module_(std::move(module)), api_(api), name_(api.name) {}
  ~PluginWindowBackend() override {
    if (instance_) {
      if (created_) api_.detach(instance_);
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
  int attach_host_window(void *window) override {
    return window ? status(api_.attach(instance_, window)) : -EINVAL;
  }
  void detach_host_window() noexcept override { api_.detach(instance_); }
  int resize(int32_t width, int32_t height, bool resize_host) override {
    if (width <= 0 || height <= 0) return -EINVAL;
    return status(api_.resize(instance_, width, height, resize_host ? 1u : 0u));
  }
  AndroidWindowBinding binding() override {
    bifrost_android_window_binding_v1 out{};
    if (status(api_.binding(instance_, &out)) || !out.window) return {};
    return {out.window, out.display};
  }

private:
  Module module_;
  // Copy the validated v1 prefix: callbacks cannot change under the host.
  bifrost_android_window_plugin_v1 api_;
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
  const auto *api = entry(BIFROST_ANDROID_WINDOW_ABI_V1);
  if (!api || api->abi_version != BIFROST_ANDROID_WINDOW_ABI_V1 ||
      api->struct_size < sizeof(bifrost_android_window_plugin_v1)) {
    error = "window plugin has an incompatible ABI version or descriptor size";
    return nullptr;
  }
  if (!api->name || !api->name[0] || strnlen(api->name, 129) > 128 ||
      !api->create || !api->destroy || !api->attach || !api->detach ||
      !api->resize || !api->binding) {
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
