#include "frost/android_surface.hpp"
#include <cstdio>
#include <memory>
#include <cerrno>

namespace {
struct State {
  bool opened = false;
  int width = 0, height = 0;
  bool host_resize = false;
  bool fail_open = false, fail_resize = false;
};
class TestBackend final : public frost::AndroidWindowBackend {
public:
  explicit TestBackend(std::shared_ptr<State> state)
      : state_(std::move(state)) {}
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
  frost::AndroidWindowBinding binding() override {
    return state_->opened ? frost::AndroidWindowBinding{0x123, state_.get()}
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
  CHECK(manager.from_surface() != 0);
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
