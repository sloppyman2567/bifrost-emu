// Surface.hpp — Android NativeActivity layer (v2).
//
// v1 scope: ANativeWindow shims for NativeActivity-style guest apps. A
// guest surface handle is a fake opaque pointer (0xA9xx… range) that the
// EGL interception in GraphicThunk::dispatch substitutes for the real
// host native window (X11 Window) when the guest calls
// eglCreateWindowSurface. Each live surface has its own native backing;
// managed adapters resolve Java object identity before thunking.
//
// v2 scope (2026-08): full NativeActivity lifecycle driver + ALooper +
// AInputQueue + AConfiguration + liblog stubs, so guest .so's built
// against android_native_app_glue boot under `bifrost-emu --android
// libfoo.so`. The manager plays the role of the Android framework:
//   - create_activity() synthesizes an ANativeActivity struct + callback
//     table in GUEST memory (modern 16-entry NDK layout;
//     BIFROST_ANDROID_LEGACY_CB=1 switches to the pre-API-26 layout);
//   - the emulator calls the guest's ANativeActivity_onCreate via the
//     borrow-CPU runner, then fires onStart/onResume/window callbacks —
//     each a GUEST function pointer read live from the callback table;
//   - glue threads block in our ALooper_pollAll thunk, which reports
//     readiness of registered fds (real ::poll on the resolved HOST fd —
//     guest pipes are real host pipes) and input-queue attachments;
//   - loopers are per guest thread (AOSP `Looper::getForThread` is
//     thread-local); each looper also carries that thread's
//     AChoreographer frame callbacks, dispatched on a 60 Hz
//     CLOCK_MONOTONIC tick from inside the owning thread's poll;
//   - ASensorManager reports a device with no sensors.
//   - SDL events are translated to AMotionEvent/AKeyEvent objects stored
//     host-side; AInputQueue_getEvent hands out stable guest handles and
//     the AMotionEvent_*/AKeyEvent_* getters read them back.
#ifndef FROST_ANDROID_SURFACE_HPP_
#define FROST_ANDROID_SURFACE_HPP_

#include "System/Android/Window/WindowBackend.hpp"
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace arm64emu {
class Memory;
class CPU;
}

namespace frost {

class AndroidSurfaceManager {
public:
    // Guest-visible shim handle range. Unmistakable, never a real pointer.
    static constexpr uint64_t kHandleBase = 0xA90000000000ULL;
    // ANativeWindow format codes (android/rect.h + nativewindow.h).
    static constexpr int32_t kFormatRGBA8888 = 1;
    static constexpr int32_t kFormatRGBX8888 = 2;
    static constexpr int32_t kBaseWidth = 800;
    static constexpr int32_t kBaseHeight = 600;

    // ── v2 handle ranges (same unmistakable 0xA9xx style) ──────────────
    static constexpr uint64_t kLooperBase = 0xA90001000000ULL;
    static constexpr uint64_t kQueueBase  = 0xA90002000000ULL;
    static constexpr uint64_t kEventBase  = 0xA90003000000ULL;
    // 0xA90004… is the fake AConfiguration handle (DisplayThunk.cpp).
    static constexpr uint64_t kChoreographerBase = 0xA90005000000ULL;
    static constexpr uint64_t kSensorManager     = 0xA90006000001ULL;
    static constexpr uint64_t kSensorQueueBase   = 0xA90007000000ULL;
    // Host frame clock for AChoreographer (no display refresh query yet).
    static constexpr int64_t kVsyncPeriodNs = 16666667;

    // Borrow-CPU guest function runner (installed by the Emulator after
    // thunk init). Runs fn on `cpu` with x0..x7 = iargs and returns x0.
    using GuestRunner = std::function<uint64_t(
        arm64emu::CPU& cpu, uint64_t fn, const int64_t* iargs, size_t n_iargs)>;
    // Guest fd → host fd resolver (installed by the Emulator). Returns
    // -1 when the fd is not open or is not backed by a host descriptor.
    using FdResolver = std::function<int(int guest_fd)>;

    static AndroidSurfaceManager& instance();
    // Independent host adapter context, useful to embedders and plugins.
    // Throws if the backend cannot accept the initial geometry.
    explicit AndroidSurfaceManager(std::unique_ptr<AndroidWindowBackend> backend);
    ~AndroidSurfaceManager();

    // ── Wiring (Emulator side) ─────────────────────────────────────────
    void set_memory(arm64emu::Memory* m) { mem_ = m; }
    void set_runner(GuestRunner r);
    void set_fd_resolver(FdResolver r);

    // Backend owns its host window; no SDL pointer crosses this interface.
    bool open_host_window();
    int set_window_identity(const std::string& title, const uint8_t* rgba, int32_t width, int32_t height);
    void close_host_window();
    bool ready() const { return window_open_; }
    // Commit geometry only when the backend accepts the host notification.
    int handle_host_resize(int32_t width, int32_t height);

    // ── ANativeWindow shims (v1 API, unchanged) ────────────────────────
    // ANativeWindow_fromSurface: returns a stable shim for this identity (0 on
    // failure — matches Android returning NULL without a valid surface).
    // Stable identity supplied by the managed JNI adapter (not a jobject address).
    // Identity zero is reserved for the NativeActivity primary surface.
    uint64_t from_surface(uint64_t identity = 0);
    static bool is_shim(uint64_t handle) {
        return handle >= kHandleBase && handle <= kHandleBase + 0xFFFF;
    }
    // References retain surface backing through Java and EGL ownership.
    void acquire(uint64_t handle);
    void release(uint64_t handle);
    void register_egl_surface(uint64_t egl_surface, uint64_t handle);
    void destroy_egl_surface(uint64_t egl_surface);
    bool present_egl_surface(uint64_t egl_surface);
    int set_surface_opaque(uint64_t handle, bool opaque);
    bool surface_opaque(uint64_t handle) const;

    int32_t width(uint64_t handle = 0) const;
    int32_t height(uint64_t handle = 0) const;
    int32_t format(uint64_t handle = 0) const;
    // ANativeWindow_setBuffersGeometry: both dimensions must be zero or
    // positive. Zero restores the adapter's base geometry/format.
    int32_t set_buffers_geometry(int32_t w, int32_t h, int32_t fmt, uint64_t handle = 0);
    // An advisory content-rate hint; this backend keeps its current display mode.
    int32_t set_frame_rate(uint64_t handle, float rate, int8_t compatibility, int8_t strategy);

    // The host-side native window value host EGL expects for
    // eglCreateWindowSurface: X11 Window id on an X11-backed display,
    // wl_egl_window* on Wayland. 0 when unknown — callers must fail the
    // surface creation.
    uint64_t host_native_window(uint64_t handle = 0);
    // The native display the guest's eglGetDisplay(EGL_DEFAULT_DISPLAY)
    // should wrap so it lands on the SAME connection as the surface:
    // wl_display* on Wayland, Display* on X11.
    void* native_display() const;
    // Install at startup, before a host window or guest surface exists.
    bool install_window_backend(std::unique_ptr<AndroidWindowBackend> backend);
    const char* window_backend_name() const { return window_backend_->name(); }

    // ── NativeActivity lifecycle (v2) ──────────────────────────────────
    // ANativeActivityCallbacks slots (MODERN NDK layout — the first five
    // match every NDK version; the rest follow the API-26+ header).
    enum ActivityCb {
        CB_ON_START = 0,
        CB_ON_RESUME,
        CB_ON_SAVE_INSTANCE_STATE,
        CB_ON_PAUSE,
        CB_ON_STOP,
        CB_ON_DESTROY,
        CB_ON_WINDOW_FOCUS,
        CB_ON_NATIVE_WINDOW_CREATED,
        CB_ON_NATIVE_WINDOW_RESIZED,
        CB_ON_NATIVE_WINDOW_REDRAW_NEEDED,
        CB_ON_NATIVE_WINDOW_DESTROYED,
        CB_ON_INPUT_QUEUE_CREATED,
        CB_ON_INPUT_QUEUE_DESTROYED,
        CB_ON_CONTENT_RECT_CHANGED,
        CB_ON_CONFIGURATION_CHANGED,
        CB_ON_LOW_MEMORY,
        CB_COUNT
    };
    // Legacy (pre-API-26) callback-table slot numbers for the callbacks
    // we actually fire. BIFROST_ANDROID_LEGACY_CB=1 selects this table:
    //   start resume saveState pause stop configChanged lowMemory focus
    //   winCreated winDestroyed inputQCreated inputQDestroyed contentRect
    static int legacy_cb_slot(ActivityCb cb) {
        switch (cb) {
        case CB_ON_START:                    return 0;
        case CB_ON_RESUME:                   return 1;
        case CB_ON_SAVE_INSTANCE_STATE:      return 2;
        case CB_ON_PAUSE:                    return 3;
        case CB_ON_STOP:                     return 4;
        case CB_ON_CONFIGURATION_CHANGED:    return 5;
        case CB_ON_LOW_MEMORY:               return 6;
        case CB_ON_WINDOW_FOCUS:             return 7;
        case CB_ON_NATIVE_WINDOW_CREATED:    return 8;
        case CB_ON_NATIVE_WINDOW_DESTROYED:  return 9;
        case CB_ON_INPUT_QUEUE_CREATED:      return 10;
        case CB_ON_INPUT_QUEUE_DESTROYED:    return 11;
        case CB_ON_CONTENT_RECT_CHANGED:     return 12;
        default:                             return -1;
        }
    }
    static bool legacy_callbacks() {
        static const bool legacy =
            getenv("BIFROST_ANDROID_LEGACY_CB") != nullptr;
        return legacy;
    }

    // Synthesize the ANativeActivity + callback table + path strings in
    // guest memory. Returns the activity guest address (0 on failure).
    uint64_t create_activity();
    uint64_t activity_addr() const { return activity_addr_; }

    // Fire lifecycle callback `cb` (x0 = activity, x1.. = extra args)
    // through the borrow-CPU runner, reading the function pointer LIVE
    // from the guest callback table (the guest fills it during onCreate).
    // Returns x0 from the callback. No-op (returns 0) when unset.
    uint64_t fire_activity_cb(arm64emu::CPU& cpu, ActivityCb cb,
                              const int64_t* extra = nullptr,
                              size_t n_extra = 0);

    // Input queue handle handed to the guest via onInputQueueCreated.
    uint64_t input_queue_handle() const { return kQueueBase | 1; }
    static bool is_queue(uint64_t h) {
        return h == (kQueueBase | 1);
    }
    static bool is_event(uint64_t h) {
        return h >= kEventBase && h < kEventBase + 1024 * 16;
    }

    // ── Looper registry (thunk API) ────────────────────────────────────
    // One looper per guest thread, keyed by CPU::tid (a monotonic counter,
    // never reused within a process). Loopers live until process exit;
    // ALooper_acquire/release are not reference-counted.
    static bool is_looper_handle(uint64_t h) {
        return (h & ~0xFFFFFFULL) == kLooperBase && (h & 0xFFFFFF) != 0;
    }
    // ALooper_prepare: the calling thread's looper, created on first use.
    // opts bit 0 = ALOOPER_PREPARE_ALLOW_NON_CALLBACKS.
    uint64_t looper_prepare(int tid, int opts);
    // ALooper_forThread: the calling thread's looper, or 0.
    uint64_t looper_for_thread(int tid);
    // ALooper_addFd (AOSP Looper::addFd rules): returns 1, or -1 for a bad
    // looper/fd, a NULL callback on a looper without ALLOW_NON_CALLBACKS,
    // or a NULL callback with ident < 0.
    int looper_add_fd(uint64_t looper, int fd, int ident, int events,
                      uint64_t callback /*guest fn*/, void* data);
    int looper_remove_fd(uint64_t looper, int fd);
    // ALooper_wake: unblock a poll in progress on that looper.
    void looper_wake(uint64_t looper);
    // ALooper_pollOnce/pollAll on the calling thread's looper
    // (ALOOPER_POLL_ERROR when it has none). Returns ident >= 0 and fills
    // out_fd/out_events/out_data, or a negative ALOOPER_POLL_* code.
    // Callbacks (fd, input queue, choreographer) run via the runner;
    // pollOnce then reports ALOOPER_POLL_CALLBACK (-2) and pollAll keeps
    // waiting. A callback returning 0 unregisters its fd.
    int looper_poll(arm64emu::CPU& cpu, int timeout_ms, int* out_fd, int* out_events,
                     void** out_data, bool poll_all);

    // ── AChoreographer (thunk API) ─────────────────────────────────────
    // getInstance: the calling thread's choreographer, 0 without a looper.
    uint64_t choreographer_for_thread(int tid);
    // postFrameCallback*: queue `fn(frameTimeNanos, data)` on the
    // choreographer's looper, due after `delay_ns`. Returns false for an
    // unknown handle or NULL fn.
    bool choreographer_post(uint64_t choreographer, uint64_t fn,
                            uint64_t data, int64_t delay_ns);

    // ── ASensorManager (thunk API): a device with no sensors ───────────
    // Guest address of an empty, NULL-terminated ASensorList.
    uint64_t sensor_empty_list();
    // createEventQueue: 0 when the looper is NULL. The queue never has
    // events, so it registers nothing with the looper.
    uint64_t sensor_create_queue(uint64_t looper);
    bool sensor_destroy_queue(uint64_t queue);
    bool sensor_is_queue(uint64_t queue);

    // ── Input queue (thunk API) ────────────────────────────────────────
    // AInputQueue_attachLooper/detachLooper.
    void queue_attach_looper(uint64_t queue, uint64_t looper, int ident,
                             uint64_t callback, void* data);
    void queue_detach_looper(uint64_t queue);
    // AInputQueue_getEvent: pops the next queued event into a free slot
    // and writes its handle to *out_event. Returns 0 on success, a
    // negative errno-style value when the queue is empty.
    int queue_get_event(uint64_t queue, uint64_t* out_event);
    // AInputQueue_preDispatchEvent: 0 = not consumed.
    int queue_pre_dispatch(uint64_t queue, uint64_t event);
    // AInputQueue_finishEvent.
    void queue_finish_event(uint64_t queue, uint64_t event);

    // ── Event store (getter thunks read these) ─────────────────────────
    // Minimal AInputEvent model. Motion events keep up to 8 pointers.
    struct TouchPointer {
        float x = 0, y = 0, pressure = 1.0f, size = 1.0f;
        int id = 0;
    };
    struct InputEvent {
        int32_t type = 0;          // AINPUT_EVENT_TYPE_*
        int32_t device_id = 1;
        int32_t source = 0;        // AINPUT_SOURCE_*
        int32_t action = 0;        // AKEY/AMOTION_EVENT_ACTION_*
        int32_t meta_state = 0;
        int32_t key_code = 0;
        int32_t scan_code = 0;
        int32_t repeat_count = 0;
        int32_t edge_flags = 0;
        int32_t flags = 0;
        int64_t down_time_ms = 0;
        int64_t event_time_ms = 0;
        float x_precision = 1.0f, y_precision = 1.0f;
        TouchPointer pointers[8];
        int32_t pointer_count = 0;
    };

 private:
    AndroidSurfaceManager();
    // Resolve a guest event handle to its live slot (nullptr if stale).
    const InputEvent* find_event(uint64_t handle) const;

 public:
    // Field accessors used by the AInputEvent_getType/AKeyEvent_*/
    // AMotionEvent_* getter thunks.
    int32_t event_type(uint64_t h);
    int32_t event_device_id(uint64_t h);
    int32_t event_source(uint64_t h);
    int32_t motion_action(uint64_t h);
    int32_t motion_pointer_count(uint64_t h);
    int32_t motion_pointer_id(uint64_t h, size_t idx);
    float   motion_x(uint64_t h, size_t idx);
    float   motion_y(uint64_t h, size_t idx);
    float   motion_pressure(uint64_t h, size_t idx);
    float   motion_size(uint64_t h, size_t idx);
    float   motion_touch_major(uint64_t h, size_t idx);
    float   motion_touch_minor(uint64_t h, size_t idx);
    int64_t motion_down_time(uint64_t h);
    int64_t motion_event_time(uint64_t h);
    float   motion_axis_value(uint64_t h, int axis, size_t idx);
    int32_t key_action(uint64_t h);
    int32_t key_code(uint64_t h);
    int32_t key_meta_state(uint64_t h);
    int32_t key_repeat_count(uint64_t h);
    int32_t key_scan_code(uint64_t h);
    int32_t key_flags(uint64_t h);
    int64_t key_down_time(uint64_t h);
    int64_t key_event_time(uint64_t h);

    // ── Host event pump (driver loop calls this every few ms) ──────────
    // Drains SDL events on the proxy window: translates mouse/touch/
    // keyboard into queued input events, resizes into geometry updates +
    // onNativeWindowResized/onContentRectChanged lifecycle fires (when a
    // CPU is provided), and sets the quit flag on window close.
    // cpu may be nullptr (no lifecycle fires — pure translation).
    void pump_host_events(arm64emu::CPU* cpu);
    bool quit_requested() const { return quit_; }
    void request_quit() { quit_ = true; }

    // Queue one synthetic tap at the client-area center (test hook:
    // BIFROST_ANDROID_TAP=1 makes the driver inject it once after the
    // input queue is delivered — exercises the full input path without
    // a human moving the mouse).
    void inject_test_tap();

 private:
    arm64emu::Memory* mem_ = nullptr;
    GuestRunner runner_;
    FdResolver fd_resolver_;
    void translate_window_event_(const AndroidWindowEvent& event);

    bool window_open_ = false;
    int64_t pointer_down_time_ms_ = 0;
    std::unique_ptr<AndroidWindowBackend> window_backend_;
    struct Surface {
        uint64_t identity = 0;
        size_t refs = 1;
        int32_t width = kBaseWidth, height = kBaseHeight, format = kFormatRGBA8888;
        bool opaque = false;
        std::unique_ptr<AndroidWindowBackend> backend;
    };
    mutable std::mutex surface_mu_;
    std::map<uint64_t, Surface> surfaces_;
    std::map<uint64_t, uint64_t> egl_surfaces_;
    uint64_t surface_handle_ = 0;
    uint64_t next_surface_ = 0;
    int32_t width_ = kBaseWidth;
    int32_t height_ = kBaseHeight;
    int32_t format_ = kFormatRGBA8888;

    // ── Activity state (v2) ────────────────────────────────────────────
    std::mutex mu_;
    uint64_t activity_addr_ = 0;      // guest ANativeActivity*
    uint64_t callbacks_addr_ = 0;     // guest ANativeActivityCallbacks*
    uint64_t rect_addr_ = 0;          // guest ARect scratch
    uint64_t strings_addr_ = 0;       // path strings block base
    bool quit_ = false;
    bool tap_injected_ = false;

    // ── Looper registry ────────────────────────────────────────────────
    struct FdEntry {
        int fd = -1;
        int ident = 0;
        int events = 0;
        uint64_t callback = 0;   // guest fn or 0
        void* data = nullptr;
    };
    struct FrameCallback {
        uint64_t fn = 0;
        uint64_t data = 0;
        int64_t due_ns = 0;      // CLOCK_MONOTONIC
    };
    struct Looper {
        int tid = 0;
        bool allow_non_callbacks = false;
        bool wake = false;
        std::vector<FdEntry> fds;
        std::vector<FrameCallback> frames;
    };
    std::map<uint64_t, Looper> loopers_;     // handle → looper
    std::map<int, uint64_t> thread_loopers_; // tid → handle
    uint64_t next_looper_id_ = 1;
    Looper* find_looper_(uint64_t handle);   // caller holds mu_

    // ── Sensors ────────────────────────────────────────────────────────
    uint64_t sensor_list_addr_ = 0;
    std::map<uint64_t, uint64_t> sensor_queues_;  // queue → looper
    uint64_t next_sensor_queue_ = 1;

    // ── Input queue ────────────────────────────────────────────────────
    struct QueueAttachment {
        uint64_t looper = 0;
        int ident = 0;
        uint64_t callback = 0;
        void* data = nullptr;
    };
    QueueAttachment queue_attach_;
    std::deque<InputEvent> pending_;
    struct Slot { bool used = false; InputEvent ev; };
    Slot slots_[32];
    uint64_t next_time_ms_ = 1000;
    int32_t last_pump_w_ = -1, last_pump_h_ = -1;

    uint64_t alloc_event_slot_(const InputEvent& ev);  // caller holds mu_
};

}  // namespace frost

#endif  // FROST_ANDROID_SURFACE_HPP_
