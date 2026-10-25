#pragma once
#include "Bifrost/AndroidWindowPlugin.h"
#include <cstdint>
#include <vector>

namespace frost {
using WheelEvent = bifrost_android_window_event_v2;

// A desktop pointing device as the two-finger gestures a touchscreen would send, for apps that zoom and move
// their view by touch (maps, sandbox games): a mouse wheel or trackpad pinch becomes a pinch around the cursor,
// and a scroll with Shift held or a drag with the middle button becomes a two-finger pan.
//
// The fingers are synthetic and exist only while a gesture runs: they go down together, move, and lift when the
// wheel has been idle for a moment (or the button is released), so an app sees ordinary touch events and nothing
// is left pressed. A pinch that runs out of room on the screen lifts and starts again at the middle spacing, so
// a long scroll keeps zooming. Coordinates are surface pixels, times CLOCK_MONOTONIC milliseconds.
class WheelGestures {
public:
    // Opaque pointer keys, far from the mouse fingers (0, 1) and the touchscreen fingers (2 and up).
    static constexpr int32_t kFirstFinger = 0x7ffffff0, kSecondFinger = 0x7ffffff1;
    static constexpr uint64_t kIdleMs = 160;

    // Size of the surface; the finger spacing and the room to move are relative to its smaller side.
    void set_view(int32_t width, int32_t height);
    // Spread the fingers by `factor` (above 1 zooms in) around the cursor.
    void zoom(double factor, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out);
    // Move both fingers by (dx, dy) pixels, starting from the cursor.
    void pan(float dx, float dy, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out);
    // A held drag: the fingers follow the cursor until drag_end.
    void drag_begin(float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out);
    void drag_move(float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out);
    void drag_end(uint64_t now_ms, std::vector<WheelEvent>& out);
    // Lift the fingers of a zoom or pan whose input stopped.
    void idle(uint64_t now_ms, std::vector<WheelEvent>& out);
    // Lift whatever is down, at once.
    void cancel(uint64_t now_ms, std::vector<WheelEvent>& out);
    // Pixels one wheel step pans by.
    float scroll_step() const;
    bool active() const { return mode_ != Mode::None; }
    bool dragging() const { return mode_ == Mode::Drag; }

private:
    enum class Mode { None, Zoom, Pan, Drag };
    float extent() const;
    float base_spacing() const { return extent() * 0.20f; }
    void begin(Mode mode, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out);
    void place(float x, float y, float spacing, uint64_t now_ms, std::vector<WheelEvent>& out);
    void emit(int32_t finger, int32_t action, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) const;
    void clamp_center(float& x, float& y) const;
    void end(uint64_t now_ms, std::vector<WheelEvent>& out);

    int32_t width_ = 0, height_ = 0;
    Mode mode_ = Mode::None;
    float x_ = 0, y_ = 0, spacing_ = 0;  // center and finger spacing of the running gesture
    uint64_t last_ms_ = 0;
};
}  // namespace frost
