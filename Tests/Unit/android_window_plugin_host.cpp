#include "System/Android/Surface/Surface.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "plugin check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
std::string contents(const char *path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void mode(const char *value) { setenv("BIFROST_TEST_PLUGIN_MODE", value, 1); }
int main(int argc, char **argv) {
  CHECK(argc == 3);
  char log[] = "/tmp/bifrost-window-plugin-XXXXXX";
  int fd = mkstemp(log);
  CHECK(fd >= 0);
  close(fd);
  struct Cleanup { const char *path; ~Cleanup() { unlink(path); unsetenv("BIFROST_TEST_PLUGIN_LOG"); unsetenv("BIFROST_TEST_PLUGIN_MODE"); } } cleanup{log};
  setenv("BIFROST_TEST_PLUGIN_LOG", log, 1);
  std::string error;
  CHECK(!frost::load_android_window_backend("soname.so", error) && error.find("explicit host path") != std::string::npos);
  CHECK(!frost::load_android_window_backend("/does/not/exist.so", error) && !error.empty());
  CHECK(!frost::load_android_window_backend(argv[2], error) && error.find("entry point") != std::string::npos);
  for (const char *invalid : {"null-api", "bad-version", "legacy-version", "bad-size", "missing-callback", "empty-name", "create-null", "create-fail"}) {
    mode(invalid);
    std::ofstream(log, std::ios::trunc).close();
    CHECK(!frost::load_android_window_backend(argv[1], error) && !error.empty());
    std::string events = contents(log);
    CHECK(events.size() >= 7 && events.substr(events.size() - 7) == "unload\n");
    if (std::string(invalid) == "create-fail") CHECK(events == "create\ndestroy\nunload\n");
  }
  mode("");
  std::ofstream(log, std::ios::trunc).close();
  auto backend = frost::load_android_window_backend(argv[1], error);
  CHECK(backend && error.empty());
  CHECK(std::string(backend->name()) == "c-fixture");
  {
    frost::AndroidSurfaceManager manager(std::move(backend));
    CHECK(manager.open_host_window());
    CHECK(manager.host_native_window() == (uint64_t(800) << 32 | 600));
    CHECK(manager.native_display() != nullptr);
    mode("resize-fail");
    CHECK(manager.set_buffers_geometry(320, 240, 1) == -ENOTSUP);
    CHECK(manager.width() == 800 && manager.height() == 600 && manager.format() == 1);
    mode("positive-status");
    CHECK(manager.set_buffers_geometry(320, 240, 0) == -EPROTO);
    mode("binding-fail");
    CHECK(manager.host_native_window() == 0 && manager.native_display() == nullptr);
    mode("");
    CHECK(manager.set_buffers_geometry(320, 240, 1) == 0);
    CHECK(manager.host_native_window() == (uint64_t(320) << 32 | 240));
    CHECK(manager.set_buffers_geometry(-1, 240, 0) == -EINVAL);
    CHECK(manager.set_buffers_geometry(0, 240, 0) == -EINVAL);
    CHECK(manager.set_buffers_geometry(0, 0, 0) == 0);
    CHECK(manager.width() == 800 && manager.height() == 600 && manager.format() == 1);
    CHECK(manager.host_native_window() == (uint64_t(800) << 32 | 600));
    CHECK(contents(log).find("unload") == std::string::npos);
    CHECK(manager.from_surface() != 0);
    uint64_t input = 0;
    // Concurrent touches form one gesture, as InputReader reports them:
    // pointers in ascending id order, the changed pointer's index in the
    // action, the lowest free id for a new touch and the leaving pointer in
    // its up event. Hover while touched and a move for no touch are dropped.
    mode("poll-two-fingers");
    manager.pump_host_events(nullptr);
    mode("");
    struct Expected { int32_t action; std::vector<int32_t> ids; float x0; int64_t time_ms; };
    const Expected gesture[] = {
        {0, {0}, 10, 1000}, {0x105, {0, 1}, 10, 1001}, {2, {0, 1}, 12, 1002},
        {6, {0, 1}, 12, 1004}, {5, {0, 1}, 50, 1005}, {0x106, {0, 1}, 50, 1006},
        {1, {0}, 50, 1007},
    };
    for (const Expected &expected : gesture) {
      CHECK(manager.queue_get_event(manager.input_queue_handle(), &input) == 0);
      CHECK(manager.motion_action(input) == expected.action);
      CHECK(manager.motion_pointer_count(input) == int32_t(expected.ids.size()));
      for (size_t i = 0; i < expected.ids.size(); ++i)
        CHECK(manager.motion_pointer_id(input, i) == expected.ids[i]);
      CHECK(manager.motion_x(input, 0) == expected.x0);
      CHECK(manager.motion_event_time(input) == expected.time_ms * 1000000);
      CHECK(manager.motion_down_time(input) == 1000000000);
      manager.queue_finish_event(manager.input_queue_handle(), input);
    }
    CHECK(manager.queue_get_event(manager.input_queue_handle(), &input) != 0);
    // NDK event times are System.nanoTime nanoseconds; the plugin reports ms.
    mode("poll-pointer-down");
    manager.pump_host_events(nullptr);
    mode("");
    CHECK(manager.queue_get_event(manager.input_queue_handle(), &input) == 0);
    CHECK(manager.motion_event_time(input) == 1234000000);
    CHECK(manager.motion_down_time(input) == 1234000000);
    CHECK(!manager.install_window_backend(frost::make_sdl_android_window_backend()));
    // Destructor must close the owned window while the DSO is resident.
  }
  CHECK(contents(log) == "create\nopen\nclose\ndestroy\nunload\n");
  // Poll has its own positive-success contract; poison is discarded.
  auto polling = frost::load_android_window_backend(argv[1], error);
  CHECK(polling);
  frost::AndroidWindowEvent event{};
  for (const char *invalid : {"poll-invalid", "poll-positive", "poll-fail", "poll-nan", "poll-key-invalid"}) {
    mode(invalid);
    CHECK(polling->poll(event) < 0 && event.type == 0);
  }
  mode("poll-resize");
  CHECK(polling->poll(event) == 1 && event.width == 640 && event.height == 480);
  mode("");
  CHECK(polling->poll(event) == 0 && event.type == 0);
  // Optional popup callbacks receive the guest's pixels unchanged.
  std::ofstream(log, std::ios::trunc).close();
  const uint32_t pixels[] = {0x80ff0000, 0xff00ff00};
  CHECK(polling->show_popup(3, 10, -4, 2, 1, pixels) == 0);
  CHECK(polling->show_popup(3, 0, 0, 0, 1, pixels) == -EINVAL);
  polling->remove_popup(3);
  CHECK(contents(log) == "popup 3 10,-4 2x1 80ff0000\nunpopup 3\n");
  polling.reset();
  // An original-size v2 descriptor still loads; its trailing fields are ignored.
  mode("old-size");
  auto old = frost::load_android_window_backend(argv[1], error);
  CHECK(old && error.empty());
  std::ofstream(log, std::ios::trunc).close();
  CHECK(old->show_popup(1, 0, 0, 1, 1, pixels) == -ENOTSUP);
  old->remove_popup(1);
  CHECK(contents(log).find("popup") == std::string::npos);
  old.reset();
  mode("");
  // Multiple instances must retain independent geometry and module references.
  std::ofstream(log, std::ios::trunc).close();
  auto first = std::make_unique<frost::AndroidSurfaceManager>(
      frost::load_android_window_backend(argv[1], error));
  auto second = std::make_unique<frost::AndroidSurfaceManager>(
      frost::load_android_window_backend(argv[1], error));
  CHECK(first->open_host_window());
  CHECK(second->open_host_window());
  CHECK(first->set_buffers_geometry(128, 64, 0) == 0);
  CHECK(second->host_native_window() == (uint64_t(800) << 32 | 600));
  first.reset();
  CHECK(contents(log).find("unload") == std::string::npos);
  CHECK(second->native_display() != nullptr);
  second.reset();
  CHECK(contents(log) == "create\ncreate\nopen\nopen\nclose\ndestroy\nclose\ndestroy\nunload\n");
  std::puts("android_window_plugin_host: checks passed");
}
