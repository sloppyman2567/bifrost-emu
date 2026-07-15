// frost/input.hpp — FrostInput: input event capture + queue (Turn 38-39).
//
// Captures keyboard, mouse, joystick, and game controller events from the
// host SDL2 window and exposes them to the guest via:
//   - /dev/input/eventX  (Linux input_event format, 24 bytes)
//   - /dev/input/js0     (Linux JS_EVENT format, 8 bytes)
//   - /dev/input/mice    (ImPS/2 mouse protocol, 4 bytes per packet)
//
// The event queues are bounded ring buffers. SDL2's event handler is the
// producer; the guest's read() is the consumer.
//
// Without SDL2 (headless build), all queues are always empty and
// poll_events() is a no-op. Guests that block on /dev/input/eventX
// will get EOF (read returns 0).
//
// ── Event formats ─────────────────────────────────────────────────────
//
// Linux input_event (24 bytes on AArch64):
//   struct input_event {
//       struct timeval time;  // 16 bytes (8 tv_sec + 8 tv_usec)
//       uint16_t type;        // EV_KEY=1, EV_REL=2, EV_ABS=3, EV_SYN=0
//       uint16_t code;        // BTN_LEFT=0x110, KEY_A=30, ABS_X=0, ...
//       int32_t  value;       // 1=press, 0=release, 2=repeat (for keys)
//                              // delta (for relative axes)
//                              // absolute position (for absolute axes)
//   };
//
// Linux JS_EVENT (8 bytes):
//   struct js_event {
//       uint32_t time;     // milliseconds since startup
//       int16_t  value;    // axis value or button value (0/1)
//       uint8_t  type;     // JS_EVENT_BUTTON=1, JS_EVENT_AXIS=2, INIT=0x80
//       uint8_t  number;   // axis/button index
//   };
//
// We emit:
//   - EV_KEY for keyboard + mouse buttons + joystick buttons
//   - EV_REL for mouse movement (REL_X, REL_Y, REL_WHEEL)
//   - EV_ABS for joystick axes (ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ,
//     ABS_HAT0X, ABS_HAT0Y)
//   - EV_SYN after each "batch" of events to delimit frames
//   - JS_EVENT_BUTTON / JS_EVENT_AXIS for /dev/input/js0
//
// ── Game controller support ─────────────────────────────────
// SDL2's game controller API provides a higher-level abstraction than the
// raw joystick API: it maps physical controls to standard names (A, B, X,
// Y, D-pad, left/right stick, triggers). We use SDL_GameControllerOpen
// to open all connected controllers and translate their events to both
// EV_ABS/EV_KEY (for eventX) and JS_EVENT (for js0).
//
// The mapping from SDL2 game controller axes to Linux ABS_* codes:
//   SDL_CONTROLLER_AXIS_LEFTX        → ABS_X
//   SDL_CONTROLLER_AXIS_LEFTY        → ABS_Y
//   SDL_CONTROLLER_AXIS_RIGHTX       → ABS_RX
//   SDL_CONTROLLER_AXIS_RIGHTY       → ABS_RY
//   SDL_CONTROLLER_AXIS_TRIGGERLEFT  → ABS_BRAKE
//   SDL_CONTROLLER_AXIS_TRIGGERRIGHT → ABS_GAS
//
// The mapping from SDL2 game controller buttons to Linux BTN_* codes:
//   SDL_CONTROLLER_BUTTON_A             → BTN_GAMEPAD (0x130)
//   SDL_CONTROLLER_BUTTON_B             → BTN_B (variant — we use BTN_EAST)
//   SDL_CONTROLLER_BUTTON_X             → BTN_NORTH
//   SDL_CONTROLLER_BUTTON_Y             → BTN_WEST (actually BTN_C)
//   SDL_CONTROLLER_BUTTON_LEFTSHOULDER  → BTN_TL
//   SDL_CONTROLLER_BUTTON_RIGHTSHOULDER → BTN_TR
//   SDL_CONTROLLER_BUTTON_START         → BTN_START
//   SDL_CONTROLLER_BUTTON_BACK          → BTN_SELECT
//   SDL_CONTROLLER_BUTTON_DPAD_UP       → BTN_DPAD_UP
//   SDL_CONTROLLER_BUTTON_DPAD_DOWN     → BTN_DPAD_DOWN
//   SDL_CONTROLLER_BUTTON_DPAD_LEFT     → BTN_DPAD_LEFT
//   SDL_CONTROLLER_BUTTON_DPAD_RIGHT    → BTN_DPAD_RIGHT
//
// ── Limitations ───────────────────────────────────────────────────────
//   - Only one logical input device (no /dev/input/event0 + event1).
//   - Only one game controller (js0). Multi-controller support is a
//     future enhancement.
//   - No multitouch (SDL2 has it; not yet plumbed through).
//   - Time stamps use CLOCK_REALTIME; real Linux uses CLOCK_MONOTONIC
//     for input events. Cosmetic difference — guests rarely care.
//   - JS_EVENT time is milliseconds since startup (we use a steady_clock
//     baseline). Real Linux uses jiffies.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <sys/types.h>  // ssize_t
#include <vector>
namespace arm64emu {
// Forward-declare the pimpl.
struct FrostInputImpl;
// Device type for read(). Selects which event format to return.
enum class InputDevice {
    Event,   // /dev/input/eventX — 24-byte input_event records
    Js,      // /dev/input/js0 — 8-byte js_event records
    Mouse,   // /dev/input/mice — 4-byte ImPS/2 packets (not yet implemented)
};
class FrostInput {
public:
    FrostInput();
    ~FrostInput();
    // Non-copyable, non-movable.
    FrostInput(const FrostInput&) = delete;
    FrostInput& operator=(const FrostInput&) = delete;
    // Whether input capture is active (requires SDL2). Always false in
    // headless builds.
    bool active() const;
    // Pump the host's event queue (SDL_PollEvent). Translates SDL2
    // events into Linux input events and pushes them to the ring
    // buffers. Should be called periodically by the run loop.
    // Returns false if the user requested window close (SDL_QUIT),
    // true otherwise.
    bool poll();
    // Read up to `n` bytes of event records from the queue into `buf`.
    // The format depends on `dev`:
    //   - InputDevice::Event: 24-byte input_event records
    //   - InputDevice::Js:    8-byte js_event records
    //   - InputDevice::Mouse: 4-byte ImPS/2 packets (not yet implemented)
    // Returns bytes read (multiple of the record size), 0 if the queue
    // is empty, or -1 on error.
    ssize_t read(uint8_t* buf, size_t n, InputDevice dev = InputDevice::Event,
                 bool blocking = false);
    // Drain pending events without delivering them. Used when the
    // guest closes /dev/input/eventX.
    void drain();
    // Total events captured since construction (diagnostic).
    uint64_t event_count() const;
    // Whether any game controllers are connected (diagnostic).
    bool has_game_controller() const;
    // Number of game controllers currently open.
    int game_controller_count() const;
private:
    std::unique_ptr<FrostInputImpl> impl_;
};
} // namespace arm64emu
