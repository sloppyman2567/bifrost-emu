#include "System/Android/Surface/Surface.hpp"
#include <cstdio>
#include <memory>
#include <cerrno>

namespace {
struct State {
  std::string title = "Bifrost Android";
  bool opened = false;
  int width = 0, height = 0;
  bool host_resize = false;
  bool fail_open = false, fail_resize = false;
  uint64_t binding = 0x123;
  void* display = nullptr;
  bool hidden = false;
  std::vector<std::shared_ptr<State>> children;
};
class TestBackend final : public frost::AndroidWindowBackend {
public:
  explicit TestBackend(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  ~TestBackend() override { close(); }
  const char *name() const noexcept override { return "test-window"; }
  int open(const char *, int32_t, int32_t) override {
    if (state_->fail_open) return -EIO;
    state_->opened = true;
    return 0;
  }
  void close() noexcept override { state_->opened = false; }
  int poll(frost::AndroidWindowEvent &event) override { event = {}; return 0; }
  int resize(int32_t w, int32_t h, bool host) override {
    if (state_->fail_resize) return -ENOTSUP;
    state_->width = w;
    state_->height = h;
    state_->host_resize = host;
    return 0;
  }
  std::unique_ptr<frost::AndroidWindowBackend> create_surface(int32_t w, int32_t h) override {
    auto child = std::make_shared<State>();
    child->binding = state_->binding + state_->children.size() + 1;
    child->display = state_.get();
    auto backend = std::make_unique<TestBackend>(child);
    backend->open("layer", w, h);
    backend->resize(w, h, false);
    state_->children.push_back(child);
    return backend;
  }
  void set_visible(bool visible) override { state_->hidden = !visible; }
  bool window_info(arm64emu::window_stats::WindowInfo &info) override {
    info.title = state_->title;
    info.w = state_->width; info.h = state_->height;
    return state_->opened;
  }
  void set_title(const char *title) override { state_->title = title; }
  frost::AndroidWindowBinding binding() override {
    return state_->opened ? frost::AndroidWindowBinding{state_->binding, state_->display ? state_->display : state_.get()}
                          : frost::AndroidWindowBinding{};
  }

private:
  std::shared_ptr<State> state_;
};
} // namespace
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "backend check failed: %s\n", #x);                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main() {
  auto &manager = frost::AndroidSurfaceManager::instance();
  CHECK(manager.host_native_window() == 0);
  CHECK(!manager.install_window_backend(nullptr));
  auto state = std::make_shared<State>();
  CHECK(manager.install_window_backend(std::make_unique<TestBackend>(state)));
  CHECK(state->width == 800 && state->height == 600 && !state->host_resize);
  state->fail_open = true;
  CHECK(!manager.open_host_window() && !manager.ready());
  state->fail_open = false;
  CHECK(manager.open_host_window());
  CHECK(manager.ready() && manager.host_native_window() == 0x123);
  CHECK(manager.native_display() == state.get());
  CHECK(!manager.install_window_backend(
      frost::make_sdl_android_window_backend()));
  CHECK(manager.set_buffers_geometry(320, 240, 0) == 0);
  CHECK(state->width == 320 && state->height == 240 && state->host_resize);
  state->fail_resize = true;
  CHECK(manager.set_buffers_geometry(100, 200, 1) == -ENOTSUP);
  CHECK(manager.width() == 320 && manager.height() == 240 && manager.format() == 1);
  CHECK(manager.handle_host_resize(640, 480) == -ENOTSUP);
  CHECK(manager.width() == 320 && manager.height() == 240);
  state->fail_resize = false;
  CHECK(manager.set_buffers_geometry(320, 240, 2) == -ENOTSUP);
  CHECK(manager.handle_host_resize(640, 480) == 0);
  CHECK(manager.width() == 640 && manager.height() == 480 && !state->host_resize);
  CHECK(manager.set_buffers_geometry(-1, 240, 0) == -EINVAL);
  CHECK(manager.set_buffers_geometry(0, 240, 0) == -EINVAL);
  CHECK(manager.set_buffers_geometry(320, 0, 0) == -EINVAL);
  CHECK(manager.set_buffers_geometry(320, 240, -1) == -EINVAL);
  CHECK(manager.set_buffers_geometry(320, 240, 1) == 0);
  CHECK(manager.set_buffers_geometry(0, 0, 0) == 0);
  CHECK(manager.width() == 800 && manager.height() == 600 && manager.format() == 1);
  CHECK(manager.set_buffers_geometry(320, 240, 0) == 0);
  auto primary = manager.from_surface(100);
  CHECK(primary != 0 && manager.from_surface(100) == primary);
  auto layer = manager.from_surface(200);
  CHECK(layer != 0 && layer != primary);
  CHECK(!manager.surface_opaque(primary) && !manager.surface_opaque(layer));
  CHECK(manager.set_surface_opaque(primary, true) == 0);
  CHECK(manager.surface_opaque(primary) && !manager.surface_opaque(layer));
  CHECK(manager.set_surface_opaque(0, true) == -EINVAL);
  CHECK(manager.host_native_window(layer) != manager.host_native_window(primary));
  CHECK(manager.set_buffers_geometry(123, 45, 1, layer) == 0);
  CHECK(manager.width(layer) == 123 && manager.height(layer) == 45);
  CHECK(manager.width(primary) == 320 && manager.height(primary) == 240);
  manager.register_egl_surface(900, layer);
  CHECK(!manager.present_egl_surface(901));
  CHECK(manager.present_egl_surface(900));
  manager.pump_host_events(nullptr);
  CHECK(state->title.find("Android EGL") != std::string::npos);
  CHECK(state->title.find("frame 1") != std::string::npos);
  CHECK(manager.set_surface_opaque(layer, true) == -EBUSY);
  CHECK(manager.set_surface_opaque(layer, false) == 0);
  manager.release(layer);
  CHECK(manager.host_native_window(layer) != 0 && state->children[0]->opened);
  manager.destroy_egl_surface(900);
  CHECK(!manager.present_egl_surface(900));
  CHECK(manager.host_native_window(layer) == 0 && !state->children[0]->opened);
  CHECK(manager.width(layer) == -EINVAL);
  auto replacement = manager.from_surface(200);
  CHECK(replacement && replacement != layer);
  manager.release(replacement);
  manager.close_host_window();
  CHECK(!state->opened && !manager.ready() && !manager.host_native_window());
  // Guest handles can remain live after close; replacing their backend
  // would change the meaning of existing EGL surfaces.
  CHECK(!manager.install_window_backend(
      frost::make_sdl_android_window_backend()));
  // An independent adapter context owns its plugin and closes on teardown.
  auto independent = std::make_shared<State>();
  {
    frost::AndroidSurfaceManager local(std::make_unique<TestBackend>(independent));
    CHECK(local.open_host_window());
    CHECK(local.width() == 800 && manager.width() == 320);
    CHECK(local.from_surface() != 0);
  }
  CHECK(!independent->opened);
  std::puts("android_window_backend_host: checks passed");
}
