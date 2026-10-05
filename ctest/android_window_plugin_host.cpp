#include "frost/android_surface.hpp"
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
  for (const char *invalid : {"null-api", "bad-version", "bad-size", "missing-callback", "empty-name", "create-null", "create-fail"}) {
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
    int first_window, second_window;
    CHECK(manager.set_host_sdl_window(&first_window));
    CHECK(manager.host_native_window() == (uint64_t(800) << 32 | 600));
    CHECK(manager.native_display() == &first_window);
    mode("attach-fail");
    CHECK(!manager.set_host_sdl_window(&second_window));
    CHECK(manager.uses_host_window(&first_window) && manager.native_display() == &first_window);
    mode("resize-fail");
    CHECK(manager.set_buffers_geometry(320, 240, 2) == -ENOTSUP);
    CHECK(manager.width() == 800 && manager.height() == 600 && manager.format() == 1);
    mode("positive-status");
    CHECK(manager.set_buffers_geometry(320, 240, 0) == -EPROTO);
    mode("binding-fail");
    CHECK(manager.host_native_window() == 0 && manager.native_display() == nullptr);
    mode("");
    CHECK(manager.set_buffers_geometry(320, 240, 2) == 0);
    CHECK(manager.host_native_window() == (uint64_t(320) << 32 | 240));
    CHECK(manager.set_buffers_geometry(-1, 240, 0) == -EINVAL);
    CHECK(manager.set_buffers_geometry(0, 240, 0) == -EINVAL);
    CHECK(manager.set_buffers_geometry(0, 0, 0) == 0);
    CHECK(manager.width() == 800 && manager.height() == 600 && manager.format() == 1);
    CHECK(manager.host_native_window() == (uint64_t(800) << 32 | 600));
    CHECK(contents(log).find("unload") == std::string::npos);
    CHECK(manager.from_surface() != 0);
    CHECK(!manager.install_window_backend(frost::make_sdl_android_window_backend()));
    // Destructor must detach the borrowed window while the DSO is resident.
  }
  CHECK(contents(log) == "create\nattach\ndetach\ndestroy\nunload\n");
  // Multiple instances must retain independent geometry and module references.
  std::ofstream(log, std::ios::trunc).close();
  auto first = std::make_unique<frost::AndroidSurfaceManager>(
      frost::load_android_window_backend(argv[1], error));
  auto second = std::make_unique<frost::AndroidSurfaceManager>(
      frost::load_android_window_backend(argv[1], error));
  int first_window, second_window;
  CHECK(first->set_host_sdl_window(&first_window));
  CHECK(second->set_host_sdl_window(&second_window));
  CHECK(first->set_buffers_geometry(128, 64, 0) == 0);
  CHECK(second->host_native_window() == (uint64_t(800) << 32 | 600));
  first.reset();
  CHECK(contents(log).find("unload") == std::string::npos);
  CHECK(second->native_display() == &second_window);
  second.reset();
  CHECK(contents(log) == "create\ncreate\nattach\nattach\ndetach\ndestroy\ndetach\ndestroy\nunload\n");
  std::puts("android_window_plugin_host: checks passed");
}
