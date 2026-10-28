#include "System/Android/Window/WindowBackend.hpp"
#include "Bifrost/AndroidWindowPlugin.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <SDL3/SDL.h>

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
  // open() takes desktop units; a high-density window on the 2x output has
  // twice as many pixels, which window stats and the Android display use.
  CHECK(first->window_info(info) && info.title == "APK name" && info.w == 1600 && info.h == 1200);
  int32_t pixel_w = 0, pixel_h = 0;
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1600 && pixel_h == 1200);
  CHECK(first->pixel_density() == 2.0);
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
  const auto before_key = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  CHECK(first->poll(event) == 1 && event.key_code == 29 && event.meta_state == 0x1001);
  const auto after_key = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  CHECK(event.time_ms >= before_key && event.time_ms <= after_key);
  setenv("BIFROST_TEST_SDL_EVENT", "pointer", 1);
  CHECK(first->poll(event) == 1 && event.x == 20 && event.y == 40 && event.action == 0);
  setenv("BIFROST_TEST_SDL_EVENT", "other-window", 1);
  CHECK(first->poll(event) == 0 && event.type == 0);
  // The left button is still down from above: a wheel step is not a pinch then.
  setenv("BIFROST_TEST_SDL_EVENT", "gesture-wheel-held:1", 1);
  CHECK(first->poll(event) == 0 && event.type == 0);
  // Gestures arrive as synthetic fingers around the cursor, in order, whatever else is polled. The window is 1600x1200
  // pixels (2x), the cursor at (800, 600), the fingers 0.20 of the smaller side (240 px) apart before the step.
  const int32_t kA = 0x7ffffff0, kB = 0x7ffffff1;
  auto near = [](float a, float b) { return a - b < 0.5f && b - a < 0.5f; };
  auto next = [&](const char *mode) { setenv("BIFROST_TEST_SDL_EVENT", mode, 1); return first->poll(event); };
  auto step = [&](int32_t finger, int32_t action, float x, float y) {
    return first->poll(event) == 1 && event.type == BIFROST_WINDOW_POINTER && event.pointer_id == finger &&
           event.action == action && near(event.x, x) && near(event.y, y);
  };
  CHECK(next("gesture-left-up") == 1 && event.action == 1);
  CHECK(next("gesture-wheel:1") == 1 && event.pointer_id == kA && event.action == 0 && near(event.x, 680));
  CHECK(step(kB, 0, 920, 600) && step(kA, 2, 665.6f, 600) && step(kB, 2, 934.4f, 600));
  CHECK(first->poll(event) == 0);
  // The fingers lift once the wheel has been quiet for a moment, the second one first.
  usleep(250000);
  CHECK(step(kB, 1, 934.4f, 600) && step(kA, 1, 665.6f, 600) && first->poll(event) == 0);
  // A natural-scrolling (flipped) wheel zooms the same way for the same physical motion: its values are negated.
  CHECK(next("gesture-wheel-flipped:-1") == 1 && event.pointer_id == kA && event.action == 0);
  CHECK(step(kB, 0, 920, 600) && step(kA, 2, 665.6f, 600) && step(kB, 2, 934.4f, 600));
  // A button press lifts them before its own event is delivered.
  CHECK(next("gesture-left-down") == 1 && event.pointer_id == kB && event.action == 1 && near(event.x, 934.4f));
  CHECK(step(kA, 1, 665.6f, 600));
  CHECK(first->poll(event) == 1 && event.pointer_id == 0 && event.action == 0 && near(event.x, 20));
  CHECK(next("gesture-left-up") == 1 && event.pointer_id == 0 && event.action == 1);
  // Shift pans: the wheel moves both fingers 0.05 of the smaller side (60 px) per step, content following the wheel.
  setenv("BIFROST_TEST_SDL_SHIFT", "1", 1);
  CHECK(next("gesture-wheel-pan:1") == 1 && event.pointer_id == kA && event.action == 0);
  CHECK(step(kB, 0, 920, 600) && step(kA, 2, 680, 660) && step(kB, 2, 920, 660));
  unsetenv("BIFROST_TEST_SDL_SHIFT");
  // Another kind of event ends it at once.
  CHECK(next("gesture-left-down") == 1 && event.action == 1 && event.pointer_id == kB);
  CHECK(step(kA, 1, 680, 660));
  CHECK(first->poll(event) == 1 && event.pointer_id == 0 && event.action == 0);
  CHECK(next("gesture-left-up") == 1 && event.action == 1);
  // A held middle button drags both fingers with the cursor (hover moves are not reported while it does).
  CHECK(next("gesture-middle-down") == 1 && event.pointer_id == kA && event.action == 0);
  CHECK(step(kB, 0, 920, 600));
  CHECK(next("gesture-move-held") == 1 && event.pointer_id == kA && event.action == 2 && near(event.x, 700) && near(event.y, 620));
  CHECK(step(kB, 2, 940, 620) && first->poll(event) == 0);
  CHECK(next("gesture-middle-up") == 1 && event.pointer_id == kB && event.action == 1);
  CHECK(step(kA, 1, 700, 620) && first->poll(event) == 0);
  // After it the cursor moves are ordinary hover events again.
  CHECK(next("gesture-move") == 1 && event.pointer_id == 0 && event.action == 7);
#if SDL_VERSION_ATLEAST(3, 4, 0)
  // A trackpad pinch spreads the fingers around the cursor, by the event's scale.
  CHECK(next("gesture-pinch:1.5") == 1 && event.pointer_id == kA && event.action == 0);
  CHECK(step(kB, 0, 940, 620) && step(kA, 2, 640, 620) && step(kB, 2, 1000, 620));
  unsetenv("BIFROST_TEST_SDL_EVENT");
  first->close();  // a gesture still running when the window closes sends nothing more
  CHECK(first->poll(event) == 0);
  first->close(); first.reset();
#else
  unsetenv("BIFROST_TEST_SDL_EVENT");
  first->close(); first.reset();
#endif
  // Opening keeps the requested desktop size: no host resize follows creation.
  CHECK(contents(log) == "video-init\nwindow-create\nvideo-init\nwindow-create\nwindow-icon\nload\ncreate\ncreate\ndestroy\nwindow-destroy\nvideo-quit\n");
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
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("orientation", 800, 600) == 0);
  CHECK(first->set_orientation(3) < 0);
  CHECK(first->set_fixed_size(1600, 1200) == 0);
  CHECK(first->set_orientation(1) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1200 && pixel_h == 1600);
  CHECK(first->set_orientation(2) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1600 && pixel_h == 1200);
  setenv("BIFROST_TEST_SDL_ASPECT_FAIL", "1", 1);
  CHECK(first->set_orientation(1) < 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1600 && pixel_h == 1200);
  unsetenv("BIFROST_TEST_SDL_ASPECT_FAIL");
  CHECK(first->set_orientation(0) == 0);
  first.reset();
  // A compositor that clamps a new window to its work area must not change the shape the app asked for: a phone
  // window of 720x1280 desktop units on a 720x692 area is 389x692, not a near-square a fixed-size app keeps forever.
  setenv("BIFROST_TEST_SDL_WORK_AREA", "720x692", 1);
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("clamped portrait", 720, 1280) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 778 && pixel_h == 1384);
  first.reset();
  setenv("BIFROST_TEST_SDL_WORK_AREA", "1720x692", 1);
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("clamped landscape", 1280, 720) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 2460 && pixel_h == 1384);
  first.reset();
  // A fixed-size app is the virtual panel: rotating it gives the panel's shape fitted to the work area, not its
  // clamped pixels swapped (Roblox opens in a menu, then turns the window to landscape for a game).
  setenv("BIFROST_TEST_SDL_WORK_AREA", "1720x692", 1);
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("panel", 1280, 720) == 0);
  CHECK(first->set_fixed_size(2460, 1384) == 0);
  CHECK(first->set_orientation(1) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 778 && pixel_h == 1384);
  CHECK(first->set_orientation(2) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 2460 && pixel_h == 1384);
  first.reset();
  // A window that fits is untouched.
  setenv("BIFROST_TEST_SDL_WORK_AREA", "3440x1440", 1);
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("fits", 1280, 720) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 2560 && pixel_h == 1440);
  first.reset();
  unsetenv("BIFROST_TEST_SDL_WORK_AREA");
  // Resizable apps rotate native pixels too and can release the constraint.
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("resizable orientation", 800, 600) == 0);
  CHECK(first->set_orientation(1) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1200 && pixel_h == 1600);
  CHECK(first->set_orientation(0) == 0);
  CHECK(first->resize(1600, 1200, true) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1600 && pixel_h == 1200);
  first.reset();
  // Pointer shapes: system cursors made on first use and reused, hiding and showing, and every
  // cursor freed (and the pointer shown again) before SDL quits.
  {
    const std::string pointer = std::to_string(SDL_SYSTEM_CURSOR_POINTER), text = std::to_string(SDL_SYSTEM_CURSOR_TEXT);
    auto shapes = frost::make_sdl_android_window_backend();
    CHECK(shapes->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == -ENODEV);  // no window yet
    CHECK(shapes->open("cursors", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    CHECK(shapes->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == 0);
    CHECK(shapes->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == 0);  // reused, not created again
    CHECK(shapes->set_cursor(BIFROST_WINDOW_CURSOR_HIDDEN) == 0);
    CHECK(shapes->set_cursor(BIFROST_WINDOW_CURSOR_TEXT) == 0);     // shown again
    CHECK(shapes->set_cursor(-1) == -EINVAL && shapes->set_cursor(BIFROST_WINDOW_CURSOR_COUNT) == -EINVAL);
    shapes.reset();
    CHECK(contents(log) == "cursor-create " + pointer + "\ncursor-set " + pointer + "\ncursor-set " + pointer +
                           "\ncursor-hide\ncursor-create " + text + "\ncursor-set " + text + "\ncursor-show\n"
                           "cursor-destroy " + text + "\ncursor-destroy " + pointer + "\nwindow-destroy\nvideo-quit\n");
    // Closing while hidden leaves the pointer visible, with no cursor of ours to free.
    auto hidden = frost::make_sdl_android_window_backend();
    CHECK(hidden->open("hidden", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    CHECK(hidden->set_cursor(BIFROST_WINDOW_CURSOR_HIDDEN) == 0);
    hidden.reset();
    CHECK(contents(log) == "cursor-hide\ncursor-show\nwindow-destroy\nvideo-quit\n");
  }
  for (int i = 0; i < 256; ++i) {
    first = frost::make_sdl_android_window_backend();
    CHECK(first->open("stress", 800, 600) == 0 && first->binding().window);
    CHECK(first->resize(320, 240, true) == 0);
    first->close(); first->close(); first.reset();
  }
  std::puts("android_sdl_backend_lifetime: SDL3 checks and 256 teardown cycles passed");
}
