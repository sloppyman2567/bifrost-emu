// A pointer driven by a gamepad, for apps that only take touch.
//
// The window backend feeds it SDL's gamepad buttons and axes and the clock; it says what to do in return: pointer events at
// app pixels (the same events a mouse makes, so the app sees ordinary touch) and which buttons the game must not see. Start
// and Select pressed together switch it on and off (mode Auto); mode Gamepad never has a cursor, mode Cursor starts with it
// on. While it is on the left stick moves the pointer (the left trigger slows it, the right trigger speeds it up), A is the
// primary finger and X a second one that stays where it went down; the other buttons and the right stick still reach the game.
//
// No SDL here: buttons and axes use SDL_GamepadButton and SDL_GamepadAxis numbers, so the logic is tested on its own.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace frost {

enum class PadCursorMode { Auto = 0, Gamepad = 1, Cursor = 2 };

namespace padbtn { constexpr int kSouth = 0, kWest = 2, kBack = 4, kStart = 6; }
namespace padaxis { constexpr int kLeftX = 0, kLeftY = 1, kLeftTrigger = 4, kRightTrigger = 5; }

class PadCursor {
public:
  struct Output {
    enum Kind { Move, Down, Up, ReleaseToGame, Toggled } kind = Move;
    int finger = 0;       // 0 the primary finger, 1 the second
    float x = 0, y = 0;   // app pixels
    bool held = false;    // Move: the primary finger is down, so this is a drag and not a hover
    int button = -1;      // ReleaseToGame: the SDL button the game saw pressed and must now see released
    bool on = false;      // Toggled: the cursor is now on or off
  };

  static constexpr int64_t kChordMs = 350;     // Start and Select count as together when pressed this close
  static constexpr float kDeadZone = 0.15f;
  static constexpr float kSpeed = 1.1f;        // panel heights per second at full deflection, before the curve
  static constexpr float kSlow = 0.25f, kFast = 2.0f;

  explicit PadCursor(PadCursorMode mode = PadCursorMode::Auto) : mode_(mode), active_(mode == PadCursorMode::Cursor) {}

  PadCursorMode mode() const { return mode_; }
  bool active() const { return active_; }
  float x() const { return x_; }
  float y() const { return y_; }
  int width() const { return w_; }
  int height() const { return h_; }

  // The panel the pointer lives in, in app pixels. The first call puts the pointer in the middle.
  void set_bounds(int width, int height) {
    if (width <= 0 || height <= 0) return;
    const bool first = w_ <= 0;
    w_ = width; h_ = height;
    if (first) { x_ = width / 2.0f; y_ = height / 2.0f; }
    x_ = std::clamp(x_, 0.0f, float(w_ - 1));
    y_ = std::clamp(y_, 0.0f, float(h_ - 1));
  }

  // A gamepad button changed. True when the cursor used it and the game must not see it.
  bool button(int button, bool down, int64_t ms, std::vector<Output> &out) {
    if (mode_ == PadCursorMode::Gamepad) return false;
    if (button == padbtn::kStart || button == padbtn::kBack) return chord(button, down, ms, out);
    if (!active_) return false;
    if (button == padbtn::kSouth || button == padbtn::kWest) {
      const int finger = button == padbtn::kSouth ? 0 : 1;
      if (down == fingers_[finger]) return true;
      fingers_[finger] = down;
      Output o; o.kind = down ? Output::Down : Output::Up; o.finger = finger;
      o.x = finger == 0 ? x_ : second_x_; o.y = finger == 0 ? y_ : second_y_;
      if (finger == 1 && down) { second_x_ = x_; second_y_ = y_; o.x = x_; o.y = y_; }
      out.push_back(o);
      return true;
    }
    return false;
  }

  // A gamepad axis moved (SDL's -32768..32767). True when the cursor used it.
  bool axis(int axis, int value) {
    if (mode_ == PadCursorMode::Gamepad || !active_) return false;
    const float v = std::clamp(value / 32767.0f, -1.0f, 1.0f);
    switch (axis) {
    case padaxis::kLeftX: stick_x_ = v; return true;
    case padaxis::kLeftY: stick_y_ = v; return true;
    case padaxis::kLeftTrigger: slow_ = v > 0.3f; return true;
    case padaxis::kRightTrigger: fast_ = v > 0.3f; return true;
    default: return false;
    }
  }

  // Time passes: move the pointer by what the stick asks for.
  void tick(int64_t ms, std::vector<Output> &out) {
    const int64_t before = last_ms_;
    last_ms_ = ms;
    if (!active_ || w_ <= 0 || before == INT64_MIN) return;
    const float dt = std::clamp((ms - before) / 1000.0f, 0.0f, 0.05f);
    const float magnitude = std::hypot(stick_x_, stick_y_);
    if (magnitude <= kDeadZone || dt <= 0.0f) return;
    const float scaled = std::min(1.0f, (magnitude - kDeadZone) / (1.0f - kDeadZone));
    float speed = kSpeed * float(std::min(w_, h_)) * scaled * scaled;   // a curve: fine near the middle, quick at the edge
    if (slow_) speed *= kSlow;
    if (fast_) speed *= kFast;
    const float nx = std::clamp(x_ + stick_x_ / magnitude * speed * dt, 0.0f, float(w_ - 1));
    const float ny = std::clamp(y_ + stick_y_ / magnitude * speed * dt, 0.0f, float(h_ - 1));
    if (nx == x_ && ny == y_) return;
    x_ = nx; y_ = ny;
    Output o; o.kind = Output::Move; o.x = x_; o.y = y_; o.held = fingers_[0];
    out.push_back(o);
  }

private:
  bool chord(int button, bool down, int64_t ms, std::vector<Output> &out) {
    const bool start = button == padbtn::kStart;
    bool &mine = start ? start_down_ : back_down_;
    bool &other = start ? back_down_ : start_down_;
    int64_t &my_ms = start ? start_ms_ : back_ms_;
    const int64_t other_ms = start ? back_ms_ : start_ms_;
    bool &swallow = start ? swallow_start_ : swallow_back_;
    if (!down) {
      mine = false;
      if (swallow) { swallow = false; return true; }   // the cursor took this press; its release is not the game's either
      return false;
    }
    mine = true; my_ms = ms;
    if (mode_ != PadCursorMode::Auto && mode_ != PadCursorMode::Cursor) return false;
    if (!other || ms - other_ms > kChordMs) return false;
    // Start and Select together. The one pressed first already went to the game as a press: take it back.
    active_ = !active_;
    Output toggled; toggled.kind = Output::Toggled; toggled.on = active_; out.push_back(toggled);
    Output release; release.kind = Output::ReleaseToGame; release.button = start ? padbtn::kBack : padbtn::kStart;
    out.push_back(release);
    swallow_start_ = swallow_back_ = true;
    if (!active_) {
      for (int finger = 0; finger < 2; ++finger) {
        if (!fingers_[finger]) continue;
        fingers_[finger] = false;
        Output up; up.kind = Output::Up; up.finger = finger;
        up.x = finger == 0 ? x_ : second_x_; up.y = finger == 0 ? y_ : second_y_;
        out.push_back(up);
      }
      stick_x_ = stick_y_ = 0.0f; slow_ = fast_ = false;
    }
    return true;
  }

  PadCursorMode mode_;
  bool active_;
  int w_ = 0, h_ = 0;
  float x_ = 0, y_ = 0, second_x_ = 0, second_y_ = 0;
  float stick_x_ = 0, stick_y_ = 0;
  bool slow_ = false, fast_ = false;
  bool fingers_[2] = {false, false};
  bool start_down_ = false, back_down_ = false, swallow_start_ = false, swallow_back_ = false;
  int64_t start_ms_ = 0, back_ms_ = 0;
  int64_t last_ms_ = INT64_MIN;
};

// The pointer's picture, drawn here rather than shipped: a white arrow with a dark outline, `size` pixels tall, the tip at
// the top left pixel. Straight (not premultiplied) ARGB, as the popup layers take it.
inline std::vector<uint32_t> pad_cursor_bitmap(int size) {
  struct Point { float x, y; };
  static const Point kArrow[] = {{0.00f, 0.00f}, {0.00f, 0.86f}, {0.21f, 0.67f}, {0.37f, 1.00f},
                                 {0.52f, 0.93f}, {0.36f, 0.61f}, {0.64f, 0.61f}};
  constexpr int kPoints = sizeof(kArrow) / sizeof(kArrow[0]);
  const float scale = float(size) - 3.0f;           // room for the outline around the shape
  std::vector<Point> shape;
  for (const Point &p : kArrow) shape.push_back({p.x * scale + 1.5f, p.y * scale + 1.5f});
  auto signed_distance = [&](float px, float py) {
    float best = 1e9f; bool inside = false;
    for (int i = 0, j = kPoints - 1; i < kPoints; j = i++) {
      const Point &a = shape[i], &b = shape[j];
      if (((a.y > py) != (b.y > py)) && (px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x)) inside = !inside;
      const float ex = b.x - a.x, ey = b.y - a.y, t = std::clamp(((px - a.x) * ex + (py - a.y) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
      best = std::min(best, std::hypot(px - (a.x + t * ex), py - (a.y + t * ey)));
    }
    return inside ? -best : best;
  };
  std::vector<uint32_t> pixels(size_t(size) * size, 0);
  const float outline = std::max(1.0f, size / 18.0f);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const float d = signed_distance(x + 0.5f, y + 0.5f);
      const float shape_alpha = std::clamp(0.5f - d, 0.0f, 1.0f);
      const float outer_alpha = std::clamp(0.5f - (d - outline), 0.0f, 1.0f);
      if (outer_alpha <= 0.0f) continue;
      const float fill = std::min(1.0f, shape_alpha / outer_alpha);
      const uint32_t grey = uint32_t(fill * 255.0f + 0.5f), alpha = uint32_t(outer_alpha * 255.0f + 0.5f);
      pixels[size_t(y) * size + x] = (alpha << 24) | (grey << 16) | (grey << 8) | grey;
    }
  return pixels;
}

// The top left `width` x `height` pixels of a `size` x `size` picture: the part of the arrow that fits in the panel when the
// pointer is near its right or bottom edge. A layer larger than that would hang outside the window, which a compositor draws.
inline std::vector<uint32_t> pad_cursor_crop(const std::vector<uint32_t> &bitmap, int size, int width, int height) {
  width = std::clamp(width, 0, size); height = std::clamp(height, 0, size);
  std::vector<uint32_t> out(size_t(width) * height);
  for (int y = 0; y < height; ++y)
    std::copy_n(bitmap.begin() + size_t(y) * size, width, out.begin() + size_t(y) * width);
  return out;
}

}  // namespace frost
