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
    // Gamepad buttons (from <linux/input-event-codes.h>).
    constexpr uint16_t BTN_GAMEPAD   = 0x130;  // A / Cross
    constexpr uint16_t BTN_EAST      = 0x131;  // B / Circle
    constexpr uint16_t BTN_NORTH     = 0x133;  // X / Triangle (varies)
    constexpr uint16_t BTN_WEST      = 0x134;  // Y / Square (varies)
    constexpr uint16_t BTN_TL        = 0x136;  // left shoulder
    constexpr uint16_t BTN_TR        = 0x137;  // right shoulder
    constexpr uint16_t BTN_TL2       = 0x138;  // left trigger (full press)
    constexpr uint16_t BTN_TR2       = 0x139;  // right trigger (full press)
    constexpr uint16_t BTN_SELECT    = 0x13a;
    constexpr uint16_t BTN_START     = 0x13b;
    constexpr uint16_t BTN_MODE      = 0x13c;  // center / home / guide
    constexpr uint16_t BTN_THUMBL    = 0x13d;  // left stick click
    constexpr uint16_t BTN_THUMBR    = 0x13e;  // right stick click
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
    std::vector<input_event_> event_queue;
    size_t event_head = 0, event_tail = 0;
    std::vector<js_event_> js_queue;
    size_t js_head = 0, js_tail = 0;
    std::mutex mu;
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
    FrostInputImpl()
        : event_queue(EVENT_CAP), js_queue(JS_CAP),
          startup_time(std::chrono::steady_clock::now()) {}
    // ── Push an input_event (24 bytes) into the event queue ─────────
    void push_event(uint16_t type, uint16_t code, int32_t value) {
        std::lock_guard<std::mutex> g(mu);
        auto now = std::chrono::system_clock::now();
        auto dur = now.time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
        auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(dur - secs);
        input_event_& ev = event_queue[event_tail];
        ev.tv_sec  = static_cast<int64_t>(secs.count());
        ev.tv_usec = static_cast<int64_t>(usecs.count());
        ev.type    = type;
        ev.code    = code;
        ev.value   = value;
        event_tail = (event_tail + 1) % EVENT_CAP;
        if (event_tail == event_head) {
            event_head = (event_head + 1) % EVENT_CAP;  // drop oldest
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
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                uint16_t code = sdl_scancode_to_linux(ev.key.keysym.scancode);
                if (code != linux_input::KEY_RESERVED) {
                    int32_t value = (ev.type == SDL_KEYDOWN) ? 1 : 0;
                    impl_->push_event(linux_input::EV_KEY, code, value);
                    impl_->push_event(linux_input::EV_SYN, 0, 0);
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
                impl_->push_event(linux_input::EV_REL, linux_input::REL_X,
                                  static_cast<int32_t>(ev.motion.xrel));
                impl_->push_event(linux_input::EV_REL, linux_input::REL_Y,
                                  static_cast<int32_t>(ev.motion.yrel));
                impl_->push_event(linux_input::EV_SYN, 0, 0);
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
}
// ── Diagnostics ─────────────────────────────────────────────────────────
uint64_t FrostInput::event_count() const {
    if (!impl_) return 0;
    return impl_->event_count;
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
