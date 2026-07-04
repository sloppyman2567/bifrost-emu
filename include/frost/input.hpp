// frost/input.hpp — FrostInput: input event capture + queue (Turn 38).
//
// Captures keyboard, mouse, and joystick events from the host SDL2
// window and exposes them to the guest via /dev/input/eventX and
// /dev/input/js0. The queue is a bounded SPSC ring buffer (the SDL2
// event handler is the producer; the guest's read() is the consumer).
//
// Without SDL2 (headless build), the queue is always empty and
// poll_events() is a no-op. Guests that block on /dev/input/eventX
// will get EOF (read returns 0).
//
// ── Event format ──────────────────────────────────────────────────────
// Linux input events are 24 bytes on AArch64:
//   struct input_event {
//       struct timeval time;  // 16 bytes (8 tv_sec + 8 tv_usec)
//       uint16_t type;        // EV_KEY=1, EV_REL=2, EV_ABS=3, EV_SYN=0
//       uint16_t code;        // BTN_LEFT=0x110, KEY_A=30, REL_X=0, ...
//       int32_t  value;       // 1=press, 0=release, 2=repeat (for keys)
//                              // delta (for relative axes)
//                              // absolute position (for absolute axes)
//   };
//
// We emit:
//   - EV_KEY for keyboard + mouse buttons (translated from SDL2 events)
//   - EV_REL for mouse movement (REL_X, REL_Y, REL_WHEEL)
//   - EV_SYN after each "batch" of events to delimit frames
//
// ── Limitations ───────────────────────────────────────────────────────
//   - Only one logical input device (no /dev/input/event0 + event1).
//   - Joystick axis values are 16-bit (Linux uses 16-bit for JS_EVENT).
//   - No multitouch (SDL2 has it; not yet plumbed through).
//   - Time stamps use CLOCK_REALTIME; real Linux uses CLOCK_MONOTONIC
//     for input events. Cosmetic difference — guests rarely care.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <sys/types.h>  // ssize_t
#include <vector>

namespace arm64emu {

// Forward-declare the pimpl.
struct FrostInputImpl;

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
    // buffer. Should be called periodically by the run loop.
    // Returns false if the user requested window close (SDL_QUIT),
    // true otherwise.
    bool poll();

    // Read up to `n` bytes of input_event records from the queue into
    // `buf`. Returns bytes read (always a multiple of 24, the size of
    // struct input_event), 0 if the queue is empty, or -1 on error.
    // Blocks if `blocking` is true and the queue is empty (until the
    // next poll() produces events).
    ssize_t read(uint8_t* buf, size_t n, bool blocking = false);

    // Drain pending events without delivering them. Used when the
    // guest closes /dev/input/eventX.
    void drain();

    // Total events captured since construction (diagnostic).
    uint64_t event_count() const;

private:
    std::unique_ptr<FrostInputImpl> impl_;
};

} // namespace arm64emu
