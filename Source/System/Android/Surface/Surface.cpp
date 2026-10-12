// Surface.cpp — Android NativeActivity layer
//
// v1 (see Surface.hpp): ANativeWindow shims backed by the
// backend-owned host window; EGL window-surface interception in
// GraphicThunk substitutes the host native window for the shim handle.
//
// v2 (2026-08): NativeActivity lifecycle driver state, ALooper registry,
// and the AInputQueue event store. This class plays the Android
// framework role for `bifrost-emu --android libapp.so` runs:
//   - create_activity() synthesizes an ANativeActivity struct + callback
//     table in guest memory (mmap_alloc, read inline by the guest JIT);
//   - lifecycle callbacks are GUEST function pointers read live from the
//     callback table and invoked through the borrow-CPU runner wired by
//     the Emulator (same mechanism as GLFW callback delivery);
//   - ALooper_pollAll/pollOnce report readiness of registered fds via a
//     real ::poll() on the resolved HOST fd (guest pipes are real host
//     pipes under HostNode) plus input-queue attachments;
//   - SDL mouse/touch/keyboard events become queued AInputEvent objects;
//     AInputQueue_getEvent hands out stable guest handles into a fixed
//     slot table and the AMotionEvent_*/AKeyEvent_* getters read back.
#include "System/Android/Surface/Surface.hpp"

#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <ctime>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <cerrno>
#include <stdexcept>
#include <poll.h>
#include <unistd.h>

namespace frost {

using arm64emu::CPU;
using arm64emu::Memory;

AndroidSurfaceManager& AndroidSurfaceManager::instance() {
    static AndroidSurfaceManager mgr;
    return mgr;
}

void AndroidSurfaceManager::set_runner(GuestRunner r) { runner_ = std::move(r); }
void AndroidSurfaceManager::set_fd_resolver(FdResolver r) {
    fd_resolver_ = std::move(r);
}

bool AndroidSurfaceManager::open_host_window() {
    if (ready()) return true;
    int error = window_backend_->open("Bifrost Android", width_, height_);
    if (error) {
        std::fprintf(stderr, "[android] window backend %s open failed (%d)\n",
                     window_backend_->name(), error);
        return false;
    }
    window_open_ = true;
    return true;
}

int AndroidSurfaceManager::set_window_identity(const std::string& title, const uint8_t* rgba,
                                               int32_t width, int32_t height) {
    if (!window_open_) return -ENODEV;
    if (!title.empty()) window_backend_->set_title(title.c_str());
    if (rgba && !window_backend_->set_icon(rgba, width, height)) return -ENOTSUP;
    return 0;
}

void AndroidSurfaceManager::close_host_window() {
    std::lock_guard<std::mutex> lock(surface_mu_);
    arm64emu::window_stats::forget(window_backend_.get());
    for (auto& [handle, surface] : surfaces_)
        if (surface.backend) surface.backend->close();
    window_backend_->close();
    window_open_ = false;
}

int AndroidSurfaceManager::handle_host_resize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return -EINVAL;
    int error = window_backend_->resize(width, height, false);
    if (error) return error;
    width_ = width;
    height_ = height;
    return 0;
}

uint64_t AndroidSurfaceManager::from_surface(uint64_t identity) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!ready()) return 0;
    for (auto& [handle, surface] : surfaces_) {
        if (surface.identity == identity) { ++surface.refs; return handle; }
    }
    if (next_surface_ >= 0xFFFF) return 0;
    Surface surface;
    surface.identity = identity;
    surface.width = width_; surface.height = height_; surface.format = format_;
    if (surface_handle_) {
        surface.backend = window_backend_->create_surface(width_, height_);
        if (!surface.backend) return 0;
    }
    uint64_t handle = kHandleBase + ++next_surface_;
    surfaces_.emplace(handle, std::move(surface));
    if (!surface_handle_) surface_handle_ = handle;
    return handle;
}

void AndroidSurfaceManager::acquire(uint64_t handle) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    auto it = surfaces_.find(handle);
    if (it != surfaces_.end()) ++it->second.refs;
}

void AndroidSurfaceManager::release(uint64_t handle) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    auto it = surfaces_.find(handle);
    if (it == surfaces_.end() || !it->second.refs) return;
    if (--it->second.refs == 0) {
        if (it->second.backend) it->second.backend->set_visible(false);
        if (handle == surface_handle_) surface_handle_ = 0;
        surfaces_.erase(it);
    }
}

void AndroidSurfaceManager::register_egl_surface(uint64_t egl_surface, uint64_t handle) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    auto it = surfaces_.find(handle);
    if (it != surfaces_.end() && egl_surfaces_.emplace(egl_surface, handle).second)
        ++it->second.refs;
}
bool AndroidSurfaceManager::present_egl_surface(uint64_t egl_surface) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!window_open_ || !egl_surfaces_.count(egl_surface)) return false;
    arm64emu::window_stats::note_present(window_backend_.get(), "Android EGL");
    return true;
}
void AndroidSurfaceManager::destroy_egl_surface(uint64_t egl_surface) {
    uint64_t handle;
    {
        std::lock_guard<std::mutex> lock(surface_mu_);
        auto it = egl_surfaces_.find(egl_surface);
        if (it == egl_surfaces_.end()) return;
        handle = it->second;
        egl_surfaces_.erase(it);
    }
    release(handle);
}

int AndroidSurfaceManager::set_surface_opaque(uint64_t handle, bool opaque) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    auto it = surfaces_.find(handle);
    if (it == surfaces_.end()) return -EINVAL;
    // Opacity is an EGL creation attribute. Changing the SurfaceView format
    // requires recreating its surface, as SurfaceView.updateSurface does.
    if (it->second.opaque != opaque)
        for (const auto& [egl, window] : egl_surfaces_)
            if (window == handle) return -EBUSY;
    it->second.opaque = opaque;
    return 0;
}

bool AndroidSurfaceManager::surface_opaque(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(surface_mu_);
    auto it = surfaces_.find(handle);
    return it != surfaces_.end() && it->second.opaque;
}

int32_t AndroidSurfaceManager::width(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!handle || handle == surface_handle_) return width_;
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? -EINVAL : it->second.width;
}
int32_t AndroidSurfaceManager::height(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!handle || handle == surface_handle_) return height_;
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? -EINVAL : it->second.height;
}
int32_t AndroidSurfaceManager::format(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!handle || handle == surface_handle_) return format_;
    auto it = surfaces_.find(handle);
    return it == surfaces_.end() ? -EINVAL : it->second.format;
}

int32_t AndroidSurfaceManager::set_buffers_geometry(int32_t w, int32_t h,
                                                    int32_t fmt, uint64_t handle) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (w < 0 || h < 0 || fmt < 0 || (w == 0) != (h == 0)) return -EINVAL;
    if (fmt && fmt != kFormatRGBA8888) return -ENOTSUP;
    int32_t width = w ? w : kBaseWidth, height = h ? h : kBaseHeight;
    if (handle && handle != surface_handle_) {
        auto it = surfaces_.find(handle);
        if (it == surfaces_.end() || !it->second.backend) return -EINVAL;
        int error = it->second.backend->resize(width, height, false);
        if (error) return error;
        it->second.width = width; it->second.height = height;
        it->second.format = fmt ? fmt : kFormatRGBA8888;
        return 0;
    }
    int error = window_backend_->resize(width, height, true);
    if (error) return error;
    width_ = width;
    height_ = height;
    format_ = fmt ? fmt : kFormatRGBA8888;
    return 0;
}

int32_t AndroidSurfaceManager::set_frame_rate(uint64_t handle, float rate,
                                             int8_t compatibility, int8_t strategy) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!surfaces_.count(handle) || !std::isfinite(rate) || rate < 0 ||
            (compatibility != 0 && compatibility != 1) || (strategy != 0 && strategy != 1))
        return -EINVAL;
    // Like Surface::setFrameRate when its destination is not SurfaceFlinger,
    // accept the hint without changing the host display's refresh rate.
    return 0;
}

uint64_t AndroidSurfaceManager::host_native_window(uint64_t handle) {
    std::lock_guard<std::mutex> lock(surface_mu_);
    if (!handle) return window_backend_->binding().window;
    auto it = surfaces_.find(handle);
    if (it == surfaces_.end()) return 0;
    return it->second.backend ? it->second.backend->binding().window : window_backend_->binding().window;
}
void* AndroidSurfaceManager::native_display() const {
    return window_backend_->binding().display;
}
AndroidSurfaceManager::AndroidSurfaceManager()
    : AndroidSurfaceManager(make_sdl_android_window_backend()) {}

AndroidSurfaceManager::AndroidSurfaceManager(std::unique_ptr<AndroidWindowBackend> backend)
    : window_backend_(std::move(backend)) {
    if (!window_backend_ || window_backend_->resize(width_, height_, false))
        throw std::runtime_error("Android window backend cannot initialize geometry");
}

AndroidSurfaceManager::~AndroidSurfaceManager() {
    close_host_window();
}

bool AndroidSurfaceManager::install_window_backend(std::unique_ptr<AndroidWindowBackend> backend) {
    if (!backend || ready() || next_surface_) return false;
    if (backend->resize(width_, height_, false)) return false;
    window_backend_ = std::move(backend);
    return true;
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
// ALooper registry: one looper per guest thread
// ═══════════════════════════════════════════════════════════════════
// Semantics follow AOSP android15-release: libutils Looper.cpp
// (prepare/addFd/pollOnce/pollAll) behind frameworks/base
// native/android/looper.cpp, which always polls the CALLING thread's
// looper (Looper::getForThread).
// ALooper ident/poll codes (android/looper.h).
static constexpr int LOOPER_POLL_WAKE    = -1;
static constexpr int LOOPER_POLL_CALLBACK = -2;
static constexpr int LOOPER_POLL_TIMEOUT = -3;
static constexpr int LOOPER_POLL_ERROR   = -4;
static constexpr int LOOPER_EVENT_INPUT  = 1 << 0;
static constexpr int LOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0;

static int64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

AndroidSurfaceManager::Looper*
AndroidSurfaceManager::find_looper_(uint64_t handle) {
    auto it = loopers_.find(handle);
    return it == loopers_.end() ? nullptr : &it->second;
}

uint64_t AndroidSurfaceManager::looper_prepare(int tid, int opts) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = thread_loopers_.find(tid);
    if (it != thread_loopers_.end()) return it->second;
    // Looper::prepare keeps the first looper's ALLOW_NON_CALLBACKS value.
    uint64_t handle = kLooperBase | (next_looper_id_++ & 0xFFFFFF);
    Looper& looper = loopers_[handle];
    looper.tid = tid;
    looper.allow_non_callbacks =
        (opts & LOOPER_PREPARE_ALLOW_NON_CALLBACKS) != 0;
    thread_loopers_[tid] = handle;
    return handle;
}

uint64_t AndroidSurfaceManager::looper_for_thread(int tid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = thread_loopers_.find(tid);
    return it == thread_loopers_.end() ? 0 : it->second;
}

int AndroidSurfaceManager::looper_add_fd(uint64_t looper, int fd, int ident,
                                         int events, uint64_t callback,
                                         void* data) {
    if (fd < 0) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    Looper* l = find_looper_(looper);
    if (!l) return -1;
    if (!callback) {
        if (!l->allow_non_callbacks || ident < 0) return -1;
    } else {
        ident = LOOPER_POLL_CALLBACK;
    }
    for (auto& e : l->fds) {
        if (e.fd == fd) {
            // Looper::addFd replaces an existing registration for the fd.
            e = FdEntry{fd, ident, events, callback, data};
            return 1;
        }
    }
    l->fds.push_back(FdEntry{fd, ident, events, callback, data});
    return 1;
}

int AndroidSurfaceManager::looper_remove_fd(uint64_t looper, int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    Looper* l = find_looper_(looper);
    if (!l) return 0;
    for (size_t i = 0; i < l->fds.size(); i++) {
        if (l->fds[i].fd == fd) {
            l->fds.erase(l->fds.begin() + i);
            return 1;
        }
    }
    return 0;
}

void AndroidSurfaceManager::looper_wake(uint64_t looper) {
    std::lock_guard<std::mutex> lk(mu_);
    if (Looper* l = find_looper_(looper)) l->wake = true;
}

int AndroidSurfaceManager::looper_poll(CPU& cpu, int timeout_ms, int* out_fd,
                                       int* out_events, void** out_data,
                                       bool poll_all) {
    const int64_t start = monotonic_ns();
    const int64_t deadline =
        timeout_ms < 0 ? INT64_MAX
                       : start + static_cast<int64_t>(timeout_ms) * 1000000LL;

    GuestRunner runner_copy;
    FdResolver resolver;
    uint64_t handle = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = thread_loopers_.find(cpu.tid);
        if (it == thread_loopers_.end()) return LOOPER_POLL_ERROR;
        handle = it->second;
        runner_copy = runner_;
        resolver = fd_resolver_;
    }

    for (;;) {
        // Group exit stops this CPU; leave instead of sleeping forever.
        if (!cpu.running) return LOOPER_POLL_ERROR;
        bool callback_ran = false;
        bool wake = false;
        bool input_ready = false;
        QueueAttachment att;
        std::vector<FdEntry> regs;
        std::vector<FrameCallback> due;
        int64_t vsync_ns = 0;
        int64_t next_frame_due = INT64_MAX;
        {
            std::lock_guard<std::mutex> lk(mu_);
            Looper* l = find_looper_(handle);
            if (!l) return LOOPER_POLL_ERROR;
            if (l->wake) {
                l->wake = false;
                wake = true;
            }
            // Choreographer::dispatchVsync runs every callback whose due
            // time precedes the vsync, passing the vsync timestamp.
            // Callbacks posted from inside a callback wait for the next one.
            if (!l->frames.empty()) {
                const int64_t now = monotonic_ns();
                vsync_ns = now - now % kVsyncPeriodNs;
                for (size_t i = 0; i < l->frames.size();) {
                    if (l->frames[i].due_ns < vsync_ns) {
                        due.push_back(l->frames[i]);
                        l->frames.erase(l->frames.begin() + i);
                    } else {
                        next_frame_due =
                            std::min(next_frame_due, l->frames[i].due_ns);
                        i++;
                    }
                }
            }
            if (!pending_.empty() && queue_attach_.ident != 0 &&
                queue_attach_.looper == handle) {
                input_ready = true;
                att = queue_attach_;
            }
            regs = l->fds;
        }
        if (wake) return LOOPER_POLL_WAKE;

        for (const auto& f : due) {
            if (!runner_copy) break;
            int64_t iargs[2] = {vsync_ns, static_cast<int64_t>(f.data)};
            runner_copy(cpu, f.fn, iargs, 2);
            callback_ran = true;
        }

        // ── Input queue readiness: games want input promptly ──────────
        if (input_ready) {
            if (att.callback) {
                int64_t iargs[3] = {-1, LOOPER_EVENT_INPUT,
                                    static_cast<int64_t>(
                                        reinterpret_cast<uintptr_t>(
                                            att.data))};
                if (runner_copy) {
                    runner_copy(cpu, att.callback, iargs, 3);
                    callback_ran = true;
                }
            } else if (!callback_ran) {
                if (out_fd) *out_fd = -1;
                if (out_events) *out_events = LOOPER_EVENT_INPUT;
                if (out_data) *out_data = att.data;
                return att.ident;
            }
        }

        // ── Registered fds: real ::poll on resolved host fds ──────────
        // Callbacks run outside mu_ (they may re-enter the looper thunks).
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
                uint64_t keep = runner_copy(cpu, e.callback, iargs, 3);
                callback_ran = true;
                if (static_cast<int32_t>(keep) == 0) {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (Looper* l = find_looper_(handle)) {
                        for (size_t i = 0; i < l->fds.size(); i++) {
                            if (l->fds[i].fd == e.fd &&
                                l->fds[i].callback == e.callback) {
                                l->fds.erase(l->fds.begin() + i);
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (callback_ran) continue;  // report the ident on the next poll
            // Non-callback mode: report ident + source pointer (the glue
            // reads the command byte from the pipe itself).
            if (out_fd) *out_fd = e.fd;
            if (out_events) *out_events = got;
            if (out_data) *out_data = e.data;
            return e.ident;
        }

        if (callback_ran && !poll_all) {
            if (out_fd) *out_fd = -1;
            if (out_events) *out_events = 0;
            if (out_data) *out_data = nullptr;
            return LOOPER_POLL_CALLBACK;
        }
        const int64_t now = monotonic_ns();
        if (now >= deadline) return LOOPER_POLL_TIMEOUT;
        // Sleep a 2 ms slice for fd readiness, waking early for the next
        // vsync that has a due callback or for the deadline.
        int64_t wake_at = std::min<int64_t>(deadline, now + 2000000LL);
        if (next_frame_due != INT64_MAX) {
            int64_t tick = next_frame_due - next_frame_due % kVsyncPeriodNs +
                           kVsyncPeriodNs;
            wake_at = std::min(wake_at, tick);
        }
        if (wake_at > now)
            std::this_thread::sleep_for(std::chrono::nanoseconds(wake_at - now));
    }
}

// ═══════════════════════════════════════════════════════════════════
// AChoreographer: frame callbacks on the owning thread's looper
// ═══════════════════════════════════════════════════════════════════
// AOSP Choreographer::getForThread (frameworks/native libs/gui) returns
// NULL on a thread without a looper and otherwise one instance per thread.
// Frame callbacks are one-shot and run inside that thread's looper poll.
uint64_t AndroidSurfaceManager::choreographer_for_thread(int tid) {
    uint64_t looper = looper_for_thread(tid);
    return looper ? kChoreographerBase | (looper & 0xFFFFFF) : 0;
}

bool AndroidSurfaceManager::choreographer_post(uint64_t choreographer,
                                               uint64_t fn, uint64_t data,
                                               int64_t delay_ns) {
    if ((choreographer & ~0xFFFFFFULL) != kChoreographerBase || !fn)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    Looper* l = find_looper_(kLooperBase | (choreographer & 0xFFFFFF));
    if (!l) return false;
    l->frames.push_back(
        FrameCallback{fn, data, monotonic_ns() + std::max<int64_t>(delay_ns, 0)});
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// ASensorManager: a device with no sensors
// ═══════════════════════════════════════════════════════════════════
uint64_t AndroidSurfaceManager::sensor_empty_list() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!sensor_list_addr_ && mem_) {
        // A non-NULL list with zero entries (plus a NULL terminator), so a
        // caller that dereferences the list pointer never faults.
        sensor_list_addr_ = mem_->mmap_alloc(4096);
    }
    return sensor_list_addr_;
}

uint64_t AndroidSurfaceManager::sensor_create_queue(uint64_t looper) {
    if (!looper) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t queue = kSensorQueueBase | (next_sensor_queue_++ & 0xFFFFFF);
    sensor_queues_[queue] = looper;
    return queue;
}

bool AndroidSurfaceManager::sensor_destroy_queue(uint64_t queue) {
    std::lock_guard<std::mutex> lk(mu_);
    return sensor_queues_.erase(queue) != 0;
}

bool AndroidSurfaceManager::sensor_is_queue(uint64_t queue) {
    std::lock_guard<std::mutex> lk(mu_);
    return sensor_queues_.count(queue) != 0;
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

void AndroidSurfaceManager::translate_window_event_(const AndroidWindowEvent& e) {
    if (e.type == BIFROST_WINDOW_CLOSE) { request_quit(); return; }
    if (e.type == BIFROST_WINDOW_RESIZE) {
        if (int error = handle_host_resize(e.width, e.height))
            std::fprintf(stderr, "[android] window backend %s resize failed (%d)\n",
                         window_backend_->name(), error);
        return;
    }
    InputEvent ev{};
    if (e.type == BIFROST_WINDOW_KEY) {
        ev.type = IE_TYPE_KEY;
        ev.source = SRC_KEYBOARD;
        ev.action = e.action;
        ev.key_code = e.key_code;
        ev.scan_code = e.scan_code;
        ev.meta_state = e.meta_state;
        ev.repeat_count = e.repeat_count;
    } else if (e.type == BIFROST_WINDOW_POINTER) {
        ev.type = IE_TYPE_MOTION;
        ev.source = SRC_TOUCHSCREEN;
        ev.action = e.action;
        ev.pointer_count = 1;
        ev.pointers[0] = TouchPointer{e.x, e.y, e.pressure, 1.0f, e.pointer_id};
    } else { return; }
    std::lock_guard<std::mutex> lk(mu_);
    if (pending_.size() >= 256) pending_.pop_front();
    if (e.type == BIFROST_WINDOW_POINTER && e.action == ACT_DOWN)
        pointer_down_time_ms_ = e.time_ms;
    ev.down_time_ms = e.type == BIFROST_WINDOW_POINTER ? pointer_down_time_ms_ : e.time_ms;
    ev.event_time_ms = e.time_ms;
    pending_.push_back(ev);
}

void AndroidSurfaceManager::pump_host_events(CPU* cpu) {
    arm64emu::window_stats::WindowInfo info;
    if (window_open_ && window_backend_->window_info(info)) {
        auto title = arm64emu::window_stats::update_native(window_backend_.get(), info);
        if (!title.empty()) window_backend_->set_title(title.c_str());
    }
    // Bound plugin work per pump, including a plugin that never reports empty.
    for (unsigned i = 0; i < 256; ++i) {
        AndroidWindowEvent event{};
        int result = window_backend_->poll(event);
        if (result <= 0) {
            if (result < 0) std::fprintf(stderr, "[android] window event poll failed (%d)\n", result);
            break;
        }
        translate_window_event_(event);
    }
    if (!cpu || !mem_ || !activity_addr_ || surface_handle_ == 0) return;
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
