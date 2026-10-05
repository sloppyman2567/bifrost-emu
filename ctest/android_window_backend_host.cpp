#include "frost/android_surface.hpp"
#include <cstdio>
#include <memory>

namespace {
struct State {
  void *window = nullptr;
  int width = 0, height = 0;
  bool host_resize = false;
};
class TestBackend final : public frost::AndroidWindowBackend {
public:
  explicit TestBackend(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
  const char *name() const noexcept override { return "test-window"; }
  void attach_host_window(void *window) override { state_->window = window; }
  void resize(int32_t w, int32_t h, bool host) override {
    state_->width = w;
    state_->height = h;
    state_->host_resize = host;
  }
  frost::AndroidWindowBinding binding() override {
    return state_->window ? frost::AndroidWindowBinding{0x123, state_->window}
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
  int borrowed_window;
  manager.set_host_sdl_window(&borrowed_window);
  CHECK(manager.ready() && manager.host_native_window() == 0x123);
  CHECK(manager.native_display() == &borrowed_window);
  CHECK(!manager.install_window_backend(
      frost::make_sdl_android_window_backend()));
  CHECK(manager.set_buffers_geometry(320, 240, 0) == 0);
  CHECK(state->width == 320 && state->height == 240 && state->host_resize);
  CHECK(manager.from_surface() != 0);
  manager.set_host_sdl_window(nullptr);
  CHECK(!state->window && !manager.ready() && !manager.host_native_window());
  // Guest handles can remain live after detach; replacing their backend
  // would change the meaning of existing EGL surfaces.
  CHECK(!manager.install_window_backend(
      frost::make_sdl_android_window_backend()));
  std::puts("android_window_backend_host: checks passed");
}
