// frost_graphics/input.cpp — FrostInput: SDL2 → Linux input event translation.
//
// v1.4.5-alpha (Turn 38): NEW. Captures keyboard/mouse events from the
// host's SDL2 window and exposes them as Linux input_event records
// (24 bytes each on AArch64). The guest reads them via /dev/input/eventX.
//
// ── SDL2 → Linux input event translation ──────────────────────────────
// SDL2 uses its own keycode/scan set (SDL_Scancode, SDL_Keycode). We
// translate the common ones (letters, digits, arrow keys, modifiers)
// to Linux KEY_* codes (defined in <linux/input-event-codes.h>).
//
// Mouse buttons:
//   SDL_BUTTON_LEFT   → BTN_LEFT   (0x110)
//   SDL_BUTTON_MIDDLE → BTN_MIDDLE (0x112)
//   SDL_BUTTON_RIGHT  → BTN_RIGHT  (0x111)
//   SDL_BUTTON_X1     → BTN_SIDE   (0x113)
//   SDL_BUTTON_X2     → BTN_EXTRA  (0x114)
//
// Mouse motion:
//   SDL_MOUSEMOTION   → EV_REL REL_X / REL_Y
//   SDL_MOUSEWHEEL    → EV_REL REL_WHEEL (positive = up)
//
// ── Ring buffer ───────────────────────────────────────────────────────
// We use a bounded SPSC ring buffer protected by a mutex. The SDL2
// event handler (producer) calls push(); the guest's read() (consumer)
// calls pop(). Capacity is 256 events — plenty for typical use (a
// burst of keypresses + mouse motion). Overflows drop the oldest
// event (the guest will see a gap, but won't block).
#include "frost/input.hpp"

#if defined(BIFROST_USE_SDL2)
#  include <SDL2/SDL.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace arm64emu {

// ── Linux input event constants (from <linux/input-event-codes.h>) ─────
// We hardcode them (instead of #including the kernel header) to keep
// the build self-contained on hosts that may not have linux/input.h.
namespace linux_input {
    constexpr uint16_t EV_SYN = 0x00;
    constexpr uint16_t EV_KEY = 0x01;
    constexpr uint16_t EV_REL = 0x02;
    constexpr uint16_t EV_ABS = 0x03;

    // Mouse buttons.
    constexpr uint16_t BTN_LEFT   = 0x110;
    constexpr uint16_t BTN_RIGHT  = 0x111;
    constexpr uint16_t BTN_MIDDLE = 0x112;
    constexpr uint16_t BTN_SIDE   = 0x113;
    constexpr uint16_t BTN_EXTRA  = 0x114;

    // Relative axes.
    constexpr uint16_t REL_X      = 0x00;
    constexpr uint16_t REL_Y      = 0x01;
    constexpr uint16_t REL_WHEEL  = 0x08;

    // Common keyboard keys (subset — extend as needed).
    constexpr uint16_t KEY_RESERVED = 0;
    constexpr uint16_t KEY_ENTER    = 28;
    constexpr uint16_t KEY_ESC      = 1;
    constexpr uint16_t KEY_BACKSPACE = 14;
    constexpr uint16_t KEY_TAB      = 15;
    constexpr uint16_t KEY_SPACE    = 57;
    constexpr uint16_t KEY_LEFT     = 105;
    constexpr uint16_t KEY_RIGHT    = 106;
    constexpr uint16_t KEY_UP       = 103;
    constexpr uint16_t KEY_DOWN     = 108;
    constexpr uint16_t KEY_LEFTSHIFT  = 42;
    constexpr uint16_t KEY_RIGHTSHIFT = 54;
    constexpr uint16_t KEY_LEFTCTRL   = 29;
    constexpr uint16_t KEY_RIGHTCTRL  = 97;
    constexpr uint16_t KEY_LEFTALT    = 56;
    constexpr uint16_t KEY_RIGHTALT   = 100;
    constexpr uint16_t KEY_CAPSLOCK   = 58;
    constexpr uint16_t KEY_HOME     = 102;
    constexpr uint16_t KEY_END      = 107;
    constexpr uint16_t KEY_PAGEUP   = 104;
    constexpr uint16_t KEY_PAGEDOWN = 109;
    constexpr uint16_t KEY_INSERT   = 110;
    constexpr uint16_t KEY_DELETE   = 111;
}

// ── Linux input_event layout (AArch64) ─────────────────────────────────
// 24 bytes: 16 (timeval) + 2 (type) + 2 (code) + 4 (value).
// The timeval is two 64-bit fields (tv_sec, tv_usec) on AArch64.
struct input_event_ {
    int64_t tv_sec;    // seconds since epoch
    int64_t tv_usec;   // microseconds
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
static_assert(sizeof(input_event_) == 24, "input_event_ must be 24 bytes");

// ── SDL2 scancode → Linux KEY_* translation table ──────────────────────
// Covers the common keys. Unknown scancodes map to KEY_RESERVED (0),
// which the guest will ignore.
#if defined(BIFROST_USE_SDL2)
static uint16_t sdl_scancode_to_linux(SDL_Scancode sc) {
    // Letters and digits: SDL scancodes are arranged so SDL_SCANCODE_A
    // through SDL_SCANCODE_Z map directly to Linux KEY_A..KEY_Z (30..55),
    // and SDL_SCANCODE_1..SDL_SCANCODE_0 map to KEY_1..KEY_0 (2..11).
    // We handle these with arithmetic instead of a giant switch.
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return 30 + (sc - SDL_SCANCODE_A);  // KEY_A=30 .. KEY_Z=55
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return 2 + (sc - SDL_SCANCODE_1);   // KEY_1=2 .. KEY_9=10
    }
    if (sc == SDL_SCANCODE_0) return 11;    // KEY_0=11

    switch (sc) {
        case SDL_SCANCODE_RETURN:       return linux_input::KEY_ENTER;
        case SDL_SCANCODE_ESCAPE:       return linux_input::KEY_ESC;
        case SDL_SCANCODE_BACKSPACE:    return linux_input::KEY_BACKSPACE;
        case SDL_SCANCODE_TAB:          return linux_input::KEY_TAB;
        case SDL_SCANCODE_SPACE:        return linux_input::KEY_SPACE;
        case SDL_SCANCODE_LEFT:         return linux_input::KEY_LEFT;
        case SDL_SCANCODE_RIGHT:        return linux_input::KEY_RIGHT;
        case SDL_SCANCODE_UP:           return linux_input::KEY_UP;
        case SDL_SCANCODE_DOWN:         return linux_input::KEY_DOWN;
        case SDL_SCANCODE_LSHIFT:       return linux_input::KEY_LEFTSHIFT;
        case SDL_SCANCODE_RSHIFT:       return linux_input::KEY_RIGHTSHIFT;
        case SDL_SCANCODE_LCTRL:        return linux_input::KEY_LEFTCTRL;
        case SDL_SCANCODE_RCTRL:        return linux_input::KEY_RIGHTCTRL;
        case SDL_SCANCODE_LALT:         return linux_input::KEY_LEFTALT;
        case SDL_SCANCODE_RALT:         return linux_input::KEY_RIGHTALT;
        case SDL_SCANCODE_CAPSLOCK:     return linux_input::KEY_CAPSLOCK;
        case SDL_SCANCODE_HOME:         return linux_input::KEY_HOME;
        case SDL_SCANCODE_END:          return linux_input::KEY_END;
        case SDL_SCANCODE_PAGEUP:       return linux_input::KEY_PAGEUP;
        case SDL_SCANCODE_PAGEDOWN:     return linux_input::KEY_PAGEDOWN;
        case SDL_SCANCODE_INSERT:       return linux_input::KEY_INSERT;
        case SDL_SCANCODE_DELETE:       return linux_input::KEY_DELETE;
        default:                        return linux_input::KEY_RESERVED;
    }
}

static uint16_t sdl_mouse_button_to_linux(uint8_t btn) {
    switch (btn) {
        case SDL_BUTTON_LEFT:   return linux_input::BTN_LEFT;
        case SDL_BUTTON_MIDDLE: return linux_input::BTN_MIDDLE;
        case SDL_BUTTON_RIGHT:  return linux_input::BTN_RIGHT;
        case SDL_BUTTON_X1:     return linux_input::BTN_SIDE;
        case SDL_BUTTON_X2:     return linux_input::BTN_EXTRA;
        default:                return 0;
    }
}
#endif  // BIFROST_USE_SDL2

// ── FrostInputImpl — the real implementation (pimpl) ───────────────────
struct FrostInputImpl {
    // Bounded ring buffer of input_event_ records. Mutex-protected
    // (the SDL2 event handler runs on the main thread; the guest's
    // read() may run on any thread for spawned threads). 256 events
    // is plenty for typical interactive use.
    static constexpr size_t CAPACITY = 256;
    std::vector<input_event_> queue;
    size_t head = 0;  // consumer index
    size_t tail = 0;  // producer index
    std::mutex mu;

    uint64_t event_count = 0;
    bool sdl_active = false;

    FrostInputImpl() : queue(CAPACITY) {}

    // Push an event. Drops the oldest event if the queue is full.
    void push(uint16_t type, uint16_t code, int32_t value) {
        std::lock_guard<std::mutex> g(mu);
        // Time stamp: real Linux uses CLOCK_REALTIME for input events
        // (some guests check for monotonic — but most don't care).
        auto now = std::chrono::system_clock::now();
        auto dur = now.time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
        auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(dur - secs);

        input_event_& ev = queue[tail];
        ev.tv_sec  = static_cast<int64_t>(secs.count());
        ev.tv_usec = static_cast<int64_t>(usecs.count());
        ev.type    = type;
        ev.code    = code;
        ev.value   = value;

        tail = (tail + 1) % CAPACITY;
        if (tail == head) {
            // Queue full — drop the oldest by advancing head.
            head = (head + 1) % CAPACITY;
        }
        event_count++;
    }

    // Pop one event. Returns false if the queue is empty.
    bool pop(input_event_& out) {
        std::lock_guard<std::mutex> g(mu);
        if (head == tail) return false;
        out = queue[head];
        head = (head + 1) % CAPACITY;
        return true;
    }
};

// ── FrostInput method implementations ──────────────────────────────────
FrostInput::FrostInput() {
    impl_ = std::make_unique<FrostInputImpl>();
#if defined(BIFROST_USE_SDL2)
    // We don't SDL_InitSubSystem(SDL_INIT_EVENTS) here — that's done
    // by FrostGraphics when it initializes SDL_INIT_VIDEO. SDL2 events
    // are part of the video subsystem, so they're already available.
    impl_->sdl_active = true;
#else
    impl_->sdl_active = false;
#endif
}

FrostInput::~FrostInput() = default;

bool FrostInput::active() const {
    return impl_ && impl_->sdl_active;
}

// ── poll() — pump SDL2 events and translate to Linux input events ──────
bool FrostInput::poll() {
    if (!impl_ || !impl_->sdl_active) return true;
#if defined(BIFROST_USE_SDL2)
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                return false;  // caller should stop the guest
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    return false;
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                uint16_t code = sdl_scancode_to_linux(ev.key.keysym.scancode);
                if (code != linux_input::KEY_RESERVED) {
                    int32_t value = (ev.type == SDL_KEYDOWN) ? 1 : 0;
                    impl_->push(linux_input::EV_KEY, code, value);
                    // SYN event to delimit this input frame.
                    impl_->push(linux_input::EV_SYN, 0, 0);
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                uint16_t code = sdl_mouse_button_to_linux(ev.button.button);
                if (code != 0) {
                    int32_t value = (ev.type == SDL_MOUSEBUTTONDOWN) ? 1 : 0;
                    impl_->push(linux_input::EV_KEY, code, value);
                    impl_->push(linux_input::EV_SYN, 0, 0);
                }
                break;
            }
            case SDL_MOUSEMOTION: {
                // Relative motion (deltas). Linux input_event uses
                // REL_X/REL_Y for relative motion.
                impl_->push(linux_input::EV_REL, linux_input::REL_X,
                            static_cast<int32_t>(ev.motion.xrel));
                impl_->push(linux_input::EV_REL, linux_input::REL_Y,
                            static_cast<int32_t>(ev.motion.yrel));
                impl_->push(linux_input::EV_SYN, 0, 0);
                break;
            }
            case SDL_MOUSEWHEEL: {
                // Wheel: positive = up, negative = down. Linux uses
                // REL_WHEEL with the same sign convention.
                impl_->push(linux_input::EV_REL, linux_input::REL_WHEEL,
                            static_cast<int32_t>(ev.wheel.y));
                impl_->push(linux_input::EV_SYN, 0, 0);
                break;
            }
            default:
                // Unhandled event type — ignore. SDL2 has many event
                // types (text editing, joystick axis, controller, etc.)
                // that we don't translate yet. Future enhancement.
                break;
        }
    }
#endif  // BIFROST_USE_SDL2
    return true;
}

// ── read() — dequeue input_event records into the guest buffer ─────────
ssize_t FrostInput::read(uint8_t* buf, size_t n, bool blocking) {
    if (!impl_) return -ENODEV;
    if (n < sizeof(input_event_)) return 0;  // need at least 24 bytes

    (void)blocking;  // blocking not yet supported (would need a condvar)

    size_t max_events = n / sizeof(input_event_);
    size_t events_read = 0;
    auto* out = reinterpret_cast<input_event_*>(buf);

    while (events_read < max_events) {
        input_event_ ev;
        if (!impl_->pop(ev)) break;
        out[events_read++] = ev;
    }

    return static_cast<ssize_t>(events_read * sizeof(input_event_));
}

// ── drain() — discard all pending events ───────────────────────────────
void FrostInput::drain() {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->head = impl_->tail;
}

// ── event_count() — diagnostic ─────────────────────────────────────────
uint64_t FrostInput::event_count() const {
    if (!impl_) return 0;
    return impl_->event_count;
}

} // namespace arm64emu
