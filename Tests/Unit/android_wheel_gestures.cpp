// WheelGestures: a wheel, trackpad pinch or middle-button drag as the two-finger touch gestures an app expects.
#include "System/Android/Window/WheelGestures.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using frost::WheelEvent;
using frost::WheelGestures;
using Events = std::vector<WheelEvent>;
constexpr int32_t kA = WheelGestures::kFirstFinger, kB = WheelGestures::kSecondFinger;
constexpr int32_t kDown = 0, kUp = 1, kMove = 2;
constexpr int32_t kWidth = 1440, kHeight = 1384;

static bool near(double a, double b, double tolerance = 0.01) { return std::fabs(a - b) <= tolerance; }
static double spacing(const WheelEvent& a, const WheelEvent& b) { return std::fabs(b.x - a.x); }
// A well-formed pair of events: first finger then second, same time, same row.
static void check_pair(const Events& events, size_t at, int32_t action, uint64_t time) {
    assert(at + 1 < events.size());
    const auto &first = events[at], &second = events[at + 1];
    assert(first.type == BIFROST_WINDOW_POINTER && second.type == BIFROST_WINDOW_POINTER);
    assert(first.action == action && second.action == action);
    assert(first.pointer_id == kA && second.pointer_id == kB);
    assert(first.time_ms == static_cast<int64_t>(time) && second.time_ms == static_cast<int64_t>(time));
    assert(near(first.y, second.y) && first.x < second.x);
    assert(first.pressure == (action == kUp ? 0.0f : 1.0f));
}

int main() {
    WheelGestures g;
    g.set_view(kWidth, kHeight);
    const double base = 0.20 * kHeight, low = 0.06 * kHeight, high = 0.42 * kHeight;

    // A wheel step starts a pinch around the cursor: both fingers down at the base spacing, then spread.
    Events events;
    g.zoom(1.5, 700, 600, 1000, events);
    assert(events.size() == 4 && g.active() && !g.dragging());
    check_pair(events, 0, kDown, 1000);
    assert(near(spacing(events[0], events[1]), base) && near((events[0].x + events[1].x) / 2, 700) &&
           near(events[0].y, 600));
    check_pair(events, 2, kMove, 1000);
    assert(near(spacing(events[2], events[3]), base * 1.5));
    assert(near((events[2].x + events[3].x) / 2, 700));

    // More steps in the burst only move the fingers, around the same center whatever the cursor does.
    events.clear();
    g.zoom(1.2, 100, 100, 1100, events);
    assert(events.size() == 2);
    check_pair(events, 0, kMove, 1100);
    assert(near(spacing(events[0], events[1]), base * 1.5 * 1.2) && near((events[0].x + events[1].x) / 2, 700) &&
           near(events[0].y, 600));

    // Nothing lifts before the idle time; then both fingers come up, second first.
    events.clear();
    g.idle(1100 + WheelGestures::kIdleMs - 1, events);
    assert(events.empty() && g.active());
    g.idle(1100 + WheelGestures::kIdleMs, events);
    assert(events.size() == 2 && !g.active());
    assert(events[0].pointer_id == kB && events[1].pointer_id == kA);
    assert(events[0].action == kUp && events[1].action == kUp && events[0].pressure == 0);
    assert(near(spacing(events[1], events[0]), base * 1.5 * 1.2));
    events.clear();
    g.idle(5000, events);
    assert(events.empty());

    // Activity keeps the pinch alive: the idle time counts from the latest step.
    g.zoom(1.1, 700, 600, 2000, events);
    events.clear();
    g.zoom(1.1, 700, 600, 2100, events);
    g.idle(2100 + WheelGestures::kIdleMs - 1, events);
    assert(g.active());
    events.clear();
    g.cancel(2200, events);  // lifts at once
    assert(events.size() == 2 && !g.active());
    events.clear();
    g.cancel(2300, events);
    assert(events.empty());

    // Zooming in past the room on the screen lifts, starts again at the base spacing and goes on, so the total
    // zoom is what was asked for. Track the product of spacing ratios within each session.
    auto total_zoom = [&](double step, int steps, double expected_min_sessions) {
        WheelGestures z;
        z.set_view(kWidth, kHeight);
        Events all;
        double virtual_scale = 1.0, session_start = 0, requested = 1.0;
        int sessions = 0;
        double current = 0;
        for (int i = 0; i < steps; ++i) {
            Events step_events;
            z.zoom(step, 720, 692, 3000 + i * 10, step_events);
            requested *= step;
            for (size_t j = 0; j < step_events.size(); ++j) {
                const auto& e = step_events[j];
                assert(e.x >= 0 && e.x <= kWidth && e.y >= 0 && e.y <= kHeight);
                if (e.action == kDown && e.pointer_id == kB) { ++sessions; session_start = spacing(step_events[j - 1], e); current = session_start; }
                if (e.action == kMove && e.pointer_id == kB) {
                    const double now_spacing = spacing(step_events[j - 1], e);
                    assert(now_spacing >= low - 0.5 && now_spacing <= high + 0.5);
                    virtual_scale *= now_spacing / current;
                    current = now_spacing;
                }
            }
        }
        assert(sessions >= expected_min_sessions);
        assert(near(std::log(virtual_scale), std::log(requested), 0.002));
        return sessions;
    };
    assert(total_zoom(1.12, 5, 1) == 1);        // fits in one gesture
    assert(total_zoom(1.12, 60, 2) >= 2);       // far more than fits: restarts, same total zoom
    assert(total_zoom(1 / 1.12, 60, 2) >= 2);   // and out
    assert(total_zoom(1.01, 400, 2) >= 2);      // small precise steps of a trackpad

    // Bad factors do nothing.
    {
        WheelGestures z;
        z.set_view(kWidth, kHeight);
        Events none;
        z.zoom(0, 1, 1, 1, none); z.zoom(-2, 1, 1, 1, none); z.zoom(std::nan(""), 1, 1, 1, none);
        z.zoom(INFINITY, 1, 1, 1, none);
        assert(none.empty() && !z.active());
    }

    // A pan moves both fingers together by the same amount at a constant spacing.
    {
        WheelGestures p;
        p.set_view(kWidth, kHeight);
        Events moves;
        p.pan(0, 80, 700, 600, 4000, moves);
        assert(moves.size() == 4 && !p.dragging());
        check_pair(moves, 0, kDown, 4000);
        check_pair(moves, 2, kMove, 4000);
        assert(near(moves[2].y, 680) && near(moves[3].y, 680));
        assert(near(spacing(moves[2], moves[3]), base) && near((moves[2].x + moves[3].x) / 2, 700));
        moves.clear();
        p.pan(-50, 0, 0, 0, 4050, moves);
        assert(moves.size() == 2 && near((moves[0].x + moves[1].x) / 2, 650) && near(moves[0].y, 680));
        // Switching to a zoom lifts the pan first.
        moves.clear();
        p.zoom(1.1, 700, 600, 4100, moves);
        assert(moves.size() == 2 + 2 + 2 && moves[0].action == kUp && moves[1].action == kUp &&
               moves[2].action == kDown && moves[3].action == kDown && moves[4].action == kMove);
        moves.clear();
        p.cancel(4200, moves);
        // A long pan runs out of room: it lifts and starts again at the cursor, and never leaves the surface.
        WheelGestures q;
        q.set_view(kWidth, kHeight);
        int restarts = 0;
        for (int i = 0; i < 80; ++i) {
            Events step_events;
            q.pan(60, 0, 700, 600, 5000 + i, step_events);
            for (const auto& e : step_events) {
                assert(e.x >= 0 && e.x <= kWidth && e.y >= 0 && e.y <= kHeight);
                restarts += e.action == kDown && e.pointer_id == kA;
            }
        }
        assert(restarts >= 2);
    }

    // A held drag follows the cursor with the fingers until the button is released; the idle time does not lift it.
    {
        WheelGestures d;
        d.set_view(kWidth, kHeight);
        Events drag;
        d.drag_begin(500, 400, 6000, drag);
        assert(drag.size() == 2 && d.dragging());
        check_pair(drag, 0, kDown, 6000);
        drag.clear();
        d.drag_move(540, 430, 6010, drag);
        assert(drag.size() == 2 && near((drag[0].x + drag[1].x) / 2, 540) && near(drag[0].y, 430));
        drag.clear();
        d.idle(60000, drag);
        assert(drag.empty() && d.dragging());
        d.drag_move(540, 430, 6020, drag);  // unchanged: nothing to report
        assert(drag.empty());
        d.zoom(1.1, 1, 1, 6030, drag);      // a wheel step ends the drag and starts a pinch
        assert(drag.size() == 6 && drag[0].action == kUp && !d.dragging() && d.active());
        drag.clear();
        d.drag_begin(10, 10, 6040, drag);
        drag.clear();
        d.drag_end(6050, drag);
        assert(drag.size() == 2 && drag[0].action == kUp && drag[1].action == kUp && !d.active());
        drag.clear();
        d.drag_end(6060, drag);
        d.drag_move(1, 1, 6070, drag);
        assert(drag.empty());
    }

    // Near an edge or corner the fingers stay on the surface; an unset view still works.
    {
        WheelGestures edge;
        edge.set_view(kWidth, kHeight);
        Events corner;
        edge.zoom(1.3, 0, 0, 7000, corner);
        for (const auto& e : corner) assert(e.x >= 0 && e.x <= kWidth && e.y >= 0 && e.y <= kHeight);
        corner.clear();
        edge.cancel(7100, corner);
        edge.zoom(1.3, 99999, 99999, 7200, corner);
        for (const auto& e : corner) assert(e.x >= 0 && e.x <= kWidth && e.y >= 0 && e.y <= kHeight);
        WheelGestures unset;
        Events any;
        unset.zoom(1.2, 100, 100, 1, any);
        assert(any.size() == 4);
        WheelGestures tiny;
        tiny.set_view(100, 80);
        Events small;
        tiny.zoom(1.2, 50, 40, 1, small);
        for (const auto& e : small) assert(e.x >= 0 && e.x <= 100 && e.y >= 0 && e.y <= 80);
    }
    std::puts("android_wheel_gestures: ALL PASS");
    return 0;
}
