#include "System/Android/Window/WindowBackend.hpp"
#include "Bifrost/AndroidWindowPlugin.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
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
  // Visibility is reported once per change, from the window's flags and not the event that prompted the look:
  // X11 minimises and restores, Wayland has no minimise and a compositor suspends the window (occluded, exposed).
  {
    auto state = [&](const char *flags, const char *mode, int expected) {
      setenv("BIFROST_TEST_SDL_WINDOW_FLAGS", flags, 1);
      setenv("BIFROST_TEST_SDL_EVENT", mode, 1);
      const int result = first->poll(event);
      if (expected < 0) return result == 0 && event.type == 0;
      return result == 1 && event.type == BIFROST_WINDOW_VISIBILITY && event.action == expected;
    };
    CHECK(state("", "window:exposed", -1));              // already visible: nothing to report
    CHECK(state("minimized", "window:minimized", 0));
    CHECK(state("minimized", "window:minimized", -1));   // the same state is not reported twice
    CHECK(state("", "window:restored", 1));
    CHECK(state("occluded", "window:occluded", 0));
    CHECK(state("", "window:exposed", 1));
    CHECK(state("hidden", "window:hidden", 0));
    CHECK(state("", "window:shown", 1));
    CHECK(state("minimized", "window-other:minimized", -1));  // another window's state is not this one's
    CHECK(state("", "window:restored", -1));
    unsetenv("BIFROST_TEST_SDL_WINDOW_FLAGS");
  }
  // The desktop's colour scheme: the backend answers it, and reports a change of light and dark once, whenever the
  // window was opened in the other one. SDL answers unknown when it cannot tell, and that changes nothing.
  {
    auto scheme = [&](const char *theme, int expected) {
      setenv("BIFROST_TEST_SDL_THEME", theme, 1);
      setenv("BIFROST_TEST_SDL_EVENT", "theme", 1);
      const int result = first->poll(event);
      if (expected < 0) return result == 0 && event.type == 0;
      return result == 1 && event.type == BIFROST_WINDOW_THEME && event.action == expected;
    };
    CHECK(first->system_theme() == 1);                   // the fake desktop is light until told otherwise
    setenv("BIFROST_TEST_SDL_THEME", "dark", 1);
    CHECK(first->system_theme() == 2);
    unsetenv("BIFROST_TEST_SDL_THEME");
    CHECK(scheme("light", -1));                          // the window opened light: nothing changed
    CHECK(scheme("dark", 1));
    CHECK(scheme("dark", -1));                           // the same scheme is not reported twice
    CHECK(scheme("unknown", -1));                        // not knowing is not a change
    CHECK(scheme("light", 0));
    CHECK(scheme("dark", 1));
    unsetenv("BIFROST_TEST_SDL_THEME");
  }
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
  CHECK(contents(log) == "video-init\nwindow-create\ngamepad-init\nvideo-init\nwindow-create\ngamepad-init\nwindow-icon\nload\n"
                       "create\ncreate\ndestroy\ngamepad-quit\nwindow-destroy\nvideo-quit\n");
  CHECK(second->resize(320, 240, false) == 0);
  second.reset();
  CHECK(contents(log).find("resize\ndestroy\nunload\ngamepad-quit\nwindow-destroy\nvideo-quit\n") != std::string::npos);
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
  CHECK(contents(log).find("create\ndestroy\nunload\ngamepad-quit\nwindow-destroy\nvideo-quit\n") != std::string::npos);
  // Stress repeated window, wrapper, subsystem and module teardown.
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("orientation", 800, 600) == 0);
  CHECK(first->set_orientation(3) < 0);
  CHECK(first->set_fixed_size(1600, 1200) == 0);
  CHECK(first->set_orientation(1) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1200 && pixel_h == 1600);
  CHECK(first->set_orientation(2) == 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == 1600 && pixel_h == 1200);
  // A fixed-size app's window is never aspect-locked: a compositor could not tile it to a half or a corner, so the game
  // could only be maximized or kept at its opening size. The fit centres the panel in whatever shape the window takes.
  CHECK(contents(log).find("aspect-lock") == std::string::npos);
  CHECK(contents(log).find("aspect-free") != std::string::npos);
  setenv("BIFROST_TEST_SDL_ASPECT_FAIL", "1", 1);
  CHECK(first->set_orientation(1) == 0);                     // nothing to lock, so a failing lock call cannot fail it
  unsetenv("BIFROST_TEST_SDL_ASPECT_FAIL");
  CHECK(first->set_orientation(0) == 0);
  first.reset();
  // A resizable app that asks for an orientation still keeps its window in that shape, and an error setting the lock fails
  // the request and leaves the window as it was.
  std::ofstream(log, std::ios::trunc).close();
  first = frost::make_sdl_android_window_backend();
  CHECK(first->open("resizable orientation", 800, 600) == 0);
  int32_t open_w = 0, open_h = 0;
  CHECK(first->pixel_size(open_w, open_h) && open_w > open_h);
  CHECK(first->set_orientation(1) == 0);
  CHECK(contents(log).find("aspect-lock") != std::string::npos);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == open_h && pixel_h == open_w);   // turned to portrait
  setenv("BIFROST_TEST_SDL_ASPECT_FAIL", "1", 1);
  CHECK(first->set_orientation(2) < 0);
  CHECK(first->pixel_size(pixel_w, pixel_h) && pixel_w == open_h && pixel_h == open_w);   // left as it was
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
                           "gamepad-quit\ncursor-destroy " + text + "\ncursor-destroy " + pointer + "\nwindow-destroy\nvideo-quit\n");
    // Closing while hidden leaves the pointer visible, with no cursor of ours to free.
    auto hidden = frost::make_sdl_android_window_backend();
    CHECK(hidden->open("hidden", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    CHECK(hidden->set_cursor(BIFROST_WINDOW_CURSOR_HIDDEN) == 0);
    hidden.reset();
    CHECK(contents(log) == "cursor-hide\ngamepad-quit\ncursor-show\nwindow-destroy\nvideo-quit\n");
  }
  // The pad cursor (PadCursor is tested on its own): through the real handler, Start and Select together turn it on and take
  // the Start the game already saw back; A is then a touch at the middle of the panel and never reaches the game; in gamepad
  // mode none of it happens; in cursor mode it is on from the start. The picture needs a real display, so the binding fails.
  {
    struct Seen { int type, action, pointer, key; float x, y; };
    auto must = [](bool ok, const char *what) {   // CHECK returns from main; a lambda that returns events cannot use it
      if (!ok) { std::fprintf(stderr, "SDL3 pad cursor check failed: %s\n", what); std::exit(1); }
    };
    auto play = [&](const char *script, int mode) {
      frost::set_pad_cursor_mode(mode);
      setenv("BIFROST_TEST_SDL_BINDING_FAIL", "1", 1);
      auto pads = frost::make_sdl_android_window_backend();
      must(pads->open("pad cursor", 800, 600) == 0, "open");
      setenv("BIFROST_TEST_SDL_EVENT", script, 1);
      std::vector<Seen> seen;
      for (int polls = 0; polls < 64; ++polls) {
        frost::AndroidWindowEvent e{};
        if (pads->poll(e) != 1) continue;
        if (e.type == BIFROST_WINDOW_GAMEPAD) {
          frost::AndroidWindowGamepad pad{};
          must(pads->poll_gamepad(pad) == 1, "poll_gamepad");
          seen.push_back({int(e.type), int(pad.action), 0, pad.kind == BIFROST_GAMEPAD_BUTTON ? int(pad.key_code) : -int(pad.kind), 0, 0});
        } else if (e.type == BIFROST_WINDOW_POINTER) {
          seen.push_back({int(e.type), int(e.action), int(e.pointer_id), 0, e.x, e.y});
        }
      }
      unsetenv("BIFROST_TEST_SDL_EVENT");
      unsetenv("BIFROST_TEST_SDL_BINDING_FAIL");
      pads.reset();
      frost::set_pad_cursor_mode(-1);
      return seen;
    };
    auto pad_key = [](const Seen &s, int key, int action) { return s.type == BIFROST_WINDOW_GAMEPAD && s.key == key && s.action == action; };
    constexpr int kKeyA = 96, kKeyStart = 108, kKeySelect = 109;
    // Auto: the chord turns it on. Start went to the game first, so the game is told it was released; Select is the cursor's.
    auto seen = play("pad:added,start-down,select-down,a-down,a-up,select-up,start-up", 0);
    CHECK(seen.size() == 5);
    CHECK(pad_key(seen[1], kKeyStart, 0) && pad_key(seen[2], kKeyStart, 1));
    CHECK(seen[3].type == BIFROST_WINDOW_POINTER && seen[3].action == 0 && seen[3].pointer == 0);
    CHECK(seen[3].x == 800.0f && seen[3].y == 600.0f);                 // the middle of the 1600x1200 panel
    CHECK(seen[4].type == BIFROST_WINDOW_POINTER && seen[4].action == 1 && seen[4].pointer == 0);
    for (const Seen &s : seen) CHECK(!pad_key(s, kKeyA, 0) && !pad_key(s, kKeySelect, 0));   // A and Select never reached the game
    // Off: A is an ordinary button, and Start and Select separately are the game's.
    seen = play("pad:added,start-down,start-up,select-down,select-up,a-down,a-up", 0);
    CHECK(seen.size() == 7 && pad_key(seen[1], kKeyStart, 0) && pad_key(seen[2], kKeyStart, 1) &&
          pad_key(seen[3], kKeySelect, 0) && pad_key(seen[5], kKeyA, 0) && pad_key(seen[6], kKeyA, 1));
    // Gamepad mode: the chord is two presses for the game and A stays a button.
    seen = play("pad:added,start-down,select-down,a-down,a-up", 1);
    CHECK(seen.size() == 5 && pad_key(seen[1], kKeyStart, 0) && pad_key(seen[2], kKeySelect, 0) && pad_key(seen[3], kKeyA, 0));
    for (const Seen &s : seen) CHECK(s.type != BIFROST_WINDOW_POINTER);
    // Cursor mode: on from the start, so A is a touch straight away; X is a second finger.
    seen = play("pad:added,a-down,a-up,x-down,x-up", 2);
    CHECK(seen.size() == 5);
    CHECK(seen[1].type == BIFROST_WINDOW_POINTER && seen[1].action == 0 && seen[1].pointer == 0);
    CHECK(seen[2].type == BIFROST_WINDOW_POINTER && seen[2].action == 1);
    CHECK(seen[3].type == BIFROST_WINDOW_POINTER && seen[3].action == 0 && seen[3].pointer == 1);
    CHECK(seen[4].type == BIFROST_WINDOW_POINTER && seen[4].action == 1 && seen[4].pointer == 1);
    CHECK(frost::pad_cursor_mode_for_name("auto") == 0 && frost::pad_cursor_mode_for_name("gamepad") == 1 &&
          frost::pad_cursor_mode_for_name("cursor") == 2 && frost::pad_cursor_mode_for_name("touch") < 0);
  }
  // Gamepads: hotplug, buttons as Android KEYCODE_BUTTON_*, the d-pad as hat axes (last press wins), stick and
  // trigger values in Android's ranges, repeats and unknown devices dropped, and a focus loss releasing what is held.
  {
    auto pads = frost::make_sdl_android_window_backend();
    CHECK(pads->open("pads", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    setenv("BIFROST_TEST_SDL_EVENT", "pad:added,added,unknown-down,a-down,a-down,a-up,paddle-down,left-down,"
                                     "right-down,left-up,right-up,leftx:-32768,leftx:-32768,lt:32767,a-down,"
                                     "focus-lost,removed", 1);
    std::vector<frost::AndroidWindowGamepad> got;
    for (int polls = 0; polls < 64; ++polls) {
      frost::AndroidWindowEvent e{};
      if (pads->poll(e) != 1) continue;
      CHECK(e.type == BIFROST_WINDOW_GAMEPAD && e.time_ms >= 0);
      frost::AndroidWindowGamepad pad{};
      CHECK(pads->poll_gamepad(pad) == 1 && pad.device == 7);
      got.push_back(pad);
    }
    frost::AndroidWindowGamepad none{};
    CHECK(pads->poll_gamepad(none) == 0);
    auto button = [&](size_t i, int key, int action) {
      return i < got.size() && got[i].kind == BIFROST_GAMEPAD_BUTTON && got[i].key_code == key && got[i].action == action;
    };
    auto axes = [&](size_t i, float x, float trigger, float hat_x) {
      return i < got.size() && got[i].kind == BIFROST_GAMEPAD_AXES && got[i].axes[BIFROST_GAMEPAD_AXIS_X] == x &&
             got[i].axes[BIFROST_GAMEPAD_AXIS_LTRIGGER] == trigger && got[i].axes[BIFROST_GAMEPAD_AXIS_HAT_X] == hat_x;
    };
    CHECK(got.size() == 12);
    CHECK(got[0].kind == BIFROST_GAMEPAD_ADDED && std::string(got[0].name) == "Fake Pad" &&
          got[0].vendor == 0x28de && got[0].product == 0x1304);
    CHECK(button(1, 96, 0) && button(2, 96, 1));
    CHECK(axes(3, 0, 0, -1) && axes(4, 0, 0, 1) && axes(5, 0, 0, 0));
    CHECK(axes(6, -1, 0, 0) && axes(7, -1, 1, 0) && button(8, 96, 0));
    CHECK(button(9, 96, 1) && axes(10, 0, 0, 0) && got[11].kind == BIFROST_GAMEPAD_REMOVED);
    unsetenv("BIFROST_TEST_SDL_EVENT");
    pads.reset();
    // The first press hid the pointer; closing shows it again.
    CHECK(contents(log) == "gamepad-open 7\ncursor-hide\ngamepad-close 7\ngamepad-quit\ncursor-show\nwindow-destroy\n"
                           "video-quit\n");
    // Pointer and pad: a press hides it, the mouse shows it, a resting stick's drift leaves it, a full push hides
    // it again; an app's own hidden pointer stays hidden when the mouse moves, and an app shape after the pad hid
    // the pointer does not show it.
    auto pointer = frost::make_sdl_android_window_backend();
    CHECK(pointer->open("pointer", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    auto drain = [&]() {
      for (int polls = 0; polls < 32; ++polls) {
        frost::AndroidWindowEvent e{};
        frost::AndroidWindowGamepad pad{};
        if (pointer->poll(e) == 1 && e.type == BIFROST_WINDOW_GAMEPAD) pointer->poll_gamepad(pad);
      }
    };
    setenv("BIFROST_TEST_SDL_EVENT", "pad:added,a-down,a-up,mouse-move,leftx:-8000,leftx:-32768,mouse-move", 1);
    drain();
    CHECK(contents(log) == "gamepad-open 7\ncursor-hide\ncursor-show\ncursor-hide\ncursor-show\n");
    std::ofstream(log, std::ios::trunc).close();
    CHECK(pointer->set_cursor(BIFROST_WINDOW_CURSOR_HIDDEN) == 0);
    setenv("BIFROST_TEST_SDL_EVENT", "pad:a-down,a-up,mouse-move", 1);
    drain();
    CHECK(pointer->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == 0);  // shows the app's pointer again
    setenv("BIFROST_TEST_SDL_EVENT", "pad:a-down,a-up", 1);
    drain();
    CHECK(pointer->set_cursor(BIFROST_WINDOW_CURSOR_TEXT) == 0);     // still hidden for the pad
    unsetenv("BIFROST_TEST_SDL_EVENT");
    pointer.reset();
    CHECK(contents(log) == "cursor-hide\ncursor-create " + std::to_string(SDL_SYSTEM_CURSOR_POINTER) + "\ncursor-set " +
                           std::to_string(SDL_SYSTEM_CURSOR_POINTER) + "\ncursor-show\ncursor-hide\ncursor-create " +
                           std::to_string(SDL_SYSTEM_CURSOR_TEXT) + "\ncursor-set " + std::to_string(SDL_SYSTEM_CURSOR_TEXT) +
                           "\ngamepad-close 7\ngamepad-quit\ncursor-show\ncursor-destroy " +
                           std::to_string(SDL_SYSTEM_CURSOR_TEXT) + "\ncursor-destroy " +
                           std::to_string(SDL_SYSTEM_CURSOR_POINTER) + "\nwindow-destroy\nvideo-quit\n");
    // Without gamepad support the window still opens and closes normally.
    setenv("BIFROST_TEST_SDL_GAMEPAD_FAIL", "1", 1);
    auto plain = frost::make_sdl_android_window_backend();
    std::ofstream(log, std::ios::trunc).close();
    CHECK(plain->open("no pads", 800, 600) == 0);
    plain.reset();
    unsetenv("BIFROST_TEST_SDL_GAMEPAD_FAIL");
    CHECK(contents(log) == "video-init\nwindow-create\nwindow-destroy\nvideo-quit\n");
  }
  // F11 toggles fullscreen and never reaches the app, pressed, held or released; leaving it brings the window back.
  {
    auto fs = frost::make_sdl_android_window_backend();
    CHECK(fs->open("fullscreen", 800, 600) == 0);
    std::ofstream(log, std::ios::trunc).close();
    frost::AndroidWindowEvent ev{};
    auto key = [&](const char *mode) { setenv("BIFROST_TEST_SDL_EVENT", mode, 1); return fs->poll(ev); };
    CHECK(key("once:f11-down#1") == 0 && ev.type == 0);
    CHECK(key("once:f11-repeat#2") == 0 && key("once:f11-up#3") == 0);
    CHECK(contents(log) == "fullscreen-on\n");
    CHECK(key("once:f11-down#4") == 0 && contents(log) == "fullscreen-on\nfullscreen-off\n");
    unsetenv("BIFROST_TEST_SDL_EVENT");
    fs.reset();
  }
  // The remembered size. It is applied once, after the app has set its window up, as a resize (never a different opening
  // size); held inside the display; fitted to the shape of an app that asked for an orientation; and saved on close unless
  // the window has no size of its own.
  {
    char directory[] = "/tmp/bifrost-window-state-XXXXXX";
    CHECK(mkdtemp(directory));
    const std::string state = std::string(directory) + "/launcher-window.txt";
    setenv("BIFROST_ANDROID_WINDOW_STATE", state.c_str(), 1);
    auto remember = [&](const char *text) { std::ofstream(state, std::ios::trunc) << text; };
    frost::AndroidWindowEvent ev{};
    int32_t w = 0, h = 0;
    // No file: the window stays as asked, and closing writes what it has (desktop units: the 2x display halves pixels).
    unlink(state.c_str());
    auto win = frost::make_sdl_android_window_backend();
    CHECK(win->open("remembered", 800, 600) == 0);
    CHECK(win->poll(ev) == 0 && win->pixel_size(w, h) && w == 1600 && h == 1200);
    CHECK(win->resize(1000, 800, true) == 0);
    win->close();
    CHECK(contents(state.c_str()) == "500 400\n");
    // A remembered size is applied on the first poll only.
    remember("1000 700\n");
    CHECK(win->open("remembered", 800, 600) == 0);
    CHECK(win->pixel_size(w, h) && w == 1600 && h == 1200);   // opening is unchanged
    CHECK(win->poll(ev) == 0 && win->pixel_size(w, h) && w == 2000 && h == 1400);
    CHECK(win->resize(1200, 1000, true) == 0 && win->poll(ev) == 0);
    CHECK(win->pixel_size(w, h) && w == 1200 && h == 1000);   // not applied again
    win->close();
    CHECK(contents(state.c_str()) == "600 500\n");
    // Held inside the display (1920x1080), whatever display the size was saved on.
    remember("3000 2000\n");
    CHECK(win->open("remembered", 800, 600) == 0 && win->poll(ev) == 0);
    CHECK(win->pixel_size(w, h) && w == 3840 && h == 2160);
    win->close();
    // A damaged file is ignored.
    remember("wide\n");
    CHECK(win->open("remembered", 800, 600) == 0 && win->poll(ev) == 0);
    CHECK(win->pixel_size(w, h) && w == 1600 && h == 1200);
    win->close();
    // A window with no size of its own keeps the earlier one: fullscreen, maximised, minimised or hidden.
    for (const char *flags : {"fullscreen", "maximized", "minimized", "hidden"}) {
      remember("640 480\n");
      CHECK(win->open("remembered", 800, 600) == 0 && win->poll(ev) == 0);
      win->resize(1000, 800, true);
      setenv("BIFROST_TEST_SDL_WINDOW_FLAGS", flags, 1);
      win->close();
      unsetenv("BIFROST_TEST_SDL_WINDOW_FLAGS");
      CHECK(contents(state.c_str()) == "640 480\n");
    }
    // An app that asked for an orientation keeps its shape: the largest window of that shape inside the remembered size.
    remember("1000 1000\n");
    CHECK(win->open("remembered", 800, 600) == 0);
    CHECK(win->set_orientation(1) == 0);                    // portrait: 600x800 desktop units
    CHECK(win->poll(ev) == 0 && win->pixel_size(w, h) && w == 1500 && h == 2000);
    win->close();
    // A fixed-size app's window takes any shape the user kept.
    remember("1000 500\n");
    CHECK(win->open("remembered", 800, 600) == 0);
    CHECK(win->set_fixed_size(1600, 1200) == 0);
    arm64emu::window_stats::WindowInfo shape;
    CHECK(win->poll(ev) == 0 && win->window_info(shape) && shape.w == 2000 && shape.h == 1000);
    CHECK(win->pixel_size(w, h) && w == 1600 && h == 1200);   // the app's panel is what it was
    win->close();
    // Without the variable nothing is read or written.
    unsetenv("BIFROST_ANDROID_WINDOW_STATE");
    remember("640 480\n");
    CHECK(win->open("remembered", 800, 600) == 0 && win->poll(ev) == 0);
    CHECK(win->pixel_size(w, h) && w == 1600 && h == 1200);
    win->resize(1000, 800, true);
    win->close();
    CHECK(contents(state.c_str()) == "640 480\n");
    unlink(state.c_str());
    unlink((state + ".tmp").c_str());
    rmdir(directory);
  }
  for (int i = 0; i < 256; ++i) {
    first = frost::make_sdl_android_window_backend();
    CHECK(first->open("stress", 800, 600) == 0 && first->binding().window);
    CHECK(first->resize(320, 240, true) == 0);
    first->close(); first->close(); first.reset();
  }
  std::puts("android_sdl_backend_lifetime: SDL3 checks and 256 teardown cycles passed");
}
