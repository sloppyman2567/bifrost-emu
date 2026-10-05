#include "frost/android_window_backend.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

// Stub only the SDL boundary using its actual public declarations. The backend
// and dlopen helper library execute unchanged, without needing a compositor.
static bool fail_wm_info = false;
extern "C" SDL_bool SDL_GetWindowWMInfo(SDL_Window *, SDL_SysWMinfo *info) {
  if (fail_wm_info) return SDL_FALSE;
  info->subsystem = SDL_SYSWM_WAYLAND;
  info->info.wl.display = reinterpret_cast<wl_display *>(0x1234);
  info->info.wl.surface = reinterpret_cast<wl_surface *>(0x5678);
  return SDL_TRUE;
}
extern "C" const char *SDL_GetError(void) { return "injected WM query failure"; }
extern "C" void SDL_SetWindowSize(SDL_Window *, int, int) {}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "SDL lifetime check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
static std::string contents(const char *path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
  char log[] = "/tmp/bifrost-wl-lifetime-XXXXXX";
  int fd = mkstemp(log);
  CHECK(fd >= 0);
  close(fd);
  struct Cleanup { const char *path; ~Cleanup() { unlink(path); unsetenv("BIFROST_TEST_WL_LOG"); unsetenv("BIFROST_TEST_WL_CREATE_FAIL"); } } cleanup{log};
  setenv("BIFROST_TEST_WL_LOG", log, 1);
  int first_window, second_window;
  auto first = frost::make_sdl_android_window_backend();
  auto second = frost::make_sdl_android_window_backend();
  CHECK(first->attach_host_window(&first_window) == 0);
  CHECK(second->attach_host_window(&second_window) == 0);
  fail_wm_info = true;
  CHECK(first->binding().window == 0);
  fail_wm_info = false;
  auto one = first->binding(), two = second->binding();
  CHECK(one.window && two.window && one.window != two.window && one.display == two.display);
  first->detach_host_window();
  first.reset();
  CHECK(contents(log) == "load\ncreate\ncreate\ndestroy\n");
  CHECK(second->resize(320, 240, false) == 0);
  second.reset();
  CHECK(contents(log) == "load\ncreate\ncreate\ndestroy\nresize\ndestroy\nunload\n");
  // A failed wrapper allocation must return an empty pair, release its helper
  // reference and permit recovery without reattaching the borrowed window.
  std::ofstream(log, std::ios::trunc).close();
  first = frost::make_sdl_android_window_backend();
  CHECK(first->attach_host_window(&first_window) == 0);
  setenv("BIFROST_TEST_WL_CREATE_FAIL", "1", 1);
  one = first->binding();
  CHECK(!one.window && !one.display);
  CHECK(contents(log) == "load\nunload\n");
  unsetenv("BIFROST_TEST_WL_CREATE_FAIL");
  CHECK(first->binding().window != 0);
  first.reset();
  CHECK(contents(log) == "load\nunload\nload\ncreate\ndestroy\nunload\n");
  std::puts("android_sdl_backend_lifetime: checks passed");
}
