// frost_graphics/android_surface.cpp — Android NativeActivity layer
//
// v1 (see android_surface.hpp): ANativeWindow shims backed by the
// DisplayProxy host SDL window; EGL window-surface interception in
// GraphicThunk substitutes the host native window for the shim handle.
//
// v2 (2026-08): NativeActivity lifecycle driver state, ALooper registry,
// and the AInputQueue event store. This class plays the Android
// framework role for `bifrost-emu --android libapp.so` runs:
//   - create_activity() synthesizes an ANativeActivity struct + callback
//     table in guest memory (mmap_alloc → inside the 4 GiB direct window,
//     so the guest JIT derefs them at full speed);
//   - lifecycle callbacks are GUEST function pointers read live from the
//     callback table and invoked through the borrow-CPU runner wired by
//     the Emulator (same mechanism as GLFW callback delivery);
//   - ALooper_pollAll/pollOnce report readiness of registered fds via a
//     real ::poll() on the resolved HOST fd (guest pipes are real host
//     pipes under HostNode) plus input-queue attachments;
//   - SDL mouse/touch/keyboard events become queued AInputEvent objects;
//     AInputQueue_getEvent hands out stable guest handles into a fixed
//     slot table and the AMotionEvent_*/AKeyEvent_* getters read back.
#include "frost/android_surface.hpp"

#include "core/memory.h"
#include "core/cpu.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

namespace frost {

using arm64emu::CPU;
using arm64emu::Memory;

// libwayland-egl entry points (resolved lazily on Wayland hosts).
struct WlEglFns {
    void* handle = nullptr;
    void* (*window_create)(void* surface, int width, int height) = nullptr;
    void (*window_destroy)(void* window) = nullptr;
    int (*window_resize)(void* window, int width, int height) = nullptr;
    bool ok = false;
};
static WlEglFns g_wl_egl;

static const WlEglFns& wl_egl_fns() {
    if (!g_wl_egl.handle && !g_wl_egl.ok) {
        g_wl_egl.handle = dlopen("libwayland-egl.so.1", RTLD_LAZY);
        if (!g_wl_egl.handle) g_wl_egl.handle = dlopen("libwayland-egl.so", RTLD_LAZY);
        if (g_wl_egl.handle) {
            g_wl_egl.window_create =
                reinterpret_cast<void* (*)(void*, int, int)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_create"));
            g_wl_egl.window_destroy =
                reinterpret_cast<void (*)(void*)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_destroy"));
            g_wl_egl.window_resize =
                reinterpret_cast<int (*)(void*, int, int)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_resize"));
            g_wl_egl.ok = g_wl_egl.window_create != nullptr;
        }
        if (!g_wl_egl.ok) {
            std::fprintf(stderr, "[android] libwayland-egl unavailable\n");
            g_wl_egl.handle = (void*)1;  // sentinel: tried and failed
        }
    }
    return g_wl_egl;
}

AndroidSurfaceManager& AndroidSurfaceManager::instance() {
    static AndroidSurfaceManager mgr;
    return mgr;
}

void AndroidSurfaceManager::set_runner(GuestRunner r) { runner_ = std::move(r); }
void AndroidSurfaceManager::set_fd_resolver(FdResolver r) {
    fd_resolver_ = std::move(r);
}

void AndroidSurfaceManager::set_host_sdl_window(void* sdl_window) {
    if (sdl_window_ == sdl_window) return;
    sdl_window_ = sdl_window;
    host_native_window_resolved_ = false;
    host_native_window_ = 0;
}

uint64_t AndroidSurfaceManager::from_surface() {
    if (!ready()) return 0;
    if (surface_handle_ == 0) {
        surface_handle_ = kHandleBase + 1;
        refs_ = 1;
    } else {
        ++refs_;
    }
    return surface_handle_;
}

void AndroidSurfaceManager::acquire(uint64_t handle) {
    if (is_shim(handle) && handle == surface_handle_) ++refs_;
}

void AndroidSurfaceManager::release(uint64_t handle) {
    if (is_shim(handle) && handle == surface_handle_ && refs_ > 0) --refs_;
}

int32_t AndroidSurfaceManager::set_buffers_geometry(int32_t w, int32_t h,
                                                    int32_t fmt) {
    if (w > 0) width_ = w;
    if (h > 0) height_ = h;
    if (fmt > 0) format_ = fmt;
    // Best-effort: keep the host window matching the requested geometry so
    // eglQuerySurface and the visible window agree.
    if (sdl_window_) {
        SDL_SetWindowSize(static_cast<SDL_Window*>(sdl_window_), width_,
                          height_);
    }
    // Wayland: resize the live wl_egl_window so the next frame renders at
    // the new size (X11 needs nothing — the X server tracks the window).
    if (wl_egl_window_ && wl_egl_fns().ok && wl_egl_fns().window_resize) {
        wl_egl_fns().window_resize(wl_egl_window_, width_, height_);
    }
    return 0;
}

uint64_t AndroidSurfaceManager::host_native_window() {
    if (!sdl_window_) return 0;
    if (host_native_window_resolved_) return host_native_window_;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(static_cast<SDL_Window*>(sdl_window_), &info)) {
        std::fprintf(stderr, "[android] SDL_GetWindowWMInfo failed: %s\n",
                     SDL_GetError());
        host_native_window_resolved_ = true;
        return 0;
    }
    if (info.subsystem == SDL_SYSWM_X11) {
        host_native_window_ =
            reinterpret_cast<uint64_t>(info.info.x11.window);
    } else if (info.subsystem == SDL_SYSWM_WAYLAND) {
        // Host EGL's Wayland platform consumes a wl_egl_window* (NOT the
        // raw wl_surface). Create one from the SDL window's wl_surface.
        // ALSO remember SDL's wl_display: eglGetDisplay(EGL_DEFAULT_DISPLAY)
        // must wrap THE SAME connection or Mesa rejects the surface
        // (wl_surfaces are connection-private).
        wl_display_ = info.info.wl.display;
        const WlEglFns& fns = wl_egl_fns();
        if (fns.ok && fns.window_create) {
            void* w = fns.window_create(info.info.wl.surface, width_,
                                        height_);
            wl_egl_window_ = w;
            host_native_window_ = reinterpret_cast<uint64_t>(w);
        } else {
            std::fprintf(stderr, "[android] cannot create wl_egl_window\n");
            host_native_window_ = 0;
        }
    } else {
        std::fprintf(stderr, "[android] unsupported WM subsystem %d\n",
                     static_cast<int>(info.subsystem));
        host_native_window_ = 0;
    }
    host_native_window_resolved_ = true;
    if (host_native_window_) {
        std::fprintf(stderr, "[android] host native window = 0x%llx\n",
                     static_cast<unsigned long long>(host_native_window_));
    }
    return host_native_window_;
}

// ═══════════════════════════════════════════════════════════════════
// v2: NativeActivity lifecycle
// ═══════════════════════════════════════════════════════════════════

// ANativeActivity field offsets (AArch64, NDK native_activity.h):
//   callbacks@0 vm@8 env@16 clazz@24 internalDataPath@32
//   externalDataPath@40 sdkVersion@48(+pad) instance@56 assetManager@64
//   obbPath@72 — sizeof == 80.
static constexpr uint64_t kActCallbacks = 0;
static constexpr uint64_t kActVm        = 8;
static constexpr uint64_t kActEnv       = 16;
static constexpr uint64_t kActClazz     = 24;
static constexpr uint64_t kActInternal  = 32;
static constexpr uint64_t kActExternal  = 40;
static constexpr uint64_t kActSdkVer    = 48;
static constexpr uint64_t kActInstance  = 56;
static constexpr uint64_t kActAssets    = 64;
static constexpr uint64_t kActObbPath   = 72;

// Opaque-but-stable fake JNI handles. Guests treat these as opaque; any
// attempt to call through them lands in unmapped-ish shim space and fails
// loudly instead of corrupting real guest objects.
static constexpr uint64_t kFakeVm      = 0xA90004000001ULL;
static constexpr uint64_t kFakeEnv     = 0xA90004000002ULL;
static constexpr uint64_t kFakeClazz   = 0xA90004000003ULL;
static constexpr uint64_t kFakeAssets  = 0xA90004000004ULL;

static uint64_t write_guest_cstr(Memory& mem, const std::string& s,
                                 uint64_t& cursor) {
    uint64_t addr = cursor;
    mem.write(addr, s.c_str(), s.size() + 1);
    cursor += (s.size() + 1 + 15) & ~15ULL;
    return addr;
}

uint64_t AndroidSurfaceManager::create_activity() {
    if (!mem_) return 0;
    activity_addr_ = mem_->mmap_alloc(80);
    callbacks_addr_ = mem_->mmap_alloc(16 * 8);  // zeroed (all callbacks NULL)
    rect_addr_ = mem_->mmap_alloc(16);           // ARect scratch
    strings_addr_ = mem_->mmap_alloc(256);
    uint64_t cur = strings_addr_;
    uint64_t internal = write_guest_cstr(
        *mem_, "/data/data/com.bifrost.emulator/files", cur);
    uint64_t external = write_guest_cstr(
        *mem_, "/storage/emulated/0/Android/data/com.bifrost.emulator/files",
        cur);
    uint64_t obb = write_guest_cstr(*mem_, "", cur);

    auto put = [&](uint64_t off, uint64_t v) {
        mem_->write(activity_addr_ + off, &v, sizeof(v));
    };
    put(kActCallbacks, callbacks_addr_);
    put(kActVm, kFakeVm);
    put(kActEnv, kFakeEnv);
    put(kActClazz, kFakeClazz);
    put(kActInternal, internal);
    put(kActExternal, external);
    uint32_t sdk = 34;  // Android 14 — modern enough for every game
    mem_->write(activity_addr_ + kActSdkVer, &sdk, sizeof(sdk));
    put(kActInstance, 0);
    put(kActAssets, kFakeAssets);
    put(kActObbPath, obb);
    return activity_addr_;
}

uint64_t AndroidSurfaceManager::fire_activity_cb(CPU& cpu, ActivityCb cb,
                                                 const int64_t* extra,
                                                 size_t n_extra) {
    GuestRunner r;
    uint64_t cbs = callbacks_addr_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        r = runner_;  // copy under lock; invoke outside
    }
    if (!mem_ || !cbs || !r) return 0;
    // Slot number per the guest's compile-time layout.
    const bool legacy = legacy_callbacks();
    int slot = legacy ? legacy_cb_slot(cb)
                      : static_cast<int>(cb);
    if (slot < 0 || slot >= CB_COUNT) return 0;  // not present pre-API-26
    uint64_t fn = 0;
    mem_->read(cbs + static_cast<uint64_t>(slot) * 8, &fn, sizeof(fn));
    if (!fn) return 0;
    int64_t args[4];
    args[0] = static_cast<int64_t>(activity_addr_);
    size_t n_args = 1;
    for (size_t i = 0; i < n_extra && n_args < 4; i++)
        args[n_args++] = extra[i];
    return r(cpu, fn, args, n_args);
}

// ═══════════════════════════════════════════════════════════════════
// v2: ALooper registry
// ═══════════════════════════════════════════════════════════════════
// ALooper ident/poll codes (android/looper.h).
static constexpr int LOOPER_POLL_WAKE    = -1;
static constexpr int LOOPER_POLL_CALLBACK = -2;
static constexpr int LOOPER_POLL_TIMEOUT = -3;
static constexpr int LOOPER_POLL_ERROR   = -4;
static constexpr int LOOPER_EVENT_INPUT  = 1 << 0;

int AndroidSurfaceManager::looper_add_fd(uint64_t looper, int fd, int ident,
                                         int events, uint64_t callback,
                                         void* data) {
    if (!is_looper(looper)) return -1;
    if (fd < 0) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : looper_fds_) {
        if (e.fd == fd) {
            // Replace the registration (matches re-add semantics close
            // enough to the real looper's EBUSY-free common case).
            e.ident = ident;
            e.events = events;
            e.callback = callback;
            e.data = data;
            return 1;
        }
    }
    looper_fds_.push_back(FdEntry{fd, ident, events, callback, data});
    return 1;
}

int AndroidSurfaceManager::looper_remove_fd(uint64_t looper, int fd) {
    if (!is_looper(looper)) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < looper_fds_.size(); i++) {
        if (looper_fds_[i].fd == fd) {
            looper_fds_.erase(looper_fds_.begin() + i);
            return 1;
        }
    }
    return 0;
}

void AndroidSurfaceManager::looper_wake(uint64_t /*looper*/) {
    std::lock_guard<std::mutex> lk(mu_);
    wake_flag_ = true;
}

int AndroidSurfaceManager::looper_poll(CPU& cpu, int timeout_ms, int* out_fd,
                                       int* out_events, void** out_data,
                                       bool dispatch_callbacks) {
    using clock = std::chrono::steady_clock;
    const clock::time_point deadline =
        timeout_ms < 0 ? clock::time_point::max()
                       : clock::now() + std::chrono::milliseconds(timeout_ms);

    GuestRunner runner_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        runner_copy = runner_;
    }

    for (;;) {
        bool wake = false;
        bool input_ready = false;
        FdEntry ready{};
        bool has_ready = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (wake_flag_) {
                wake_flag_ = false;
                wake = true;
            }
            if (!pending_.empty() && queue_attach_.ident != 0) {
                input_ready = true;
            }
        }
        if (wake) return LOOPER_POLL_WAKE;

        // ── Input queue readiness first: games want input promptly ────
        if (input_ready) {
            QueueAttachment att;
            {
                std::lock_guard<std::mutex> lk(mu_);
                att = queue_attach_;
            }
            if (att.callback && dispatch_callbacks) {
                // Callback-style attachment: fire and keep polling.
                int64_t iargs[3] = {-1, LOOPER_EVENT_INPUT,
                                    static_cast<int64_t>(
                                        reinterpret_cast<uintptr_t>(
                                            att.data))};
                if (runner_copy) runner_copy(cpu, att.callback, iargs, 3);
            } else {
                if (out_fd) *out_fd = -1;
                if (out_events) *out_events = LOOPER_EVENT_INPUT;
                if (out_data) *out_data = att.data;
                return att.ident;
            }
        }

        // ── Registered fds: real ::poll on resolved host fds ──────────
        // Snapshot registrations, poll each briefly, act outside mu_
        // (a callback may re-enter dispatch → recursive-lock hazard).
        std::vector<FdEntry> regs;
        FdResolver resolver;
        {
            std::lock_guard<std::mutex> lk(mu_);
            regs = looper_fds_;
            resolver = fd_resolver_;
        }
        for (const auto& e : regs) {
            int hfd = resolver ? resolver(e.fd) : -1;
            if (hfd < 0) continue;
            struct pollfd pfd{hfd, static_cast<short>(e.events), 0};
            if (::poll(&pfd, 1, 0) <= 0) continue;
            short got = pfd.revents &
                        (e.events | POLLERR | POLLHUP | POLLNVAL);
            if (!got) continue;
            if (e.callback) {
                if (!runner_copy) continue;
                int64_t iargs[3] = {e.fd, got,
                                    static_cast<int64_t>(
                                        reinterpret_cast<uintptr_t>(
                                            e.data))};
                runner_copy(cpu, e.callback, iargs, 3);
                if (!dispatch_callbacks) {
                    if (out_fd) *out_fd = e.fd;
                    if (out_events) *out_events = got;
                    if (out_data) *out_data = e.data;
                    return LOOPER_POLL_CALLBACK;
                }
                continue;  // pollAll keeps draining callbacks
            }
            // Non-callback mode: report ident + source pointer (the glue
            // reads the command byte from the pipe itself).
            ready = e;
            has_ready = true;
            break;
        }
        if (has_ready) {
            if (out_fd) *out_fd = ready.fd;
            if (out_events) *out_events = ready.events;
            if (out_data) *out_data = ready.data;
            return ready.ident;
        }

        // Nothing dispatched — sleep a slice or time out.
        if (clock::now() >= deadline) return LOOPER_POLL_TIMEOUT;
        SDL_Delay(2);
    }
}

// ═══════════════════════════════════════════════════════════════════
// v2: Input queue + event store
// ═══════════════════════════════════════════════════════════════════
// AINPUT_EVENT_TYPE_* / action / source constants (android/input.h).
static constexpr int32_t IE_TYPE_KEY    = 1;
static constexpr int32_t IE_TYPE_MOTION = 2;
static constexpr int32_t ACT_DOWN       = 0;
static constexpr int32_t ACT_UP         = 1;
static constexpr int32_t ACT_MOVE       = 2;
static constexpr int32_t ACT_CANCEL     = 3;
static constexpr int32_t ACT_HOVER_MOVE = 7;
static constexpr int32_t SRC_KEYBOARD    = 0x00000301;
static constexpr int32_t SRC_TOUCHSCREEN = 0x00001002;
static constexpr int32_t SRC_MOUSE       = 0x00002002;

void AndroidSurfaceManager::queue_attach_looper(uint64_t queue,
                                                uint64_t looper, int ident,
                                                uint64_t callback,
                                                void* data) {
    if (!is_queue(queue)) return;
    std::lock_guard<std::mutex> lk(mu_);
    queue_attach_ = QueueAttachment{looper, ident, callback, data};
}

void AndroidSurfaceManager::queue_detach_looper(uint64_t queue) {
    if (!is_queue(queue)) return;
    std::lock_guard<std::mutex> lk(mu_);
    queue_attach_ = QueueAttachment{};
}

int AndroidSurfaceManager::queue_get_event(uint64_t queue,
                                           uint64_t* out_event) {
    if (!is_queue(queue)) return -9;   // -EBADF
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.empty()) return -11;  // -EWOULDBLOCK
    InputEvent ev = pending_.front();
    pending_.pop_front();
    uint64_t h = alloc_event_slot_(ev);
    if (h == 0) {
        // slots full: drop newest, keep live handles intact.
        // requeue at front so a later finishEvent can make room.
        pending_.push_front(ev);
        return -12;  // -ENOMEM
    }
    *out_event = h;
    return 0;
}

int AndroidSurfaceManager::queue_pre_dispatch(uint64_t /*queue*/,
                                              uint64_t /*event*/) {
    return 0;  // no IME interception — everything goes to the app
}

void AndroidSurfaceManager::queue_finish_event(uint64_t queue,
                                               uint64_t event) {
    if (!is_queue(queue) || !is_event(event)) return;
    std::lock_guard<std::mutex> lk(mu_);
    size_t idx = static_cast<size_t>(event - kEventBase);
    if (idx < 32) slots_[idx].used = false;
}

uint64_t AndroidSurfaceManager::alloc_event_slot_(const InputEvent& ev) {
    for (size_t i = 0; i < 32; i++) {
        if (!slots_[i].used) {
            slots_[i].used = true;
            slots_[i].ev = ev;
            return kEventBase | i;
        }
    }
    // all slots busy (app never calls finishEvent): drop the new event
    // instead of overwriting a live handle. caller requeues + ENOMEM.
    return 0;
}

const AndroidSurfaceManager::InputEvent*
AndroidSurfaceManager::find_event(uint64_t handle) const {
    if (!is_event(handle)) return nullptr;
    size_t idx = static_cast<size_t>(handle - kEventBase);
    if (idx >= 32) return nullptr;
    return &slots_[idx].ev;
}

// Getter thunks: snapshot the event under the lock, then read fields.
#define ANDROID_EVENT_GETTER(name, expr, ret)                              \
    ret AndroidSurfaceManager::name(uint64_t h) {                          \
        std::lock_guard<std::mutex> lk(mu_);                               \
        const InputEvent* ev = find_event(h);                              \
        return ev ? static_cast<ret>(expr) : static_cast<ret>(0);          \
    }

ANDROID_EVENT_GETTER(event_type, ev->type, int32_t)
ANDROID_EVENT_GETTER(event_device_id, ev->device_id, int32_t)
ANDROID_EVENT_GETTER(event_source, ev->source, int32_t)
ANDROID_EVENT_GETTER(motion_action, ev->action, int32_t)
ANDROID_EVENT_GETTER(motion_pointer_count, ev->pointer_count, int32_t)
ANDROID_EVENT_GETTER(motion_down_time, ev->down_time_ms, int64_t)
ANDROID_EVENT_GETTER(motion_event_time, ev->event_time_ms, int64_t)
ANDROID_EVENT_GETTER(key_action, ev->type == IE_TYPE_KEY ? ev->action : -1,
                     int32_t)
ANDROID_EVENT_GETTER(key_code, ev->key_code, int32_t)
ANDROID_EVENT_GETTER(key_meta_state, ev->meta_state, int32_t)
ANDROID_EVENT_GETTER(key_repeat_count, ev->repeat_count, int32_t)
ANDROID_EVENT_GETTER(key_scan_code, ev->scan_code, int32_t)
ANDROID_EVENT_GETTER(key_flags, ev->flags, int32_t)
ANDROID_EVENT_GETTER(key_down_time, ev->down_time_ms, int64_t)
ANDROID_EVENT_GETTER(key_event_time, ev->event_time_ms, int64_t)
#undef ANDROID_EVENT_GETTER

#define ANDROID_EVENT_GETTER_IDX(name, expr, ret)                          \
    ret AndroidSurfaceManager::name(uint64_t h, size_t idx) {              \
        std::lock_guard<std::mutex> lk(mu_);                               \
        const InputEvent* ev = find_event(h);                              \
        if (!ev || ev->pointer_count == 0) return static_cast<ret>(0);     \
        /* Bounds-check BEFORE the signed compare: a guest-passed huge    */\
        /* size_t truncates to a negative int32 and would read OOB.       */\
        if (idx >= 8 || static_cast<int32_t>(idx) >= ev->pointer_count)    \
            idx = 0;                                                       \
        const TouchPointer& p = ev->pointers[idx];                         \
        return static_cast<ret>(expr);                                     \
    }

ANDROID_EVENT_GETTER_IDX(motion_pointer_id, p.id, int32_t)
ANDROID_EVENT_GETTER_IDX(motion_x, p.x, float)
ANDROID_EVENT_GETTER_IDX(motion_y, p.y, float)
ANDROID_EVENT_GETTER_IDX(motion_pressure, p.pressure, float)
ANDROID_EVENT_GETTER_IDX(motion_size, p.size, float)
ANDROID_EVENT_GETTER_IDX(motion_touch_major, p.size * 48.0f, float)
ANDROID_EVENT_GETTER_IDX(motion_touch_minor, p.size * 36.0f, float)
#undef ANDROID_EVENT_GETTER_IDX

float AndroidSurfaceManager::motion_axis_value(uint64_t h, int axis,
                                               size_t idx) {
    switch (axis) {
    case 0: return motion_x(h, idx);
    case 1: return motion_y(h, idx);
    case 2: return motion_pressure(h, idx);
    case 3: return motion_size(h, idx);
    case 4: return motion_touch_major(h, idx);
    case 5: return motion_touch_minor(h, idx);
    default: return 0;
    }
}

// ═══════════════════════════════════════════════════════════════════
// v2: SDL event pump → Android input/lifecycle translation
// ═══════════════════════════════════════════════════════════════════

// SDL_Keycode → AKEYCODE_* (+ modifier meta bits). Returns 0 (UNKNOWN)
// for untranslated keys. Values from android/keycodes.h; SDL values from
// SDL_keycode.h (arrows/F-keys live above 1<<30).
int32_t AndroidSurfaceManager::sdl_key_to_android(int32_t sdl,
                                                  int32_t* meta_out) {
    if (meta_out) *meta_out = 0;
    int32_t meta_shift = 0x01;   // AMETA_SHIFT_ON
    int32_t meta_alt = 0x02;     // AMETA_ALT_ON
    int32_t meta_ctrl = 0x1000;  // AMETA_CTRL_ON
    int32_t kc = 0;
    if (sdl >= 'a' && sdl <= 'z') {
        kc = 29 + (sdl - 'a');           // AKEYCODE_A..Z
    } else if (sdl >= 'A' && sdl <= 'Z') {
        kc = 29 + (sdl - 'A');
        if (meta_out) *meta_out |= meta_shift;
    } else if (sdl >= '0' && sdl <= '9') {
        kc = 7 + (sdl - '0');            // AKEYCODE_0..9
    } else {
        switch (sdl) {
        case SDLK_SPACE:     kc = 62;  break;  // AKEYCODE_SPACE
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  kc = 66;  break;  // AKEYCODE_ENTER
        case SDLK_BACKSPACE: kc = 67;  break;  // AKEYCODE_DEL
        case SDLK_ESCAPE:    kc = 111; break;  // AKEYCODE_ESCAPE
        case SDLK_TAB:       kc = 61;  break;  // AKEYCODE_TAB
        case SDLK_DELETE:    kc = 112; break;  // AKEYCODE_FORWARD_DEL
        case SDLK_LEFT:      kc = 21;  break;
        case SDLK_RIGHT:     kc = 22;  break;
        case SDLK_UP:        kc = 19;  break;
        case SDLK_DOWN:      kc = 20;  break;
        case SDLK_HOME:      kc = 122; break;
        case SDLK_END:       kc = 123; break;
        case SDLK_PAGEUP:    kc = 92;  break;
        case SDLK_PAGEDOWN:  kc = 93;  break;
        case SDLK_INSERT:    kc = 124; break;
        case SDLK_LSHIFT:    kc = 59;  if (meta_out) *meta_out |= meta_shift; break;
        case SDLK_RSHIFT:    kc = 60;  if (meta_out) *meta_out |= meta_shift; break;
        case SDLK_LCTRL:     kc = 129; if (meta_out) *meta_out |= meta_ctrl; break;
        case SDLK_RCTRL:     kc = 130; if (meta_out) *meta_out |= meta_ctrl; break;
        case SDLK_LALT:      kc = 57;  if (meta_out) *meta_out |= meta_alt; break;
        case SDLK_RALT:      kc = 58;  if (meta_out) *meta_out |= meta_alt; break;
        default:
            if (sdl >= SDLK_F1 && sdl <= SDLK_F12)
                kc = 131 + (sdl - SDLK_F1);
            break;
        }
    }
    if (kc == 0) return 0;
    return kc;
}

void AndroidSurfaceManager::inject_test_tap() {
    std::lock_guard<std::mutex> lk(mu_);
    if (tap_injected_) return;
    tap_injected_ = true;
    float cx = width_ * 0.5f, cy = height_ * 0.5f;
    next_time_ms_ += 16;
    int64_t t_down = next_time_ms_;
    next_time_ms_ += 40;
    int64_t t_up = next_time_ms_;
    InputEvent down{}, up{};
    down.type = up.type = IE_TYPE_MOTION;
    down.source = up.source = SRC_TOUCHSCREEN;
    down.action = ACT_DOWN;
    up.action = ACT_UP;
    down.down_time_ms = down.event_time_ms = t_down;
    up.down_time_ms = t_down;
    up.event_time_ms = t_up;
    down.pointer_count = up.pointer_count = 1;
    down.pointers[0] = TouchPointer{cx, cy, 1.0f, 1.0f, 0};
    up.pointers[0] = down.pointers[0];
    pending_.push_back(down);
    pending_.push_back(up);
}

void AndroidSurfaceManager::translate_sdl_event_(CPU* /*cpu*/,
                                                 void* sdl_event_raw) {
    SDL_Event* e = static_cast<SDL_Event*>(sdl_event_raw);
    InputEvent ev{};
    switch (e->type) {
    case SDL_QUIT:
        request_quit();
        return;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_CLOSE) {
            request_quit();
        } else if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w = e->window.data1, h = e->window.data2;
            if (w > 0) width_ = w;
            if (h > 0) height_ = h;
            if (wl_egl_window_ && wl_egl_fns().ok &&
                wl_egl_fns().window_resize) {
                wl_egl_fns().window_resize(wl_egl_window_, width_, height_);
            }
            // Lifecycle notification happens in pump_host_events below.
        }
        return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.button != SDL_BUTTON_LEFT) return;  // single pointer
        ev.type = IE_TYPE_MOTION;
        ev.source = SRC_TOUCHSCREEN;
        ev.action = e->type == SDL_MOUSEBUTTONDOWN ? ACT_DOWN : ACT_UP;
        ev.pointer_count = 1;
        ev.pointers[0] = TouchPointer{static_cast<float>(e->button.x),
                                      static_cast<float>(e->button.y),
                                      1.0f, 1.0f, 0};
        break;
    }
    case SDL_MOUSEMOTION: {
        ev.type = IE_TYPE_MOTION;
        ev.source = SRC_TOUCHSCREEN;
        bool pressed = e->motion.state & SDL_BUTTON_LMASK;
        ev.action = pressed ? ACT_MOVE : ACT_HOVER_MOVE;
        ev.pointer_count = 1;
        ev.pointers[0] = TouchPointer{static_cast<float>(e->motion.x),
                                      static_cast<float>(e->motion.y),
                                      pressed ? 1.0f : 0.0f, 1.0f, 0};
        break;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION: {
        ev.type = IE_TYPE_MOTION;
        ev.source = SRC_TOUCHSCREEN;
        ev.action = e->type == SDL_FINGERDOWN  ? ACT_DOWN
                  : e->type == SDL_FINGERUP    ? ACT_UP
                                               : ACT_MOVE;
        ev.pointer_count = 1;
        // SDL finger coords are normalized [0,1].
        ev.pointers[0] = TouchPointer{
            e->tfinger.x * static_cast<float>(width_),
            e->tfinger.y * static_cast<float>(height_),
            e->tfinger.pressure, 1.0f,
            static_cast<int>(e->tfinger.fingerId & 0x7fffffff)};
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->type == SDL_KEYDOWN && e->key.repeat) return;  // no auto-repeat downs
        int32_t meta = 0;
        int32_t kc = sdl_key_to_android(static_cast<int32_t>(e->key.keysym.sym),
                                        &meta);
        if (kc == 0) return;  // untranslated key — drop silently
        ev.type = IE_TYPE_KEY;
        ev.source = SRC_KEYBOARD;
        ev.action = e->type == SDL_KEYDOWN ? ACT_DOWN : ACT_UP;
        ev.key_code = kc;
        ev.scan_code = e->key.keysym.scancode;
        ev.meta_state = meta;
        ev.repeat_count = 0;
        break;
    }
    default:
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.size() >= 256) pending_.pop_front();  // cap: drop oldest
    next_time_ms_ += 16;
    ev.down_time_ms = next_time_ms_;
    ev.event_time_ms = next_time_ms_;
    pending_.push_back(ev);
}

void AndroidSurfaceManager::pump_host_events(CPU* cpu) {
    // Track size changes so the driver can fire onNativeWindowResized /
    // onContentRectChanged exactly once per resize.
    SDL_Event e;
    while (SDL_PollEvent(&e)) translate_sdl_event_(cpu, &e);
    if (!cpu || !mem_ || surface_handle_ == 0) return;
    if (last_pump_w_ < 0) { last_pump_w_ = width_; last_pump_h_ = height_; return; }
    if (width_ != last_pump_w_ || height_ != last_pump_h_) {
        last_pump_w_ = width_;
        last_pump_h_ = height_;
        int32_t rect[4] = {0, 0, width_, height_};
        mem_->write(rect_addr_, rect, sizeof(rect));
        int64_t win = static_cast<int64_t>(surface_handle_);
        int64_t rct = static_cast<int64_t>(rect_addr_);
        const int64_t wa[] = {win};
        const int64_t ra[] = {win, rct};
        fire_activity_cb(*cpu, CB_ON_NATIVE_WINDOW_RESIZED, wa, 1);
        fire_activity_cb(*cpu, CB_ON_CONTENT_RECT_CHANGED, ra, 2);
    }
}

}  // namespace frost
