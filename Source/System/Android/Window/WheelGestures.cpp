#include "System/Android/Window/WheelGestures.hpp"
#include <algorithm>
#include <cmath>

namespace frost {
namespace {
// Finger spacing limits, as fractions of the surface's smaller side.
constexpr float kMinSpacing = 0.06f, kMaxSpacing = 0.42f;
constexpr float kEdge = 8.0f;  // pixels kept between the fingers and the surface edge
constexpr int32_t kDown = 0, kUp = 1, kMove = 2;
}  // namespace

void WheelGestures::set_view(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
}

float WheelGestures::extent() const {
    const int32_t side = std::min(width_, height_);
    return side > 0 ? static_cast<float>(side) : 720.0f;
}

float WheelGestures::scroll_step() const { return std::max(24.0f, 0.05f * extent()); }

void WheelGestures::emit(int32_t finger, int32_t action, float x, float y, uint64_t now_ms,
                         std::vector<WheelEvent>& out) const {
    WheelEvent event{};
    event.type = BIFROST_WINDOW_POINTER;
    event.action = action;
    event.pointer_id = finger;
    event.x = x;
    event.y = y;
    event.pressure = action == kUp ? 0.0f : 1.0f;
    event.time_ms = static_cast<int64_t>(now_ms);
    out.push_back(event);
}

// Keep both fingers, at their widest spacing, on the surface.
void WheelGestures::clamp_center(float& x, float& y) const {
    const float half = kMaxSpacing * extent() / 2 + kEdge;
    const float width = static_cast<float>(width_ > 0 ? width_ : 1), height = static_cast<float>(height_ > 0 ? height_ : 1);
    x = width > 2 * half ? std::clamp(x, half, width - half) : width / 2;
    y = height > 2 * kEdge ? std::clamp(y, kEdge, height - kEdge) : height / 2;
}

void WheelGestures::begin(Mode mode, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) {
    x_ = x;
    y_ = y;
    clamp_center(x_, y_);
    spacing_ = base_spacing();
    mode_ = mode;
    last_ms_ = now_ms;
    emit(kFirstFinger, kDown, x_ - spacing_ / 2, y_, now_ms, out);
    emit(kSecondFinger, kDown, x_ + spacing_ / 2, y_, now_ms, out);
}

void WheelGestures::place(float x, float y, float spacing, uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (x == x_ && y == y_ && spacing == spacing_) return;
    x_ = x;
    y_ = y;
    spacing_ = spacing;
    emit(kFirstFinger, kMove, x_ - spacing_ / 2, y_, now_ms, out);
    emit(kSecondFinger, kMove, x_ + spacing_ / 2, y_, now_ms, out);
}

void WheelGestures::end(uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (mode_ == Mode::None) return;
    emit(kSecondFinger, kUp, x_ + spacing_ / 2, y_, now_ms, out);
    emit(kFirstFinger, kUp, x_ - spacing_ / 2, y_, now_ms, out);
    mode_ = Mode::None;
}

void WheelGestures::zoom(double factor, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (!(factor > 0) || !std::isfinite(factor)) return;
    if (mode_ != Mode::Zoom) {
        end(now_ms, out);
        begin(Mode::Zoom, x, y, now_ms, out);
    }
    last_ms_ = now_ms;
    const float low = kMinSpacing * extent(), high = kMaxSpacing * extent();
    float wanted = spacing_ * static_cast<float>(factor);
    const float bounded = std::clamp(wanted, low, high);
    if (bounded != wanted) {
        // Out of room: take what is left, then lift and start again so the zoom carries on.
        place(x_, y_, bounded, now_ms, out);
        const float remaining = wanted / bounded;
        const float cx = x_, cy = y_;
        end(now_ms, out);
        begin(Mode::Zoom, cx, cy, now_ms, out);
        wanted = std::clamp(spacing_ * remaining, low, high);
    }
    place(x_, y_, wanted, now_ms, out);
}

void WheelGestures::pan(float dx, float dy, float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (mode_ != Mode::Pan) {
        end(now_ms, out);
        begin(Mode::Pan, x, y, now_ms, out);
    }
    last_ms_ = now_ms;
    float nx = x_ + dx, ny = y_ + dy;
    float bx = nx, by = ny;
    clamp_center(bx, by);
    if (bx != nx || by != ny) {
        // Out of room: go as far as the surface allows, then lift and start again at the cursor.
        place(bx, by, spacing_, now_ms, out);
        end(now_ms, out);
        begin(Mode::Pan, x, y, now_ms, out);
        nx = x_ + dx;
        ny = y_ + dy;
        clamp_center(nx, ny);
    } else {
        nx = bx;
        ny = by;
    }
    place(nx, ny, spacing_, now_ms, out);
}

void WheelGestures::drag_begin(float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) {
    end(now_ms, out);
    begin(Mode::Drag, x, y, now_ms, out);
}

void WheelGestures::drag_move(float x, float y, uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (mode_ != Mode::Drag) return;
    clamp_center(x, y);
    place(x, y, spacing_, now_ms, out);
}

void WheelGestures::drag_end(uint64_t now_ms, std::vector<WheelEvent>& out) {
    if (mode_ == Mode::Drag) end(now_ms, out);
}

void WheelGestures::idle(uint64_t now_ms, std::vector<WheelEvent>& out) {
    if ((mode_ == Mode::Zoom || mode_ == Mode::Pan) && now_ms >= last_ms_ && now_ms - last_ms_ >= kIdleMs)
        end(now_ms, out);
}

void WheelGestures::cancel(uint64_t now_ms, std::vector<WheelEvent>& out) { end(now_ms, out); }
}  // namespace frost
