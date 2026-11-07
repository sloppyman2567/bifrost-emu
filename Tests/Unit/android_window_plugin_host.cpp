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
    CHECK(manager.set_task_visible(false) == 0);
    CHECK(manager.set_task_visible(true) == 0);
    CHECK(contents(log).find("task-hide\ntask-show\n") != std::string::npos);
    CHECK(manager.set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == 0);
    CHECK(manager.set_cursor(BIFROST_WINDOW_CURSOR_HIDDEN) == 0);
    CHECK(contents(log).find("cursor 3\ncursor 1\n") != std::string::npos);
    // An unknown shape never reaches the plugin; a plugin's refusal comes back as its errno.
    CHECK(manager.set_cursor(-1) == -EINVAL && manager.set_cursor(BIFROST_WINDOW_CURSOR_COUNT) == -EINVAL);
    CHECK(contents(log).find("cursor -1") == std::string::npos && contents(log).find("cursor 12") == std::string::npos);
    mode("cursor-fail");
    CHECK(manager.set_cursor(BIFROST_WINDOW_CURSOR_TEXT) == -EIO);
    mode("");
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
    manager.queue_finish_event(manager.input_queue_handle(), input);
    // That mode repeats its pointer event on every poll: drain what the pump queued.
    while (manager.queue_get_event(manager.input_queue_handle(), &input) == 0)
      manager.queue_finish_event(manager.input_queue_handle(), input);
    // A window that stops and starts being visible reaches the managed launcher in order, with the host's time and
    // the new state in the action; a native activity's queue never sees it.
    mode("poll-visibility");
    manager.pump_host_events(nullptr);
    mode("");
    for (const int32_t visible : {0, 1}) {
      CHECK(manager.queue_get_event(manager.input_queue_handle(), &input, true) == 0);
      CHECK(manager.event_type(input) == frost::AndroidSurfaceManager::EVENT_TYPE_VISIBILITY);
      CHECK(manager.motion_action(input) == visible);
      CHECK(manager.motion_event_time(input) == (2000 + visible) * 1000000);
      manager.queue_finish_event(manager.input_queue_handle(), input);
    }
    CHECK(manager.queue_get_event(manager.input_queue_handle(), &input, true) != 0);
    mode("poll-visibility");
    manager.pump_host_events(nullptr);
    mode("");
    CHECK(manager.queue_get_event(manager.input_queue_handle(), &input) != 0);
    CHECK(!manager.install_window_backend(frost::make_sdl_android_window_backend()));
    // Destructor must close the owned window while the DSO is resident.
  }
  CHECK(contents(log) == "create\nopen\ntask-hide\ntask-show\ncursor 3\ncursor 1\nclose\ndestroy\nunload\n");
  // Poll has its own positive-success contract; poison is discarded.
  auto polling = frost::load_android_window_backend(argv[1], error);
  CHECK(polling);
  frost::AndroidWindowEvent event{};
  for (const char *invalid : {"poll-invalid", "poll-positive", "poll-fail", "poll-nan", "poll-key-invalid",
                              "poll-visibility-invalid"}) {
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
  // Host text input. TEXT events are legal only after the host started it, each
  // has a valid payload, and they stay in order with key events.
  {
    mode("text-input");
    std::ofstream(log, std::ios::trunc).close();
    frost::AndroidSurfaceManager manager(frost::load_android_window_backend(argv[1], error));
    CHECK(manager.open_host_window());
    uint64_t input = 0;
    manager.pump_host_events(nullptr);  // nothing is reported before it starts
    CHECK(manager.queue_get_event(manager.input_queue_handle(), &input) != 0);
    CHECK(manager.set_text_input(true, 10, 20, 300, 40) == 0);
    struct Step { int32_t type; uint32_t kind; const char *text; };
    const int32_t text_type = frost::AndroidSurfaceManager::EVENT_TYPE_TEXT;
    const Step script[] = {{text_type, BIFROST_WINDOW_TEXT_COMMIT, "h\xc3\xa9llo"},
                           {text_type, BIFROST_WINDOW_TEXT_COMPOSING, "ni"},
                           {text_type, BIFROST_WINDOW_TEXT_COMMIT, "\xe4\xbd\xa0"},
                           {1, 0, ""}, {1, 0, ""},  // Backspace down and up
                           {text_type, BIFROST_WINDOW_TEXT_COMMIT, "!"}};
    size_t seen = 0;
    for (int tries = 0; tries < 300 && seen < 6; ++tries) {
      manager.pump_host_events(nullptr);
      while (manager.queue_get_event(manager.input_queue_handle(), &input) == 0) {
        CHECK(seen < 6 && manager.event_type(input) == script[seen].type);
        if (script[seen].type == text_type) {
          uint32_t kind = 0;
          char text[BIFROST_WINDOW_TEXT_MAX + 1];
          const uint32_t length = manager.event_text(input, kind, text);
          CHECK(kind == script[seen].kind && std::string(text) == script[seen].text &&
                length == std::string(script[seen].text).size());
        } else {
          CHECK(manager.key_code(input) == 67 && manager.key_action(input) == int32_t(seen - 3));
        }
        manager.queue_finish_event(manager.input_queue_handle(), input);
        ++seen;
      }
      usleep(20000);
    }
    CHECK(seen == 6);
    CHECK(manager.set_text_input(false, 0, 0, 0, 0) == 0);
    CHECK(contents(log).find("text-input 1 10,20 300x40\n") != std::string::npos);
    CHECK(contents(log).find("text-input 0 0,0 0x0\n") != std::string::npos);
  }
  {
    // A plugin may not report TEXT unasked, nor hand over a bad payload; a plugin
    // that fails to start text input, or has no callbacks, reports so.
    auto texting = frost::load_android_window_backend(argv[1], error);
    CHECK(texting);
    mode("text-unsolicited");
    CHECK(texting->poll(event) == -EPROTO && event.type == 0);
    frost::AndroidWindowText payload{};
    for (const char *invalid : {"text-bad-kind", "text-bad-length"}) {
      mode(invalid);
      CHECK(texting->set_text_input(true, 0, 0, 1, 1) == 0);
      for (int tries = 0; tries < 100 && texting->poll(event) == 0; ++tries) usleep(10000);
      CHECK(event.type == BIFROST_WINDOW_TEXT);
      CHECK(texting->poll_text(payload) == -EPROTO && payload.length == 0);
      CHECK(texting->set_text_input(false, 0, 0, 0, 0) == 0);
    }
    // A plugin from before set_cursor still loads and everything else works.
    mode("no-cursor");
    auto no_cursor = frost::load_android_window_backend(argv[1], error);
    CHECK(no_cursor && no_cursor->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == -ENOTSUP);
    CHECK(no_cursor->set_task_visible(true) == 0);
    CHECK(no_cursor->set_cursor(-1) == -EINVAL);
    mode("text-input-fail");
    CHECK(texting->set_text_input(true, 0, 0, 1, 1) == -ENOTSUP);
    mode("no-text");
    auto plain = frost::load_android_window_backend(argv[1], error);
    CHECK(plain && plain->set_text_input(true, 0, 0, 1, 1) == -ENOTSUP);
    CHECK(plain->poll_text(payload) == 0);
  }
  // Gamepads through the manager: Android device ids from 2 and controller numbers from 1 (the lowest free one),
  // buttons as KEYBOARD|GAMEPAD keys, consecutive axis changes batched into one joystick motion, a payload for a
  // device never added dropped, and closing the window removing what is left, which native queues never see.
  {
    mode("gamepad");
    frost::AndroidSurfaceManager manager(frost::load_android_window_backend(argv[1], error));
    CHECK(manager.open_host_window());
    for (int i = 0; i < 4; ++i) manager.pump_host_events(nullptr);
    const uint64_t queue = manager.input_queue_handle();
    const int32_t device_type = frost::AndroidSurfaceManager::EVENT_TYPE_DEVICE;
    uint64_t input = 0;
    auto device = [&](int32_t action, int32_t id, int32_t controller, const char *name) {
      if (manager.queue_get_event(queue, &input, true) != 0) return false;
      int32_t vendor = -1, product = -1, number = -1;
      char text[BIFROST_WINDOW_TEXT_MAX + 1];
      const int32_t length = manager.event_device(input, vendor, product, number, text);
      const bool ok = manager.event_type(input) == device_type && manager.motion_action(input) == action &&
                      manager.event_device_id(input) == id && (action == 0 || (number == controller &&
                      std::string(text) == name && length == int32_t(std::string(name).size())));
      manager.queue_finish_event(queue, input);
      return ok;
    };
    auto key = [&](int32_t action) {
      if (manager.queue_get_event(queue, &input, true) != 0) return false;
      const bool ok = manager.event_type(input) == 1 && manager.event_source(input) == 0x501 &&
                      manager.event_device_id(input) == 2 && manager.key_code(input) == 96 &&
                      manager.key_action(input) == action;
      manager.queue_finish_event(queue, input);
      return ok;
    };
    CHECK(device(1, 2, 1, "Fixture Pad"));
    CHECK(key(0));
    CHECK(manager.queue_get_event(queue, &input, true) == 0);
    float axes[BIFROST_GAMEPAD_AXIS_COUNT];
    CHECK(manager.event_type(input) == 2 && manager.event_source(input) == 0x01000010 &&
          manager.event_device_id(input) == 2 && manager.motion_action(input) == 2 &&
          manager.motion_pointer_count(input) == 1 && manager.event_axes(input, axes));
    CHECK(axes[BIFROST_GAMEPAD_AXIS_X] == 0.75f && manager.motion_x(input, 0) == 0.75f);
    CHECK(manager.motion_axis_value(input, 1, 0) == -0.25f && manager.motion_axis_value(input, 15, 0) == -1.0f);
    CHECK(manager.motion_axis_value(input, 17, 0) == 1.0f && manager.motion_axis_value(input, 18, 0) == 0.0f);
    manager.queue_finish_event(queue, input);
    CHECK(key(1));
    CHECK(device(1, 3, 2, "Second"));
    CHECK(device(0, 2, 0, ""));
    CHECK(device(1, 4, 1, "Third"));
    CHECK(manager.queue_get_event(queue, &input, true) != 0);
    manager.close_host_window();
    CHECK(manager.queue_get_event(queue, &input) != 0);  // two removals, dropped for a native queue
    CHECK(manager.queue_get_event(queue, &input, true) != 0);
  }
  {
    // An axis out of range is a protocol error; a plugin without poll_gamepad may not report gamepads.
    mode("gamepad-bad-axis");
    auto pads = frost::load_android_window_backend(argv[1], error);
    CHECK(pads && pads->open("pads", 64, 64) == 0);
    frost::AndroidWindowGamepad payload{};
    CHECK(pads->poll(event) == 1 && event.type == BIFROST_WINDOW_GAMEPAD);
    CHECK(pads->poll_gamepad(payload) == -EPROTO && payload.kind == 0);
    mode("no-gamepad");
    auto old_pads = frost::load_android_window_backend(argv[1], error);
    CHECK(old_pads && old_pads->open("old", 64, 64) == 0);
    CHECK(old_pads->poll(event) == -EPROTO && event.type == 0);
    CHECK(old_pads->poll_gamepad(payload) == 0);
  }
  // An original-size v2 descriptor still loads; its trailing fields are ignored.
  mode("old-size");
  auto old = frost::load_android_window_backend(argv[1], error);
  CHECK(old && error.empty());
  CHECK(old->set_task_visible(false) == -ENOTSUP);
  CHECK(old->set_cursor(BIFROST_WINDOW_CURSOR_POINTER) == -ENOTSUP);
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
