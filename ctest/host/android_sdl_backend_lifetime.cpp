#include "frost/android_window_backend.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "SDL3 lifetime check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
static std::string contents(const char *path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
  char log[] = "/tmp/bifrost-wl-lifetime-XXXXXX";
  int fd = mkstemp(log);
  CHECK(fd >= 0); close(fd);
  struct Cleanup { const char *path; ~Cleanup() { unlink(path); } } cleanup{log};
  setenv("BIFROST_TEST_WL_LOG", log, 1);
  auto first = frost::make_sdl_android_window_backend();
  auto second = frost::make_sdl_android_window_backend();
  setenv("BIFROST_TEST_SDL_CREATE_FAIL", "1", 1);
  CHECK(first->open("first", 800, 600) < 0 && !first->binding().window);
  unsetenv("BIFROST_TEST_SDL_CREATE_FAIL");
  std::ofstream(log, std::ios::trunc).close();
  CHECK(first->open("first", 800, 600) == 0);
  CHECK(second->open("second", 800, 600) == 0);
  arm64emu::window_stats::WindowInfo info;
  first->set_title("APK name");
  CHECK(first->window_info(info) && info.title == "APK name" && info.w == 400 && info.h == 300);
  unsigned char icon[128 * 128 * 4]{};
  CHECK(first->set_icon(icon, 128, 128));
  setenv("BIFROST_TEST_SDL_BINDING_FAIL", "1", 1);
  CHECK(first->binding().window == 0);
  unsetenv("BIFROST_TEST_SDL_BINDING_FAIL");
  auto one = first->binding(), two = second->binding();
  CHECK(one.window && two.window && one.window != two.window && one.display == two.display);
  // Failed host resize never reaches the wrapper; native binding is stable.
  setenv("BIFROST_TEST_SDL_RESIZE_FAIL", "1", 1);
  CHECK(first->resize(320, 240, true) < 0);
  CHECK(first->binding().window == one.window);
  unsetenv("BIFROST_TEST_SDL_RESIZE_FAIL");
  frost::AndroidWindowEvent event{};
  setenv("BIFROST_TEST_SDL_EVENT", "key", 1);
  CHECK(first->poll(event) == 1 && event.key_code == 29 && event.meta_state == 0x1001 && event.time_ms == 123);
  setenv("BIFROST_TEST_SDL_EVENT", "pointer", 1);
  CHECK(first->poll(event) == 1 && event.x == 20 && event.y == 40 && event.action == 0);
  setenv("BIFROST_TEST_SDL_EVENT", "other-window", 1);
  CHECK(first->poll(event) == 0 && event.type == 0);
  unsetenv("BIFROST_TEST_SDL_EVENT");
  first->close(); first.reset();
  CHECK(contents(log) == "video-init\nwindow-create\nhost-resize\nvideo-init\nwindow-create\nhost-resize\nwindow-icon\nload\ncreate\ncreate\ndestroy\nwindow-destroy\nvideo-quit\n");
  CHECK(second->resize(320, 240, false) == 0);
  second.reset();
  CHECK(contents(log).find("resize\ndestroy\nunload\nwindow-destroy\nvideo-quit\n") != std::string::npos);
  // Wrapper failure can recover; its helper reference must not leak.
  std::ofstream(log, std::ios::trunc).close();
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("recover", 800, 600) == 0);
  setenv("BIFROST_TEST_WL_CREATE_FAIL", "1", 1);
  one = first->binding();
  CHECK(!one.window && !one.display);
  CHECK(contents(log).find("load\nunload\n") != std::string::npos);
  unsetenv("BIFROST_TEST_WL_CREATE_FAIL");
  CHECK(first->binding().window != 0);
  first.reset();
  CHECK(contents(log).find("create\ndestroy\nunload\nwindow-destroy\nvideo-quit\n") != std::string::npos);
  // Stress repeated window, wrapper, subsystem and module teardown.
  for (int i = 0; i < 256; ++i) {
    first = frost::make_sdl_android_window_backend();
    CHECK(first->open("stress", 800, 600) == 0 && first->binding().window);
    CHECK(first->resize(320, 240, true) == 0);
    first->close(); first->close(); first.reset();
  }
  std::puts("android_sdl_backend_lifetime: SDL3 checks and 256 teardown cycles passed");
}
