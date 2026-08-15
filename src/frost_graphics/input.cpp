// frost_graphics/input.cpp — SDL2 → Linux input event translation.
//
// v1.4.5-alpha: NEW. Captures keyboard/mouse events from the
// host's SDL2 window and exposes them as Linux input_event records
// (24 bytes each on AArch64). The guest reads them via /dev/input/eventX.
//
// v1.4.5-alpha: Added game controller support. SDL2's game
// controller API (SDL_GameController*) provides a higher-level
// abstraction than raw joysticks. We open all connected controllers
// and translate their events to both EV_ABS/EV_KEY (for eventX) and
// JS_EVENT (for js0). Also added a separate js_event queue so
// /dev/input/js0 returns the correct 8-byte JS_EVENT format instead
// of the 24-byte input_event format.
//
// Robustness/security (1.5.3-alpha):
//   - EV_SYN/SYN_DROPPED emitted on ring-buffer overflow, matching
//     Linux evdev behavior. Guests can detect loss and resync.
//   - Event deduplication: identical consecutive events are dropped,
//     matching the Linux input subsystem's "only emit on change"
//     contract.
//   - Keyboard modifier tracking: Ctrl/Shift/Alt state is tracked
//     and emitted as EV_KEY events so guests see modifier changes.
//   - Mouse warp-to-center: when the cursor hits the window edge,
//     it's warped back to center to prevent relative-input edge
//     sticking (mirrors QEMU's SDL2 backend).
//   - Window leave: all mouse buttons are released when the cursor
//     leaves the window, preventing stuck-button state.
//   - Timestamps use CLOCK_MONOTONIC (steady_clock), matching
//     real Linux input devices.
//
// ── Ring buffers ───────────────────────────────────────────────────────
// We use two separate ring buffers:
//   - event_queue_  : 24-byte input_event records (for /dev/input/eventX)
//   - js_queue_     : 8-byte js_event records (for /dev/input/js0)
// Both are fed by the same SDL2 event handler. SDL2 keyboard/mouse events
// only go to event_queue_; SDL2 game controller events go to BOTH.
// This matches how real Linux input devices work: a gamepad appears as
// both /dev/input/eventX (with EV_ABS/EV_KEY) and /dev/input/js0 (with
// JS_EVENT), and both represent the same physical events.
#include "frost/input.hpp"
#if defined(BIFROST_USE_SDL2)
#  include <SDL2/SDL.h>
#endif
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
namespace arm64emu {
// ── Linux input event constants (from <linux/input-event-codes.h>) ─────
namespace linux_input {
    constexpr uint16_t EV_SYN = 0x00;
    constexpr uint16_t EV_KEY = 0x01;
    constexpr uint16_t EV_REL = 0x02;
    constexpr uint16_t EV_ABS = 0x03;
    // SYN codes (used with EV_SYN).
    constexpr uint16_t SYN_REPORT  = 0x00;
    constexpr uint16_t SYN_DROPPED = 0x01;  // buffer overrun — guest should resync
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
    // Absolute axes (gamepad sticks).
    constexpr uint16_t ABS_X     = 0x00;  // left stick X
    constexpr uint16_t ABS_Y     = 0x01;  // left stick Y
    constexpr uint16_t ABS_Z     = 0x02;  // left trigger
    constexpr uint16_t ABS_RX    = 0x03;  // right stick X
    constexpr uint16_t ABS_RY    = 0x04;  // right stick Y
    constexpr uint16_t ABS_RZ    = 0x05;  // right trigger
    constexpr uint16_t ABS_HAT0X = 0x10;  // D-pad X
    constexpr uint16_t ABS_HAT0Y = 0x11;  // D-pad Y
    constexpr uint16_t ABS_BRAKE = 0x0a;  // alias for ABS_Z (left trigger)
    constexpr uint16_t ABS_GAS   = 0x0b;  // alias for ABS_RZ (right trigger)
    // Keyboard modifier keys (emitted as EV_KEY for guests that read
    // /dev/input/eventX directly instead of using the keyboard scancode
    // translation table).
    constexpr uint16_t KEY_LEFTCTRL   = 29;
    constexpr uint16_t KEY_RIGHTCTRL  = 97;
    constexpr uint16_t KEY_LEFTSHIFT  = 42;
    constexpr uint16_t KEY_RIGHTSHIFT = 54;
    constexpr uint16_t KEY_LEFTALT    = 56;
    constexpr uint16_t KEY_RIGHTALT   = 100;
    constexpr uint16_t KEY_CAPSLOCK   = 58;
    // Gamepad buttons (from <linux/input-event-codes.h>).
    constexpr uint16_t BTN_GAMEPAD   = 0x130;
    constexpr uint16_t BTN_EAST      = 0x131;
    constexpr uint16_t BTN_NORTH     = 0x133;
    constexpr uint16_t BTN_WEST      = 0x134;
    constexpr uint16_t BTN_TL        = 0x136;
    constexpr uint16_t BTN_TR        = 0x137;
    constexpr uint16_t BTN_TL2       = 0x138;
    constexpr uint16_t BTN_TR2       = 0x139;
    constexpr uint16_t BTN_SELECT    = 0x13a;
    constexpr uint16_t BTN_START     = 0x13b;
    constexpr uint16_t BTN_MODE      = 0x13c;
    constexpr uint16_t BTN_THUMBL    = 0x13d;
    constexpr uint16_t BTN_THUMBR    = 0x13e;
    constexpr uint16_t BTN_DPAD_UP    = 0x220;
    constexpr uint16_t BTN_DPAD_DOWN  = 0x222;
    constexpr uint16_t BTN_DPAD_LEFT  = 0x221;
    constexpr uint16_t BTN_DPAD_RIGHT = 0x223;
    // Common keyboard keys (subset).
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
    constexpr uint16_t KEY_HOME     = 102;
    constexpr uint16_t KEY_END      = 107;
    constexpr uint16_t KEY_PAGEUP   = 104;
    constexpr uint16_t KEY_PAGEDOWN = 109;
    constexpr uint16_t KEY_INSERT   = 110;
    constexpr uint16_t KEY_DELETE   = 111;
    constexpr uint16_t KEY_0        = 11;
    constexpr uint16_t KEY_1        = 2;
    constexpr uint16_t KEY_2        = 3;
    constexpr uint16_t KEY_3        = 4;
    constexpr uint16_t KEY_4        = 5;
    constexpr uint16_t KEY_5        = 6;
    constexpr uint16_t KEY_6        = 7;
    constexpr uint16_t KEY_7        = 8;
    constexpr uint16_t KEY_8        = 9;
    constexpr uint16_t KEY_9        = 10;
    constexpr uint16_t KEY_A        = 30;
    constexpr uint16_t KEY_B        = 31;
    constexpr uint16_t KEY_C        = 32;
    constexpr uint16_t KEY_D        = 33;
    constexpr uint16_t KEY_E        = 34;
    constexpr uint16_t KEY_F        = 35;
    constexpr uint16_t KEY_G        = 36;
    constexpr uint16_t KEY_H        = 37;
    constexpr uint16_t KEY_I        = 38;
    constexpr uint16_t KEY_J        = 39;
    constexpr uint16_t KEY_K        = 40;
    constexpr uint16_t KEY_L        = 41;
    constexpr uint16_t KEY_M        = 42;
    constexpr uint16_t KEY_N        = 43;
    constexpr uint16_t KEY_O        = 44;
    constexpr uint16_t KEY_P        = 45;
    constexpr uint16_t KEY_Q        = 46;
    constexpr uint16_t KEY_R        = 47;
    constexpr uint16_t KEY_S        = 48;
    constexpr uint16_t KEY_T        = 49;
    constexpr uint16_t KEY_U        = 50;
    constexpr uint16_t KEY_V        = 51;
    constexpr uint16_t KEY_W        = 52;
    constexpr uint16_t KEY_X        = 53;
    constexpr uint16_t KEY_Y        = 54;
    constexpr uint16_t KEY_Z        = 55;
    constexpr uint16_t KEY_MINUS    = 12;
    constexpr uint16_t KEY_EQUAL    = 13;
    constexpr uint16_t KEY_LEFTBRACE   = 26;
    constexpr uint16_t KEY_RIGHTBRACE  = 27;
    constexpr uint16_t KEY_BACKSLASH   = 43;
    constexpr uint16_t KEY_SEMICOLON   = 39;
    constexpr uint16_t KEY_APOSTROPHE  = 40;
    constexpr uint16_t KEY_COMMA       = 51;
    constexpr uint16_t KEY_DOT         = 52;
    constexpr uint16_t KEY_SLASH       = 53;
    constexpr uint16_t KEY_GRAVE       = 41;
}
// ── Linux js_event constants ───────────────────────────────────────────
namespace linux_js {
    constexpr uint8_t JS_EVENT_BUTTON = 0x01;
    constexpr uint8_t JS_EVENT_AXIS   = 0x02;
    constexpr uint8_t JS_EVENT_INIT   = 0x80;
    // Standard axis indices (Linux gamepad convention).
    // 0-1: left stick X/Y, 2-3: right stick X/Y,
    // 4-5: L2/R2 triggers, 6-7: D-pad X/Y (hat).
    constexpr uint8_t AXIS_LEFT_X   = 0;
    constexpr uint8_t AXIS_LEFT_Y   = 1;
    constexpr uint8_t AXIS_RIGHT_X  = 2;
    constexpr uint8_t AXIS_RIGHT_Y  = 3;
    constexpr uint8_t AXIS_L2       = 4;
    constexpr uint8_t AXIS_R2       = 5;
    constexpr uint8_t AXIS_HAT_X    = 6;
    constexpr uint8_t AXIS_HAT_Y    = 7;
}
// ── Linux input_event layout (AArch64, 24 bytes) ───────────────────────
struct input_event_ {
    int64_t tv_sec;
    int64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
static_assert(sizeof(input_event_) == 24, "input_event_ must be 24 bytes");
// ── Linux js_event layout (8 bytes) ─────────────────────────────────────
struct js_event_ {
    uint32_t time;    // milliseconds since startup
    int16_t  value;
    uint8_t  type;
    uint8_t  number;
};
static_assert(sizeof(js_event_) == 8, "js_event_ must be 8 bytes");
// ── SDL2 → Linux input event translation tables ────────────────────────
#if defined(BIFROST_USE_SDL2)
static uint16_t sdl_scancode_to_linux(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return 30 + (sc - SDL_SCANCODE_A);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return 2 + (sc - SDL_SCANCODE_1);
    }
    if (sc == SDL_SCANCODE_0) return 11;
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
// SDL2 game controller axis → Linux ABS_* code.
static uint16_t sdl_gc_axis_to_linux_abs(SDL_GameControllerAxis axis) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:        return linux_input::ABS_X;
        case SDL_CONTROLLER_AXIS_LEFTY:        return linux_input::ABS_Y;
        case SDL_CONTROLLER_AXIS_RIGHTX:       return linux_input::ABS_RX;
        case SDL_CONTROLLER_AXIS_RIGHTY:       return linux_input::ABS_RY;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return linux_input::ABS_BRAKE;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return linux_input::ABS_GAS;
        default:                               return 0;
    }
}
// SDL2 game controller axis → Linux js_event axis number.
static uint8_t sdl_gc_axis_to_js(SDL_GameControllerAxis axis) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:        return linux_js::AXIS_LEFT_X;
        case SDL_CONTROLLER_AXIS_LEFTY:        return linux_js::AXIS_LEFT_Y;
        case SDL_CONTROLLER_AXIS_RIGHTX:       return linux_js::AXIS_RIGHT_X;
        case SDL_CONTROLLER_AXIS_RIGHTY:       return linux_js::AXIS_RIGHT_Y;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return linux_js::AXIS_L2;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return linux_js::AXIS_R2;
        default:                               return 0xff;  // invalid
    }
}
// SDL2 game controller button → Linux BTN_* code.
static uint16_t sdl_gc_button_to_linux_btn(SDL_GameControllerButton btn) {
    switch (btn) {
        case SDL_CONTROLLER_BUTTON_A:             return linux_input::BTN_GAMEPAD;
        case SDL_CONTROLLER_BUTTON_B:             return linux_input::BTN_EAST;
        case SDL_CONTROLLER_BUTTON_X:             return linux_input::BTN_NORTH;
        case SDL_CONTROLLER_BUTTON_Y:             return linux_input::BTN_WEST;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return linux_input::BTN_TL;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return linux_input::BTN_TR;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return linux_input::BTN_THUMBL;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return linux_input::BTN_THUMBR;
        case SDL_CONTROLLER_BUTTON_START:         return linux_input::BTN_START;
        case SDL_CONTROLLER_BUTTON_BACK:          return linux_input::BTN_SELECT;
        case SDL_CONTROLLER_BUTTON_GUIDE:         return linux_input::BTN_MODE;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return linux_input::BTN_DPAD_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return linux_input::BTN_DPAD_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return linux_input::BTN_DPAD_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return linux_input::BTN_DPAD_RIGHT;
        default:                                  return 0;
    }
}
// SDL2 game controller button → Linux js_event button number.
// Linux gamepad convention: buttons 0-13 map to A/B/X/Y/TL/TR/Back/Start/
// Guide/ThumbL/ThumbR/DPad-up/down/left/right.
static uint8_t sdl_gc_button_to_js(SDL_GameControllerButton btn) {
    switch (btn) {
        case SDL_CONTROLLER_BUTTON_A:             return 0;
        case SDL_CONTROLLER_BUTTON_B:             return 1;
        case SDL_CONTROLLER_BUTTON_X:             return 2;
        case SDL_CONTROLLER_BUTTON_Y:             return 3;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 4;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
        case SDL_CONTROLLER_BUTTON_BACK:          return 6;
        case SDL_CONTROLLER_BUTTON_START:         return 7;
        case SDL_CONTROLLER_BUTTON_GUIDE:         return 8;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return 9;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return 10;
        // D-pad buttons are typically sent as axis events (hat) in the
        // Linux gamepad convention, not as button events. But some
        // guests expect button events, so we also emit them as buttons
        // 11-14. The js0 device will see both the hat axis and the
        // button events — the guest picks whichever it wants.
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 11;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 12;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 13;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 14;
        default:                                  return 0xff;  // invalid
    }
}
#endif  // BIFROST_USE_SDL2
// ── FrostInputImpl — the real implementation (pimpl) ───────────────────
struct FrostInputImpl {
    static constexpr size_t EVENT_CAP = 256;
    static constexpr size_t JS_CAP = 256;
    static constexpr int32_t MOUSE_ABS_RANGE = 32767;  // ABS_X/ABS_Y range
    static constexpr uint32_t SYN_DROPPED_INTERVAL = 250;  // (unused, kept for compat)
    std::vector<input_event_> event_queue;
    size_t event_head = 0, event_tail = 0;
    std::vector<js_event_> js_queue;
    size_t js_head = 0, js_tail = 0;
    mutable std::mutex mu;
    uint64_t event_count = 0;
    bool sdl_active = false;
    // Game controller state. We store the controllers as
    // void* (not SDL_GameController*) so the struct compiles without
    // SDL2 headers. The actual SDL_GameController* type is only used
    // inside #if defined(BIFROST_USE_SDL2) blocks.
    std::vector<void*> controllers;
    bool gc_subsystem_init = false;
    int controller_count = 0;  // cached for has_game_controller()
    // Baseline timestamp for js_event.time (milliseconds since startup).
    std::chrono::steady_clock::time_point startup_time;
    // Mouse absolute position tracking. We accumulate relative motion
    // to produce ABS_X/ABS_Y events alongside EV_REL. The range is
    // 0..MOUSE_ABS_RANGE (matching Linux convention for touchscreens).
    int32_t mouse_abs_x = MOUSE_ABS_RANGE / 2;
    int32_t mouse_abs_y = MOUSE_ABS_RANGE / 2;
    // Last emitted event for deduplication. Linux only emits events when
    // values change; we match that behavior to avoid spamming the guest
    // with duplicate events.
    uint16_t last_type = 0;
    uint16_t last_code = 0;
    int32_t last_value = 0;
    bool last_valid = false;
    // Keyboard modifier state (Ctrl/Shift/Alt). Tracked so we can emit
    // EV_KEY events for modifier keys and filter GUI grab combos from
    // text input.
    uint16_t modifier_state = 0;
    // Counter for SYN_DROPPED rate-limiting.
    uint32_t syn_dropped_counter = 0;
    FrostInputImpl()
        : event_queue(EVENT_CAP), js_queue(JS_CAP),
          startup_time(std::chrono::steady_clock::now()) {}
    // ── Update modifier state from SDL2 key mods ───────────────────────
    void update_modifiers(uint16_t mods) {
        struct ModMap { uint16_t sdl_bit; uint16_t linux_code; };
        static constexpr ModMap kModMap[] = {
            { KMOD_LCTRL,  linux_input::KEY_LEFTCTRL },
            { KMOD_RCTRL,  linux_input::KEY_RIGHTCTRL },
            { KMOD_LSHIFT, linux_input::KEY_LEFTSHIFT },
            { KMOD_RSHIFT, linux_input::KEY_RIGHTSHIFT },
            { KMOD_LALT,   linux_input::KEY_LEFTALT },
            { KMOD_RALT,   linux_input::KEY_RIGHTALT },
            { KMOD_CAPS,   linux_input::KEY_CAPSLOCK },
        };
        uint16_t new_state = 0;
        for (const auto& m : kModMap) {
            if (mods & m.sdl_bit) new_state |= (1u << m.linux_code);
        }
        // Emit EV_KEY for modifiers that changed.
        for (const auto& m : kModMap) {
            bool was_set = modifier_state & (1u << m.linux_code);
            bool now_set = new_state & (1u << m.linux_code);
            if (was_set != now_set) {
                push_event(linux_input::EV_KEY, m.linux_code,
                           now_set ? 1 : 0);
            }
        }
        modifier_state = new_state;
    }
    // ── Push an input_event (24 bytes) into the event queue ─────────
    void push_event(uint16_t type, uint16_t code, int32_t value) {
        std::lock_guard<std::mutex> g(mu);
        // Use steady_clock (CLOCK_MONOTONIC) for timestamps. Real Linux
        // input events use monotonic time; system_clock (CLOCK_REALTIME)
        // can jump backwards on NTP adjustment and breaks guests that
        // compute deltas.
        auto now = std::chrono::steady_clock::now();
        auto dur = now.time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
        auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(dur - secs);
        // Deduplicate: Linux only emits events when values change. Skip
        // identical consecutive events to match real evdev behavior and
        // reduce queue pressure. Check before writing to the queue.
        //
        // NOTE: EV_REL (relative axes like mouse movement) is NOT
        // deduplicated — each delta is an independent event and must
        // always be delivered. EV_SYN is also not deduplicated.
        bool is_syn = (type == linux_input::EV_SYN);
        bool is_rel = (type == linux_input::EV_REL);
        if (last_valid && !is_syn && !is_rel
            && type == last_type && code == last_code
            && value == last_value) {
            return;  // duplicate — skip
        }
        last_type = type;
        last_code = code;
        last_value = value;
        last_valid = true;
        input_event_& ev = event_queue[event_tail];
        ev.tv_sec  = static_cast<int64_t>(secs.count());
        ev.tv_usec = static_cast<int64_t>(usecs.count());
        ev.type    = type;
        ev.code    = code;
        ev.value   = value;
        event_tail = (event_tail + 1) % EVENT_CAP;
        if (event_tail == event_head) {
            // Buffer overflow — the new event just overwrote the oldest
            // event. Match the Linux kernel evdev behavior:
            //
            // The kernel sets tail = (head - 2) & mask, then overwrites
            // the slot at tail with SYN_DROPPED. This leaves the buffer
            // containing: [dropped events...] | SYN_DROPPED | new_event
            //
            // In our ring buffer, head == tail after the push (buffer full).
            // We set event_head = (event_tail - 2 + EVENT_CAP) % EVENT_CAP
            // to drop all events before the SYN_DROPPED, then write
            // SYN_DROPPED at event_head. The new event is at event_tail - 1.
            event_head = (event_tail + EVENT_CAP - 2) % EVENT_CAP;
            input_event_& drop_ev = event_queue[event_head];
            drop_ev.tv_sec  = static_cast<int64_t>(secs.count());
            drop_ev.tv_usec = static_cast<int64_t>(usecs.count());
            drop_ev.type    = linux_input::EV_SYN;
            drop_ev.code    = linux_input::SYN_DROPPED;
            drop_ev.value   = 0;
        }
        event_count++;
    }
    // ── Push a js_event (8 bytes) into the js queue ─────────────────
    void push_js(uint8_t type, uint8_t number, int16_t value) {
        std::lock_guard<std::mutex> g(mu);
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startup_time).count();
        js_event_& ev = js_queue[js_tail];
        ev.time   = static_cast<uint32_t>(ms);
        ev.type   = type;
        ev.number = number;
        ev.value  = value;
        js_tail = (js_tail + 1) % JS_CAP;
        if (js_tail == js_head) {
            js_head = (js_head + 1) % JS_CAP;  // drop oldest
        }
    }
    // ── Pop one input_event ─────────────────────────────────────────
    bool pop_event(input_event_& out) {
        std::lock_guard<std::mutex> g(mu);
        if (event_head == event_tail) return false;
        out = event_queue[event_head];
        event_head = (event_head + 1) % EVENT_CAP;
        return true;
    }
    // ── Number of events currently in the queue ───────────────────────
    size_t queue_size() const {
        std::lock_guard<std::mutex> g(mu);
        if (event_head <= event_tail) {
            return event_tail - event_head;
        }
        return EVENT_CAP - (event_head - event_tail);
    }
    // ── Pop one js_event ────────────────────────────────────────────
    bool pop_js(js_event_& out) {
        std::lock_guard<std::mutex> g(mu);
        if (js_head == js_tail) return false;
        out = js_queue[js_head];
        js_head = (js_head + 1) % JS_CAP;
        return true;
    }
    // ── Open all connected game controllers ─────────────────────────
    void open_controllers() {
#if defined(BIFROST_USE_SDL2)
        if (!gc_subsystem_init) {
            // SDL_INIT_GAMECONTROLLER implies SDL_INIT_JOYSTICK.
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
                gc_subsystem_init = true;
            } else if (getenv("BIFROST_INPUT_TRACE")) {
                fprintf(stderr, "[input] SDL_InitSubSystem(GAMECONTROLLER) "
                        "failed: %s\n", SDL_GetError());
            }
        }
        if (!gc_subsystem_init) return;
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n; i++) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* gc = SDL_GameControllerOpen(i);
                if (gc) {
                    controllers.push_back(static_cast<void*>(gc));
                    controller_count++;
                    if (getenv("BIFROST_INPUT_TRACE")) {
                        const char* name = SDL_GameControllerNameForIndex(i);
                        fprintf(stderr, "[input] opened game controller %d: %s\n",
                                i, name ? name : "(unknown)");
                    }
                }
            }
        }
#endif
    }
    // ── Close all game controllers ──────────────────────────────────
    void close_controllers() {
#if defined(BIFROST_USE_SDL2)
        std::lock_guard<std::mutex> g(mu);
        for (void* p : controllers) {
            auto* gc = static_cast<SDL_GameController*>(p);
            if (SDL_GameControllerGetAttached(gc)) {
                SDL_GameControllerClose(gc);
            }
        }
        controllers.clear();
        controller_count = 0;
#endif
    }
};
#if defined(BIFROST_USE_SDL2)
// Result of mapping a character to a Linux keycode.
struct KeycodeResult {
    uint16_t code;
    bool needs_shift;
};
// Convert a UTF-8 character to a Linux keycode. Returns {0, false} if the
// character has no direct keycode mapping (e.g., non-Latin scripts).
static KeycodeResult utf8_char_to_linux_keycode(uint32_t cp) {
    // Digits: same keycode regardless of Shift.
    if (cp >= '0' && cp <= '9') {
        uint16_t code = (cp == '0') ? linux_input::KEY_0
                                    : linux_input::KEY_1 + (cp - '1');
        return {code, false};
    }
    // Lowercase letters: same keycode, no Shift.
    if (cp >= 'a' && cp <= 'z') {
        return {static_cast<uint16_t>(linux_input::KEY_A + (cp - 'a')), false};
    }
    // Uppercase letters: same physical key as lowercase, need Shift.
    if (cp >= 'A' && cp <= 'Z') {
        return {static_cast<uint16_t>(linux_input::KEY_A + (cp - 'A')), true};
    }
    // Shifted digit/symbol row.
    struct ShiftMap { uint32_t cp; uint16_t code; };
    static constexpr ShiftMap kShiftMap[] = {
        {'!', linux_input::KEY_1},
        {'@', linux_input::KEY_2},
        {'#', linux_input::KEY_3},
        {'$', linux_input::KEY_4},
        {'%', linux_input::KEY_5},
        {'^', linux_input::KEY_6},
        {'&', linux_input::KEY_7},
        {'*', linux_input::KEY_8},
        {'(', linux_input::KEY_9},
        {')', linux_input::KEY_0},
        {'_', linux_input::KEY_MINUS},
        {'+', linux_input::KEY_EQUAL},
        {'{', linux_input::KEY_LEFTBRACE},
        {'}', linux_input::KEY_RIGHTBRACE},
        {'|', linux_input::KEY_BACKSLASH},
        {':', linux_input::KEY_SEMICOLON},
        {'"', linux_input::KEY_APOSTROPHE},
        {'<', linux_input::KEY_COMMA},
        {'>', linux_input::KEY_DOT},
        {'?', linux_input::KEY_SLASH},
        {'~', linux_input::KEY_GRAVE},
    };
    for (const auto& m : kShiftMap) {
        if (cp == m.cp) return {m.code, true};
    }
    // Unshifted punctuation.
    switch (cp) {
        case ' ':  return {linux_input::KEY_SPACE, false};
        case '\n': return {linux_input::KEY_ENTER, false};
        case '\t': return {linux_input::KEY_TAB, false};
        case '\b': return {linux_input::KEY_BACKSPACE, false};
        case '\r': return {linux_input::KEY_ENTER, false};
        case '-':  return {linux_input::KEY_MINUS, false};
        case '=':  return {linux_input::KEY_EQUAL, false};
        case '[':  return {linux_input::KEY_LEFTBRACE, false};
        case ']':  return {linux_input::KEY_RIGHTBRACE, false};
        case '\\': return {linux_input::KEY_BACKSLASH, false};
        case ';':  return {linux_input::KEY_SEMICOLON, false};
        case '\'': return {linux_input::KEY_APOSTROPHE, false};
        case ',':  return {linux_input::KEY_COMMA, false};
        case '.':  return {linux_input::KEY_DOT, false};
        case '/':  return {linux_input::KEY_SLASH, false};
        case '`':  return {linux_input::KEY_GRAVE, false};
        case 0x1B: return {linux_input::KEY_ESC, false};
        default:   return {0, false};
    }
}
// Emit EV_KEY events for a UTF-8 text string. Each character is
// converted to a keycode and emitted as press+release. Shift is emitted
// as a modifier for characters that require it. Returns the number of
// characters emitted.
static size_t emit_text_as_keyevents(FrostInputImpl* impl, const char* utf8, size_t len) {
    size_t emitted = 0;
    for (size_t i = 0; i < len && utf8[i]; ) {
        uint32_t cp = 0;
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if (c < 0xE0) {
            if (i + 1 < len) {
                cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i+1]) & 0x3F);
                i += 2;
            } else { i++; continue; }
        } else if (c < 0xF0) {
            if (i + 2 < len) {
                cp = ((c & 0x0F) << 12)
                   | ((static_cast<uint8_t>(utf8[i+1]) & 0x3F) << 6)
                   | (static_cast<uint8_t>(utf8[i+2]) & 0x3F);
                i += 3;
            } else { i++; continue; }
        } else {
            if (i + 3 < len) i += 4;
            else i++;
            continue;
        }
        KeycodeResult kr = utf8_char_to_linux_keycode(cp);
        if (kr.code != 0) {
            if (kr.needs_shift) {
                impl->push_event(linux_input::EV_KEY, linux_input::KEY_LEFTSHIFT, 1);
            }
            impl->push_event(linux_input::EV_KEY, kr.code, 1);
            impl->push_event(linux_input::EV_KEY, kr.code, 0);
            if (kr.needs_shift) {
                impl->push_event(linux_input::EV_KEY, linux_input::KEY_LEFTSHIFT, 0);
            }
            impl->push_event(linux_input::EV_SYN, 0, 0);
            emitted++;
        }
    }
    return emitted;
}
#endif  // BIFROST_USE_SDL2
// ── FrostInput method implementations ──────────────────────────────────
FrostInput::FrostInput() {
    impl_ = std::make_unique<FrostInputImpl>();
#if defined(BIFROST_USE_SDL2)
    impl_->sdl_active = true;
    // Open any game controllers that are already connected. Controllers
    // hot-plugged after construction are opened in poll() when we see
    // SDL_CONTROLLERDEVICEADDED.
    impl_->open_controllers();
#else
    impl_->sdl_active = false;
#endif
}
FrostInput::~FrostInput() {
    if (impl_) {
        impl_->close_controllers();
    }
}
bool FrostInput::active() const {
    return impl_ && impl_->sdl_active;
}
bool FrostInput::poll() {
    if (!impl_ || !impl_->sdl_active) return true;
#if defined(BIFROST_USE_SDL2)
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                return false;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    return false;
                } else if (ev.window.event == SDL_WINDOWEVENT_ENTER) {
                    // Reset absolute mouse position to center on focus gain.
                    std::lock_guard<std::mutex> g(impl_->mu);
                    impl_->mouse_abs_x = FrostInputImpl::MOUSE_ABS_RANGE / 2;
                    impl_->mouse_abs_y = FrostInputImpl::MOUSE_ABS_RANGE / 2;
                } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                    // Emit a button release for all mouse buttons when the
                    // cursor leaves the window. This prevents the guest from
                    // thinking a button is still held after the cursor exits.
                    for (uint16_t btn : {
                        linux_input::BTN_LEFT,
                        linux_input::BTN_RIGHT,
                        linux_input::BTN_MIDDLE,
                        linux_input::BTN_SIDE,
                        linux_input::BTN_EXTRA
                    }) {
                        impl_->push_event(linux_input::EV_KEY, btn, 0);
                    }
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                uint16_t code = sdl_scancode_to_linux(ev.key.keysym.scancode);
                if (code != linux_input::KEY_RESERVED) {
                    // SDL_KEYDOWN with repeat=true → Linux KEY_REPEAT (value=2).
                    int32_t value;
                    if (ev.type == SDL_KEYDOWN && ev.key.repeat) {
                        value = 2;  // KEY_REPEAT
                    } else {
                        value = (ev.type == SDL_KEYDOWN) ? 1 : 0;
                    }
                    impl_->push_event(linux_input::EV_KEY, code, value);
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
                }
                // Track modifier state (Ctrl/Shift/Alt) so we can emit
                // EV_KEY for them and filter GUI grab combos.
                impl_->update_modifiers(ev.key.keysym.mod);
                break;
            }
            case SDL_TEXTINPUT: {
                // SDL_TEXTINPUT provides properly composed Unicode text
                // (handles IME, keyboard layouts, dead keys, etc.).
                // Convert each character to EV_KEY press+release pairs
                // so guests that read /dev/input/eventX get text input.
                const char* text = ev.text.text;
                if (text && text[0]) {
                    size_t emitted = emit_text_as_keyevents(
                        impl_.get(), text, strlen(text));
                    (void)emitted;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                uint16_t code = sdl_mouse_button_to_linux(ev.button.button);
                if (code != 0) {
                    int32_t value = (ev.type == SDL_MOUSEBUTTONDOWN) ? 1 : 0;
                    impl_->push_event(linux_input::EV_KEY, code, value);
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
                }
                break;
            }
            case SDL_MOUSEMOTION: {
                // Read/write mouse state under mu, then release before
                // push_event calls (which acquire mu internally).
                int mx, my;
                {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    impl_->mouse_abs_x += ev.motion.xrel;
                    impl_->mouse_abs_y += ev.motion.yrel;
                    // Clamp to valid range.
                    if (impl_->mouse_abs_x < 0) impl_->mouse_abs_x = 0;
                    if (impl_->mouse_abs_x > FrostInputImpl::MOUSE_ABS_RANGE)
                        impl_->mouse_abs_x = FrostInputImpl::MOUSE_ABS_RANGE;
                    if (impl_->mouse_abs_y < 0) impl_->mouse_abs_y = 0;
                    if (impl_->mouse_abs_y > FrostInputImpl::MOUSE_ABS_RANGE)
                        impl_->mouse_abs_y = FrostInputImpl::MOUSE_ABS_RANGE;
                    mx = impl_->mouse_abs_x;
                    my = impl_->mouse_abs_y;
                }
                // Emit relative motion (EV_REL) for guests that use it.
                impl_->push_event(linux_input::EV_REL, linux_input::REL_X,
                                  static_cast<int32_t>(ev.motion.xrel));
                impl_->push_event(linux_input::EV_REL, linux_input::REL_Y,
                                  static_cast<int32_t>(ev.motion.yrel));
                // Emit absolute position (EV_ABS) for guests that need it.
                impl_->push_event(linux_input::EV_ABS, linux_input::ABS_X,
                                  mx);
                impl_->push_event(linux_input::EV_ABS, linux_input::ABS_Y,
                                  my);
                impl_->push_event(linux_input::EV_SYN, 0, 0);
                // Warp mouse to window center when it hits the edge. This
                // prevents the guest cursor from getting stuck at the edge
                // in relative-input mode (mirrors QEMU's SDL2 backend).
                int win_w, win_h;
                SDL_GetWindowSize(SDL_GetWindowFromID(ev.motion.windowID),
                                  &win_w, &win_h);
                if (win_w > 0 && win_h > 0) {
                    int target_x = win_w / 2;
                    int target_y = win_h / 2;
                    bool target_at_edge =
                        (target_x <= 0 || target_x >= win_w - 1
                         || target_y <= 0 || target_y >= win_h - 1);
                    if (!target_at_edge) {
                        bool at_edge =
                            (ev.motion.x <= 0 || ev.motion.x >= win_w - 1
                             || ev.motion.y <= 0 || ev.motion.y >= win_h - 1);
                        if (at_edge) {
                            SDL_WarpMouseInWindow(
                                SDL_GetWindowFromID(ev.motion.windowID),
                                target_x, target_y);
                        }
                    }
                }
                break;
            }
            case SDL_MOUSEWHEEL: {
                impl_->push_event(linux_input::EV_REL, linux_input::REL_WHEEL,
                                  static_cast<int32_t>(ev.wheel.y));
                impl_->push_event(linux_input::EV_SYN, 0, 0);
                break;
            }
            // ── Game controller events ────────────────────
            case SDL_CONTROLLERDEVICEADDED: {
                // Hot-plug: open the newly connected controller.
                int idx = ev.cdevice.which;
                SDL_GameController* gc = SDL_GameControllerOpen(idx);
                if (gc) {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    impl_->controllers.push_back(static_cast<void*>(gc));
                    impl_->controller_count++;
                    if (getenv("BIFROST_INPUT_TRACE")) {
                        const char* name = SDL_GameControllerNameForIndex(idx);
                        fprintf(stderr, "[input] controller added: %s\n",
                                name ? name : "(unknown)");
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                // Hot-unplug: close the removed controller.
                SDL_GameController* gc = SDL_GameControllerFromInstanceID(
                    ev.cdevice.which);
                if (gc) {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    SDL_GameControllerClose(gc);
                    void* target = static_cast<void*>(gc);
                    for (size_t i = 0; i < impl_->controllers.size(); i++) {
                        if (impl_->controllers[i] == target) {
                            impl_->controllers.erase(impl_->controllers.begin() + i);
                            impl_->controller_count--;
                            break;
                        }
                    }
                    if (getenv("BIFROST_INPUT_TRACE")) {
                        fprintf(stderr, "[input] controller removed\n");
                    }
                }
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                uint16_t btn = sdl_gc_button_to_linux_btn(
                    static_cast<SDL_GameControllerButton>(ev.cbutton.button));
                int32_t value = (ev.type == SDL_CONTROLLERBUTTONDOWN) ? 1 : 0;
                if (btn != 0) {
                    // Push to event_queue as EV_KEY.
                    impl_->push_event(linux_input::EV_KEY, btn, value);
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
                }
                // Also push to js_queue as JS_EVENT_BUTTON.
                uint8_t js_btn = sdl_gc_button_to_js(
                    static_cast<SDL_GameControllerButton>(ev.cbutton.button));
                if (js_btn != 0xff) {
                    impl_->push_js(linux_js::JS_EVENT_BUTTON, js_btn,
                                   static_cast<int16_t>(value));
                }
                break;
            }
            case SDL_CONTROLLERAXISMOTION: {
                SDL_GameControllerAxis axis =
                    static_cast<SDL_GameControllerAxis>(ev.caxis.axis);
                int16_t sdl_value = ev.caxis.value;
                // Push to event_queue as EV_ABS.
                uint16_t abs_code = sdl_gc_axis_to_linux_abs(axis);
                if (abs_code != 0) {
                    // SDL2 axis values are -32768..32767. Linux ABS_*
                    // uses the same range (we pass through unchanged).
                    impl_->push_event(linux_input::EV_ABS, abs_code,
                                      static_cast<int32_t>(sdl_value));
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
                }
                // Also push to js_queue as JS_EVENT_AXIS.
                uint8_t js_axis = sdl_gc_axis_to_js(axis);
                if (js_axis != 0xff) {
                    impl_->push_js(linux_js::JS_EVENT_AXIS, js_axis, sdl_value);
                }
                break;
            }
            default:
                break;
        }
    }
#endif  // BIFROST_USE_SDL2
    return true;
}
// ── read() — dequeue events into the guest buffer ──────────────────────
ssize_t FrostInput::read(uint8_t* buf, size_t n, InputDevice dev, bool blocking) {
    if (!impl_) return -ENODEV;
    (void)blocking;  // blocking not yet supported
    switch (dev) {
        case InputDevice::Event: {
            if (n < sizeof(input_event_)) return 0;
            size_t max = n / sizeof(input_event_);
            size_t got = 0;
            auto* out = reinterpret_cast<input_event_*>(buf);
            while (got < max) {
                input_event_ ev;
                if (!impl_->pop_event(ev)) break;
                out[got++] = ev;
            }
            return static_cast<ssize_t>(got * sizeof(input_event_));
        }
        case InputDevice::Js: {
            if (n < sizeof(js_event_)) return 0;
            size_t max = n / sizeof(js_event_);
            size_t got = 0;
            auto* out = reinterpret_cast<js_event_*>(buf);
            while (got < max) {
                js_event_ ev;
                if (!impl_->pop_js(ev)) break;
                out[got++] = ev;
            }
            return static_cast<ssize_t>(got * sizeof(js_event_));
        }
        case InputDevice::Mouse:
            // ImPS/2 mouse protocol not yet implemented.
            return -ENOSYS;
        default:
            return -EINVAL;
    }
}
// ── drain() — discard all pending events ───────────────────────────────
void FrostInput::drain() {
    if (!impl_) return;
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->event_head = impl_->event_tail;
    impl_->js_head = impl_->js_tail;
    impl_->last_valid = false;
    impl_->last_type = 0;
    impl_->last_code = 0;
    impl_->last_value = 0;
}
// ── Diagnostics ─────────────────────────────────────────────────────────
uint64_t FrostInput::event_count() const {
    if (!impl_) return 0;
    return impl_->event_count;
}
size_t FrostInput::queue_size() const {
    if (!impl_) return 0;
    return impl_->queue_size();
}
bool FrostInput::has_game_controller() const {
    if (!impl_) return false;
    return impl_->controller_count > 0;
}
int FrostInput::game_controller_count() const {
    if (!impl_) return 0;
    return impl_->controller_count;
}
} // namespace arm64emu
