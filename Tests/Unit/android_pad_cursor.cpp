// PadCursor: a pointer for touch-only apps, driven by a gamepad (Start+Select toggles it, the left stick moves it, A touches).
#include "System/Android/Window/PadCursor.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using frost::PadCursor;
using frost::PadCursorMode;
using Outputs = std::vector<PadCursor::Output>;
namespace pb = frost::padbtn;
namespace pa = frost::padaxis;
constexpr int kB = 1;          // an SDL button the cursor never uses
constexpr int kRightX = 2;     // an SDL axis the cursor never uses

static int count(const Outputs &out, PadCursor::Output::Kind kind) {
  int n = 0;
  for (const auto &o : out) n += o.kind == kind;
  return n;
}
static const PadCursor::Output *first(const Outputs &out, PadCursor::Output::Kind kind) {
  for (const auto &o : out) if (o.kind == kind) return &o;
  return nullptr;
}
static bool near(float a, float b, float tolerance = 0.5f) { return std::fabs(a - b) <= tolerance; }
// The chord on a fresh cursor: first button at t=0, second at `gap` ms.
static void chord(PadCursor &c, int a, int b, int64_t gap, Outputs &out) {
  assert(!c.button(a, true, 0, out));       // the first press goes to the game
  c.button(b, true, gap, out);
}
static void push_stick(PadCursor &c, float x, float y) {
  c.axis(pa::kLeftX, int(x * 32767)); c.axis(pa::kLeftY, int(y * 32767));
}
// 16 ms ticks for `seconds`, returning the moves.
static Outputs run(PadCursor &c, int64_t &now, float seconds) {
  Outputs out;
  for (int i = 0, n = int(seconds * 1000 / 16); i < n; ++i) { now += 16; c.tick(now, out); }
  return out;
}

int main() {
  // Auto: off until the chord, and then nothing is used while it is off.
  {
    PadCursor c(PadCursorMode::Auto); Outputs out;
    c.set_bounds(1000, 500);
    assert(!c.active());
    assert(!c.axis(pa::kLeftX, 30000));
    assert(!c.button(pb::kSouth, true, 10, out) && out.empty());
    int64_t now = 0; push_stick(c, 1, 0);
    assert(run(c, now, 0.5f).empty());                                   // sticks move nothing while it is off
  }
  // The chord switches it on, in either order, and takes back the press the game already saw.
  {
    PadCursor c; Outputs out;
    chord(c, pb::kStart, pb::kBack, 100, out);
    assert(c.active());
    assert(count(out, PadCursor::Output::Toggled) == 1 && first(out, PadCursor::Output::Toggled)->on);
    assert(first(out, PadCursor::Output::ReleaseToGame)->button == pb::kStart);   // Start went to the game first
    out.clear();
    assert(c.button(pb::kStart, false, 200, out));                        // both releases belong to the cursor
    assert(c.button(pb::kBack, false, 210, out) && out.empty());
    PadCursor d; Outputs other;
    chord(d, pb::kBack, pb::kStart, 100, other);
    assert(d.active() && first(other, PadCursor::Output::ReleaseToGame)->button == pb::kBack);
  }
  // Not together: pressed more than the window apart, it is two ordinary presses.
  {
    PadCursor c; Outputs out;
    assert(!c.button(pb::kStart, true, 0, out));
    assert(!c.button(pb::kBack, true, PadCursor::kChordMs + 1, out));
    assert(!c.active() && out.empty());
    assert(!c.button(pb::kStart, false, 900, out) && !c.button(pb::kBack, false, 910, out));   // releases reach the game
  }
  // A release after a plain press is the game's, and a second chord switches it off again.
  {
    PadCursor c; Outputs out;
    chord(c, pb::kStart, pb::kBack, 50, out);
    c.button(pb::kStart, false, 300, out); c.button(pb::kBack, false, 310, out);
    out.clear();
    c.button(pb::kStart, true, 1000, out); c.button(pb::kBack, true, 1100, out);
    assert(!c.active() && !first(out, PadCursor::Output::Toggled)->on);
  }
  // Gamepad mode never has a cursor and leaves every press to the game.
  {
    PadCursor c(PadCursorMode::Gamepad); Outputs out;
    chord(c, pb::kStart, pb::kBack, 50, out);
    assert(!c.active() && out.empty());
    assert(!c.button(pb::kSouth, true, 100, out) && !c.axis(pa::kLeftX, 30000));
  }
  // Cursor mode starts on and the chord still switches it off.
  {
    PadCursor c(PadCursorMode::Cursor); Outputs out;
    assert(c.active());
    chord(c, pb::kStart, pb::kBack, 50, out);
    assert(!c.active());
  }
  // The left stick moves the pointer through a curve with a dead zone, clamped to the panel.
  {
    PadCursor c(PadCursorMode::Cursor); c.set_bounds(1000, 500);
    assert(near(c.x(), 500) && near(c.y(), 250));
    int64_t now = 1000; Outputs out; c.tick(now, out);
    push_stick(c, 0.1f, 0);                                              // inside the dead zone
    assert(run(c, now, 0.5f).empty());
    push_stick(c, 1, 0);
    Outputs moves = run(c, now, 0.2f);
    assert(!moves.empty() && moves.back().x > 500 && near(moves.back().y, 250) && !moves.back().held);
    const float after_full = c.x() - 500;
    assert(after_full > 80 && after_full < 140);                         // about 1.1 * 500 px/s for 0.2 s
    run(c, now, 3.0f);
    assert(near(c.x(), 999));                                           // stopped at the panel's edge
    push_stick(c, 0, -1); run(c, now, 3.0f);
    assert(near(c.y(), 0));
  }
  // The triggers slow and speed the pointer.
  {
    PadCursor slow(PadCursorMode::Cursor), normal(PadCursorMode::Cursor), fast(PadCursorMode::Cursor);
    for (PadCursor *c : {&slow, &normal, &fast}) { c->set_bounds(10000, 10000); push_stick(*c, 1, 0); }
    slow.axis(pa::kLeftTrigger, 32767); fast.axis(pa::kRightTrigger, 32767);
    int64_t t1 = 0, t2 = 0, t3 = 0;
    run(slow, t1, 0.15f); run(normal, t2, 0.15f); run(fast, t3, 0.15f);   // short: the fast pointer must not reach the edge
    const float s = slow.x() - 5000, n = normal.x() - 5000, f = fast.x() - 5000;
    assert(s > 0 && near(s * 4, n, n * 0.05f) && near(n * 2, f, n * 0.05f));
  }
  // A is the primary finger: down where the pointer is, a drag while it is down, up where it ended.
  {
    PadCursor c(PadCursorMode::Cursor); c.set_bounds(1000, 500); Outputs out;
    assert(c.button(pb::kSouth, true, 5, out));
    assert(out.size() == 1 && out[0].kind == PadCursor::Output::Down && out[0].finger == 0 && near(out[0].x, 500));
    out.clear();
    int64_t now = 0; c.tick(now, out); push_stick(c, 1, 0);
    Outputs drag = run(c, now, 0.2f);
    assert(!drag.empty() && drag.back().held);
    assert(c.button(pb::kSouth, false, 50, out));
    assert(out.size() == 1 && out[0].kind == PadCursor::Output::Up && near(out[0].x, c.x()));
    assert(c.button(pb::kSouth, false, 60, out));                         // a repeated release is still the cursor's
  }
  // X is a second finger that stays where it went down while the pointer moves on.
  {
    PadCursor c(PadCursorMode::Cursor); c.set_bounds(1000, 500); Outputs out;
    c.button(pb::kWest, true, 5, out);
    assert(out.size() == 1 && out[0].finger == 1 && near(out[0].x, 500));
    int64_t now = 0; c.tick(now, out); push_stick(c, 1, 0); run(c, now, 0.3f);
    out.clear(); c.button(pb::kWest, false, 100, out);
    assert(out.size() == 1 && out[0].kind == PadCursor::Output::Up && out[0].finger == 1 && near(out[0].x, 500));
  }
  // Switching it off lifts a finger that is down, and what the cursor does not use goes on to the game.
  {
    PadCursor c(PadCursorMode::Cursor); c.set_bounds(1000, 500); Outputs out;
    c.button(pb::kSouth, true, 0, out); out.clear();
    chord(c, pb::kStart, pb::kBack, 60, out);
    assert(count(out, PadCursor::Output::Up) == 1 && !c.active());
    PadCursor d(PadCursorMode::Cursor); Outputs unused;
    assert(!d.button(kB, true, 0, unused) && !d.axis(kRightX, 20000) && unused.empty());   // B stays Android's Back
  }
  // The arrow is drawn in code: opaque white inside a dark outline, the tip at the top left, transparent at the far corner.
  {
    for (int size : {24, 32, 48}) {
      const std::vector<uint32_t> pixels = frost::pad_cursor_bitmap(size);
      assert(int(pixels.size()) == size * size);
      assert(pixels[size - 1] >> 24 == 0);                                // top right corner: nothing
      assert(pixels[size_t(size - 1) * size + size - 1] >> 24 == 0);      // bottom right corner: nothing
      bool light = false, dark = false;
      for (uint32_t p : pixels) {
        if ((p >> 24) < 250) continue;
        const uint32_t grey = p & 0xff;
        light |= grey > 230; dark |= grey < 40;
      }
      assert(light && dark);
      assert((pixels[size_t(2) * size + 2] >> 24) > 200);                 // just inside the tip
    }
  }
  std::puts("android_pad_cursor: PASS");
  return 0;
}
