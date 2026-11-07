#pragma once
#include "Bifrost/AndroidWindowPlugin.h"
#include "System/Graphics/Display/WindowStats.hpp"
#include <cerrno>
#include <memory>
#include <string>
namespace frost {
// How a gamepad drives a pointer for touch-only apps (PadCursor), for windows opened from now on: 0 auto (Start and Select
// together turn it on and off), 1 gamepad (never), 2 cursor (on from the start). Unset, a window reads
// BIFROST_ANDROID_CONTROLLER. The name is one of auto, gamepad, cursor; -1 for any other.
void set_pad_cursor_mode(int mode);
int pad_cursor_mode_for_name(const std::string &name);
struct AndroidWindowBinding { uint64_t window = 0; void *display = nullptr; };
using AndroidWindowEvent = bifrost_android_window_event_v2;
using AndroidWindowText = bifrost_android_window_text_v2;
using AndroidWindowGamepad = bifrost_android_window_gamepad_v2;
// Host-only window lifecycle. The backend owns its window and input source.
// Calls run on the display thread. No guest CPU/Memory access or SDL objects.
class AndroidWindowBackend {
public:
  virtual ~AndroidWindowBackend() = default;
  virtual const char *name() const noexcept = 0;
  virtual int open(const char *title, int32_t width, int32_t height) = 0;
  virtual void close() noexcept = 0;
  virtual int resize(int32_t width, int32_t height, bool resize_host) = 0;
  virtual AndroidWindowBinding binding() = 0;
  virtual int poll(AndroidWindowEvent &event) = 0;
  // Additional independently backed layers on the same native display.
  // Unsupported backends fail closed rather than aliasing the primary window.
  virtual std::unique_ptr<AndroidWindowBackend> create_surface(int32_t, int32_t) { return {}; }
  // Pixels for this surface's own backing, below framework window layers.
  virtual int present_pixels(int32_t, int32_t, int32_t, int32_t, const uint32_t*) { return -ENOTSUP; }
  virtual int surface_geometry(int32_t, int32_t, int32_t, int32_t) { return -ENOTSUP; }
  // Restack this surface directly below another surface of the same window
  // (its native layer handle). Layers otherwise stack in creation order.
  virtual uintptr_t layer() const { return 0; }
  virtual int place_below(uintptr_t) { return -ENOTSUP; }
  // Software-drawn framework windows (dialogs, popup menus) above every
  // surface, in creation order. Pixels are unpremultiplied 0xAARRGGBB words
  // at window-pixel x/y. Popups never take host input: the primary window
  // keeps receiving it and the guest routes events to the focused popup.
  virtual int show_popup(uint32_t, int32_t, int32_t, int32_t, int32_t, const uint32_t *) {
    return -ENOTSUP;
  }
  virtual void remove_popup(uint32_t) {}
  // The desktop's text input for the focused editor: while active the backend
  // reports BIFROST_WINDOW_TEXT events in order with key events, and each one
  // has a payload for poll_text. The rectangle anchors the composition in
  // window pixels. Key events are unaffected.
  virtual int set_text_input(bool, int32_t, int32_t, int32_t, int32_t) { return -ENOTSUP; }
  virtual int poll_text(AndroidWindowText &text) { text = {}; return 0; }
  // Gamepads: each BIFROST_WINDOW_GAMEPAD event has one payload here (see the plugin ABI).
  virtual int poll_gamepad(AndroidWindowGamepad &pad) { pad = {}; return 0; }
  virtual void set_visible(bool) {}
  virtual int set_task_visible(bool) { return -ENOTSUP; }
  // The pointer shape over the window, a BIFROST_WINDOW_CURSOR_* value (-EINVAL for any other).
  // The host calls it only when the shape changes.
  virtual int set_cursor(int) { return -ENOTSUP; }
  virtual bool window_info(arm64emu::window_stats::WindowInfo &) { return false; }
  // An open window's buffer size in host pixels. open() takes the window's
  // size in desktop coordinates; on a scaled display the two differ.
  virtual bool pixel_size(int32_t &, int32_t &) { return false; }
  // Pixel size of the display currently holding the open window: the largest
  // the window can become, Android's windowConfiguration max bounds.
  virtual bool display_pixel_size(int32_t &, int32_t &) { return false; }
  // Host pixels per desktop coordinate unit (the display's scale factor).
  virtual double pixel_density() { return 1.0; }
  // An app that cannot be resized (resizeableActivity=false) keeps this pixel
  // size whatever the host window becomes: the window takes any shape (it is not
  // aspect-locked, so a compositor can tile it) and every layer, and input, is
  // scaled to fit and centred (Android scales such an app and letterboxes it
  // rather than resizing it).
  virtual int set_fixed_size(int32_t, int32_t) { return -ENOTSUP; }
  // Managed display orientation: 0 releases the request, 1 portrait, 2 landscape.
  virtual int set_orientation(int32_t) { return -ENOTSUP; }
  virtual void set_title(const char *) {}
  virtual bool set_icon(const uint8_t *, int32_t, int32_t) { return false; }
};
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend();
// Explicit trusted host path; module lives until close/destroy complete.
std::unique_ptr<AndroidWindowBackend>
load_android_window_backend(const std::string &path, std::string &error);
} // namespace frost
