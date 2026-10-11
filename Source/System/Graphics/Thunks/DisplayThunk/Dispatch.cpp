// DisplayThunk::dispatch — the SVC entry for every display-API symbol.
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {

int64_t DisplayThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if ((symbol_id & DisplayThunk::ID_MASK) != DisplayThunk::ID_BASE) {
        return -ENOENT;
    }
    uint32_t local_id = symbol_id - DisplayThunk::ID_BASE;
    if (local_id >= impl_->id_to_idx_.size()) {
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    bool trace = dbg().thunk_trace;

    // ── Proxy dispatch: route X11/Wayland calls to DisplayProxy ──────
    // When the THUNK_PROXY flag is set, the symbol is handled by the
    // DisplayProxy (SDL2-based software fallback). The proxy is preferred
    // over the host library: its handles (Display*, Window, GC) are guest
    // addresses, so they round-trip through guest memory correctly. The
    // host-lib path returns a HOST Display* which cannot be translated
    // back through guest memory — marking it as a pointer arg bounces it
    // and crashes. The host library is only a fallback when no SDL proxy
    // can be initialized (headless host).
    if ((entry.flags & THUNK_PROXY) && !proxy_symbol_needs_sdl(entry.name)) {
        // Wayland bridge symbols do not need an SDL window. In particular,
        // don't create an extra SDL window before wl_display_connect; the
        // proxy forwards these calls to the host compositor directly.
        return proxy_dispatch_(cpu, entry.name);
    }
    if (entry.flags & THUNK_PROXY) {
        // Serialize lazy init under mu: two guest threads racing here both
        // saw !ready() and double-created the SDL window (leak + torn
        // window_). ready() is re-checked inside the lock; proxy_dispatch_
        // below takes the same lock around its own init.
        bool proxy_ok = false;
        {
            std::lock_guard<std::mutex> g(impl_->mu);
            if (impl_->proxy_) {
                if (!impl_->proxy_->ready()) {
                    impl_->proxy_->init(640, 480, impl_->mem);
                }
                proxy_ok = impl_->proxy_->ready();
            }
        }
        if (proxy_ok) {
            return proxy_dispatch_(cpu, entry.name);
        }
        if (!entry.host_fn) {
            // No host function and no proxy — return 0 (NULL).
            cpu.regs[0] = 0;
            return 0;
        }
        // Proxy unavailable (headless) — fall through to host dispatch.
    }

    // ── Android NativeActivity surface layer (ANDROID_WINDOW) ────────
    // v1: ANativeWindow shims backed by the DisplayProxy host SDL window
    // (the window argument is a GUEST shim handle — raw integer, never
    // translated). fromSurface lazily initializes the proxy so a pure
    // Android app still gets a host window.
    // v2 (2026-08): the framework plumbing
    // (ALooper/AInputQueue/event getters/AConfiguration/liblog stubs) is
    // dispatched here too and must NOT require the proxy — glue code
    // calls AConfiguration_* before any window exists.
    if (entry.flags & THUNK_ANDROID_WINDOW) {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (impl_->mem) mgr.set_memory(impl_->mem);
        const std::string& n = entry.name;

        auto set_f32 = [&](float f) {
            std::memcpy(&cpu.v_lo[0], &f, sizeof(float));
            cpu.v_hi[0] = 0;
            cpu.regs[0] = 0;
        };
        static constexpr uint64_t kFakeConfig = 0xA90004000005ULL;
        // Guest NUL-string reader (up to 1 KiB; tolerant of unmapped).
        auto guest_str = [&](uint64_t addr) -> std::string {
            if (!addr || !impl_->mem) return {};
            std::string s;
            char tmp[64];
            for (size_t off = 0; off < 1024;) {
                size_t want = std::min(sizeof(tmp), size_t(1024 - off));
                try {
                    impl_->mem->read(addr + off, tmp, want);
                } catch (...) {
                    break;
                }
                size_t i = 0;
                for (; i < want; i++) {
                    if (tmp[i] == '\0') {
                        s.append(tmp, i);
                        return s;
                    }
                }
                s.append(tmp, want);
                off += want;
            }
            return s;
        };

        // ── Framework plumbing: no host window required ───────────────
        if (n.rfind("ALooper_", 0) == 0) {
            if (n == "ALooper_prepare") {
                cpu.regs[0] = mgr.looper_prepare(
                    cpu.tid, static_cast<int>(cpu.regs[0]));
            } else if (n == "ALooper_forThread") {
                cpu.regs[0] = mgr.looper_for_thread(cpu.tid);
            } else if (n == "ALooper_acquire" || n == "ALooper_release") {
                cpu.regs[0] = 0;
            } else if (n == "ALooper_pollOnce" || n == "ALooper_pollAll") {
                int timeout = static_cast<int>(static_cast<int32_t>(cpu.regs[0]));
                int ofd = -1, oev = 0;
                void* odata = nullptr;
                // pollAll keeps waiting after callbacks; pollOnce reports
                // POLL_CALLBACK (-2) once any callback ran.
                int ident = mgr.looper_poll(cpu, timeout, &ofd, &oev, &odata,
                                            n == "ALooper_pollAll");
                if (cpu.regs[1] && impl_->mem) {
                    int32_t v = ofd;
                    impl_->mem->write(cpu.regs[1], &v, sizeof(v));
                }
                if (cpu.regs[2] && impl_->mem) {
                    int32_t v = oev;
                    impl_->mem->write(cpu.regs[2], &v, sizeof(v));
                }
                if (cpu.regs[3] && impl_->mem) {
                    uint64_t v = reinterpret_cast<uintptr_t>(odata);
                    impl_->mem->write(cpu.regs[3], &v, sizeof(v));
                }
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(ident));
            } else if (n == "ALooper_addFd") {
                cpu.regs[0] = static_cast<uint64_t>(mgr.looper_add_fd(
                    cpu.regs[0], static_cast<int>(cpu.regs[1]),
                    static_cast<int>(cpu.regs[2]),
                    static_cast<int>(cpu.regs[3]), cpu.regs[4],
                    reinterpret_cast<void*>(cpu.regs[5])));
            } else if (n == "ALooper_removeFd") {
                cpu.regs[0] = static_cast<uint64_t>(mgr.looper_remove_fd(
                    cpu.regs[0], static_cast<int>(cpu.regs[1])));
            } else if (n == "ALooper_wake") {
                mgr.looper_wake(cpu.regs[0]);
                cpu.regs[0] = 0;
            } else {
                cpu.regs[0] = static_cast<uint64_t>(-22);
            }
            return 0;
        }
        if (n.rfind("AChoreographer_", 0) == 0) {
            // AOSP AChoreographer.cpp: the 32- and 64-bit frame callbacks
            // share one queue; both receive (frameTimeNanos, data).
            if (n == "AChoreographer_getInstance") {
                cpu.regs[0] = mgr.choreographer_for_thread(cpu.tid);
            } else {
                int64_t delay_ns = 0;
                if (n == "AChoreographer_postFrameCallbackDelayed")
                    delay_ns = static_cast<int64_t>(cpu.regs[3]) * 1000000LL;
                else if (n == "AChoreographer_postFrameCallbackDelayed64")
                    delay_ns = static_cast<int64_t>(
                        static_cast<uint32_t>(cpu.regs[3])) * 1000000LL;
                mgr.choreographer_post(cpu.regs[0], cpu.regs[1], cpu.regs[2],
                                       delay_ns);
                cpu.regs[0] = 0;
            }
            return 0;
        }
        if (n.rfind("ASensor", 0) == 0) {
            // A device with no sensors, following AOSP sensor.cpp: no
            // ASensor exists, so every sensor argument takes the NULL-sensor
            // path. BAD_VALUE is -EINVAL. An empty queue matches BitTube:
            // getEvents reads nothing (0) and hasEvents' poll sees
            // revents != POLLIN (-1).
            constexpr int64_t kBadValue = -22;
            const uint64_t a0 = cpu.regs[0];
            int64_t r = 0;
            if (n == "ASensorManager_getInstance" ||
                n == "ASensorManager_getInstanceForPackage") {
                r = static_cast<int64_t>(frost::AndroidSurfaceManager::kSensorManager);
            } else if (n == "ASensorManager_getSensorList" ||
                       n == "ASensorManager_getDynamicSensorList") {
                if (a0 != frost::AndroidSurfaceManager::kSensorManager) {
                    r = kBadValue;
                } else {
                    if (cpu.regs[1] && impl_->mem) {
                        uint64_t list = mgr.sensor_empty_list();
                        impl_->mem->write(cpu.regs[1], &list, sizeof(list));
                    }
                    r = 0;
                }
            } else if (n == "ASensorManager_getDefaultSensor" ||
                       n == "ASensorManager_getDefaultSensorEx") {
                r = 0;
            } else if (n == "ASensorManager_createEventQueue") {
                r = a0 == frost::AndroidSurfaceManager::kSensorManager
                        ? static_cast<int64_t>(mgr.sensor_create_queue(cpu.regs[1]))
                        : 0;
            } else if (n == "ASensorManager_destroyEventQueue") {
                r = a0 == frost::AndroidSurfaceManager::kSensorManager &&
                            mgr.sensor_destroy_queue(cpu.regs[1])
                        ? 0 : kBadValue;
            } else if (n == "ASensorEventQueue_hasEvents") {
                r = mgr.sensor_is_queue(a0) ? -1 : kBadValue;
            } else if (n == "ASensorEventQueue_getEvents") {
                r = mgr.sensor_is_queue(a0) && cpu.regs[1] ? 0 : kBadValue;
            } else if (n == "ASensorEventQueue_requestAdditionalInfoEvents") {
                r = mgr.sensor_is_queue(a0) ? 0 : kBadValue;
            } else if (n.rfind("ASensorEventQueue_", 0) == 0) {
                // enable/disable/setEventRate/registerSensor need a sensor.
                r = kBadValue;
            } else if (n == "ASensor_getResolution") {
                set_f32(std::numeric_limits<float>::quiet_NaN());
                return 0;
            } else if (n == "ASensor_getMinDelay") {
                r = INT32_MIN;
            } else if (n == "ASensor_getName" || n == "ASensor_getVendor" ||
                       n == "ASensor_getStringType" ||
                       n == "ASensor_isWakeUpSensor") {
                r = 0;
            } else {
                // getType/getFifo*/getReportingMode/getHandle: *_INVALID (-1).
                r = -1;
            }
            cpu.regs[0] = static_cast<uint64_t>(r);
            return 0;
        }
        if (n.rfind("AInputQueue_", 0) == 0) {
            if (n == "AInputQueue_attachLooper") {
                mgr.queue_attach_looper(cpu.regs[0], cpu.regs[1],
                                        static_cast<int>(cpu.regs[2]),
                                        cpu.regs[3],
                                        reinterpret_cast<void*>(cpu.regs[4]));
                cpu.regs[0] = 0;
            } else if (n == "AInputQueue_detachLooper") {
                mgr.queue_detach_looper(cpu.regs[0]);
                cpu.regs[0] = 0;
            } else if (n == "AInputQueue_getEvent") {
                uint64_t evh = 0;
                int rc = mgr.queue_get_event(cpu.regs[0], &evh);
                if (rc == 0 && cpu.regs[1] && impl_->mem) {
                    impl_->mem->write(cpu.regs[1], &evh, sizeof(evh));
                }
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(rc));
            } else if (n == "AInputQueue_preDispatchEvent") {
                cpu.regs[0] = static_cast<uint64_t>(
                    mgr.queue_pre_dispatch(cpu.regs[0], cpu.regs[1]));
            } else if (n == "AInputQueue_finishEvent") {
                mgr.queue_finish_event(cpu.regs[0], cpu.regs[1]);
                cpu.regs[0] = 0;
            } else {
                cpu.regs[0] = static_cast<uint64_t>(-22);
            }
            return 0;
        }
        if (n.rfind("AInputEvent_", 0) == 0) {
            if (n == "AInputEvent_getType")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_type(cpu.regs[0]));
            else if (n == "AInputEvent_getDeviceId")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_device_id(cpu.regs[0]));
            else if (n == "AInputEvent_getSource")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_source(cpu.regs[0]));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AMotionEvent_", 0) == 0) {
            if (n == "AMotionEvent_getAction")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_action(cpu.regs[0]));
            else if (n == "AMotionEvent_getPointerCount")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_pointer_count(cpu.regs[0]));
            else if (n == "AMotionEvent_getDownTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.motion_down_time(cpu.regs[0])));
            else if (n == "AMotionEvent_getEventTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.motion_event_time(cpu.regs[0])));
            else if (n == "AMotionEvent_getEdgeFlags")
                cpu.regs[0] = 0;
            else if (n == "AMotionEvent_getXPrecision" || n == "AMotionEvent_getYPrecision")
                set_f32(1.0f);
            else if (n == "AMotionEvent_getHistorySize")
                cpu.regs[0] = 0;
            else if (n == "AMotionEvent_getPointerId")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_pointer_id(
                    cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getX")
                set_f32(mgr.motion_x(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getY")
                set_f32(mgr.motion_y(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getRawX")
                set_f32(mgr.motion_x(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getRawY")
                set_f32(mgr.motion_y(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getPressure")
                set_f32(mgr.motion_pressure(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getSize")
                set_f32(mgr.motion_size(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getTouchMajor")
                set_f32(mgr.motion_touch_major(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getTouchMinor")
                set_f32(mgr.motion_touch_minor(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getAxisValue")
                set_f32(mgr.motion_axis_value(cpu.regs[0],
                                              static_cast<int>(cpu.regs[1]),
                                              static_cast<size_t>(cpu.regs[2])));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AKeyEvent_", 0) == 0) {
            if (n == "AKeyEvent_getAction")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_action(cpu.regs[0]));
            else if (n == "AKeyEvent_getKeyCode")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_code(cpu.regs[0]));
            else if (n == "AKeyEvent_getMetaState")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_meta_state(cpu.regs[0]));
            else if (n == "AKeyEvent_getRepeatCount")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_repeat_count(cpu.regs[0]));
            else if (n == "AKeyEvent_getScanCode")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_scan_code(cpu.regs[0]));
            else if (n == "AKeyEvent_getFlags")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_flags(cpu.regs[0]));
            else if (n == "AKeyEvent_getDownTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.key_down_time(cpu.regs[0])));
            else if (n == "AKeyEvent_getEventTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.key_event_time(cpu.regs[0])));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AConfiguration_", 0) == 0) {
            if (n == "AConfiguration_new") {
                cpu.regs[0] = kFakeConfig;
            } else if (n == "AConfiguration_getLanguage") {
                if (impl_->mem && cpu.regs[1]) {
                    const char s[] = "en";
                    impl_->mem->write(cpu.regs[1], s, sizeof(s));
                }
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getCountry") {
                if (impl_->mem && cpu.regs[1]) {
                    const char s[] = "US";
                    impl_->mem->write(cpu.regs[1], s, sizeof(s));
                }
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getDensity") {
                cpu.regs[0] = 160;
            } else if (n == "AConfiguration_getSdkVersion") {
                cpu.regs[0] = 34;
            } else if (n == "AConfiguration_getOrientation") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getTouchscreen") {
                cpu.regs[0] = 3;
            } else if (n == "AConfiguration_getKeyboard") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getNavigation") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getKeysHidden") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getNavHidden") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getScreenSize") {
                cpu.regs[0] = 2;
            } else if (n == "AConfiguration_getScreenLong") {
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getUiModeType") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getUiModeNight") {
                cpu.regs[0] = 0x10;
            } else if (n == "AConfiguration_getMcc" || n == "AConfiguration_getMnc") {
                cpu.regs[0] = 0;
            } else {
                // delete / fromAssetManager / setTo / diff — no-op
                cpu.regs[0] = 0;
            }
            return 0;
        }
        if (n.rfind("__android_log", 0) == 0) {
            std::string tag = guest_str(cpu.regs[1]);
            if (n == "__android_log_write" || n == "__android_log_buf_write") {
                std::string text = guest_str(cpu.regs[2]);
                std::fprintf(stderr, "[android-log] %s: %s\n",
                             tag.c_str(), text.c_str());
                cpu.regs[0] = 1;
                return 0;
            }
            // __android_log_print / __android_log_buf_print: varargs fmt
            std::string fmt = guest_str(cpu.regs[2]);
            uint64_t slots[12] = {0};
            size_t nslots = 0;
            for (int r = 3; r < 8 && nslots < 12; r++)
                slots[nslots++] = cpu.regs[r];
            for (size_t i = 0; i < 7 && nslots < 12; i++) {
                uint64_t v = 0;
                try { impl_->mem->read(cpu.sp + i * 8, &v, sizeof(v)); }
                catch (...) { break; }
                slots[nslots++] = v;
            }
            std::string out;
            out.reserve(512);
            size_t si = 0;
            for (size_t i = 0; i < fmt.size() && out.size() < 1024; i++) {
                char c = fmt[i];
                if (c != '%') { out.push_back(c); continue; }
                if (i + 1 >= fmt.size()) break;
                size_t j = i + 1;
                while (j < fmt.size() && (fmt[j]=='-'||fmt[j]=='+'||fmt[j]==' '||fmt[j]=='#'||(fmt[j]>='0'&&fmt[j]<='9')||fmt[j]=='.')) j++;
                std::string lm;
                while (j < fmt.size() && (fmt[j]=='l'||fmt[j]=='z'||fmt[j]=='h'||fmt[j]=='j'||fmt[j]=='t')) { lm.push_back(fmt[j]); j++; }
                if (j >= fmt.size()) break;
                char conv = fmt[j];
                i = j;
                if (conv == '%') { out.push_back('%'); continue; }
                uint64_t iv = si < nslots ? slots[si++] : 0;
                char tmp[64];
                switch (conv) {
                case 's': {
                    std::string s = guest_str(iv);
                    out += s.empty() ? std::string("(null)") : s;
                    break;
                }
                case 'd': case 'i': {
                    if (lm == "ll" || lm == "j")
                        std::snprintf(tmp, sizeof(tmp), "%lld", (long long)iv);
                    else if (lm == "l")
                        std::snprintf(tmp, sizeof(tmp), "%ld", (long)iv);
                    else if (lm == "z")
                        std::snprintf(tmp, sizeof(tmp), "%zd", (ssize_t)iv);
                    else
                        std::snprintf(tmp, sizeof(tmp), "%d", (int)(int32_t)iv);
                    out += tmp;
                    break;
                }
                case 'u': {
                    if (lm == "ll" || lm == "l")
                        std::snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)iv);
                    else
                        std::snprintf(tmp, sizeof(tmp), "%u", (unsigned)iv);
                    out += tmp;
                    break;
                }
                case 'x': case 'X': {
                    std::snprintf(tmp, sizeof(tmp), conv=='x'?"%llx":"%llX",
                                  (unsigned long long)iv);
                    out += tmp;
                    break;
                }
                case 'o':
                    std::snprintf(tmp, sizeof(tmp), "%llo", (unsigned long long)iv);
                    out += tmp;
                    break;
                case 'p':
                    std::snprintf(tmp, sizeof(tmp), "%p",
                                  reinterpret_cast<void*>(uintptr_t(iv)));
                    out += tmp;
                    break;
                case 'c': {
                    out.push_back(char(iv & 0xFF));
                    break;
                }
                case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
                    double d = 0;
                    if (conv == 'f' || conv == 'F') {
                        // Varargs double promotion: 64-bit pattern in integer slot.
                        std::memcpy(&d, &iv, sizeof(d));
                    } else {
                        std::memcpy(&d, &iv, sizeof(d));
                    }
                    std::snprintf(tmp, sizeof(tmp), "%g", d);
                    out += tmp;
                    break;
                }
                default:
                    out.push_back('%');
                    out.push_back(conv);
                    break;
                }
            }
            std::fprintf(stderr, "[android-log] %s: %s\n",
                         tag.c_str(), out.c_str());
            cpu.regs[0] = static_cast<uint64_t>(out.size());
            return 0;
        }

        // Open the backend-owned window before resolving the EGL pair.
        ensure_android_window();
        if (n == "ANativeWindow_fromSurface") {
            // The managed adapter supplies a stable identity, not a JNI pointer.
            // Returns 0 (NULL) when no host window exists,
            // matching Android's behavior without a valid surface.
            cpu.regs[0] = cpu.regs[1] ? mgr.from_surface(cpu.regs[1]) : 0;
            if (trace) fprintf(stderr, "[android] fromSurface -> 0x%llx\n",
                               static_cast<unsigned long long>(cpu.regs[0]));
            return 0;
        }
        if (!frost::AndroidSurfaceManager::is_shim(cpu.regs[0])) {
            // Unknown/foreign handle — mirror Android's EINVAL-ish 0xBAD.../
            // -EINVAL convention loosely: return -EINVAL.
            cpu.regs[0] = static_cast<uint64_t>(-22);
            return 0;
        }
        if (n == "ANativeWindow_acquire") {
            mgr.acquire(cpu.regs[0]);
            cpu.regs[0] = 0;
        } else if (n == "ANativeWindow_release" || n == "ANativeWindow_free") {
            mgr.release(cpu.regs[0]);
            cpu.regs[0] = 0;
        } else if (n == "ANativeWindow_getWidth") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.width(cpu.regs[0])));
        } else if (n == "ANativeWindow_getHeight") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.height(cpu.regs[0])));
        } else if (n == "ANativeWindow_getFormat") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.format(cpu.regs[0])));
        } else if (n == "ANativeWindow_setFrameRate" ||
                   n == "ANativeWindow_setFrameRateWithChangeStrategy") {
            float rate;
            std::memcpy(&rate, &cpu.v_lo[0], sizeof(rate));
            cpu.regs[0] = static_cast<uint64_t>(mgr.set_frame_rate(cpu.regs[0], rate,
                static_cast<int8_t>(cpu.regs[1]), n == "ANativeWindow_setFrameRate" ? 0 :
                static_cast<int8_t>(cpu.regs[2])));
        } else if (n == "ANativeWindow_setBuffersGeometry") {
            // (window, width, height, format); zero restores base settings.
            cpu.regs[0] = static_cast<uint64_t>(mgr.set_buffers_geometry(
                static_cast<int32_t>(cpu.regs[1]),
                static_cast<int32_t>(cpu.regs[2]),
                static_cast<int32_t>(cpu.regs[3]), cpu.regs[0]));
        } else {
            cpu.regs[0] = 0;
        }
        return 0;
    }

    // ── Vulkan marshalling path ──────────────────────────────────────
    // vkCreateInstance/vkCreateDevice carry nested guest pointers (string
    // arrays + struct arrays) that the generic bounce can't fix, and the
    // proc-addr functions read the pName string from arg 1. Everything
    // else falls through to the generic path with the (corrected) pointer
    // masks — opaque handles pass verbatim, out pointers bounce back.
    if (entry.flags & THUNK_VULKAN) {
        if (vk_dispatch_(cpu, entry, trace)) return 0;
    }

    // ── Double-only AAPCS64 path (glOrtho, glClearDepth, …) ──────────
    if (entry.flags & DisplayThunk::THUNK_DOUBLE) {
        double dv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&dv[i], &cpu.v_lo[i], sizeof(double));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (double×%u) d0=%g d1=%g d2=%g d3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    dv[0], dv[1], dv[2], dv[3]);
        }
        switch (entry.n_float) {
        case 1: { using Fn = void (*)(double); reinterpret_cast<Fn>(entry.host_fn)(dv[0]); break; }
        case 2: { using Fn = void (*)(double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1]); break; }
        case 3: { using Fn = void (*)(double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2]); break; }
        case 4: { using Fn = void (*)(double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3]); break; }
        case 5: { using Fn = void (*)(double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4]); break; }
        case 6: { using Fn = void (*)(double, double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5]); break; }
        default: { using Fn = void (*)(double, double, double, double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5], dv[6], dv[7]); break; }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Float-only AAPCS64 path (Vulkan float params, etc.) ──────────
    if (entry.n_float > 0 && !(entry.flags & THUNK_MIXED_FP)
        && !(entry.flags & THUNK_GET_PROC)) {
        float fv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (fp×%u) f0=%g f1=%g f2=%g f3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    fv[0], fv[1], fv[2], fv[3]);
            if (entry.n_float > 4) {
                fprintf(stderr, "[display-thunk] dispatch: %s fp×%u — only first 4 floats forwarded\n",
                        entry.name.c_str(), entry.n_float);
            }
        }
        switch (entry.n_float) {
        case 1: { using Fn = void (*)(float); reinterpret_cast<Fn>(entry.host_fn)(fv[0]); break; }
        case 2: { using Fn = void (*)(float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1]); break; }
        case 3: { using Fn = void (*)(float, float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2]); break; }
        default: { using Fn = void (*)(float, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2], fv[3]); break; }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Mixed int + float (Vulkan mixed params) ──────────────────────
    if (entry.flags & THUNK_MIXED_FP) {
        uint64_t iv[4] = {0};
        float fv[4] = {0};
        uint8_t ni = entry.n_stack;
        if (ni > 4) ni = 4;
        for (uint8_t i = 0; i < ni; i++) iv[i] = cpu.regs[i];
        for (uint8_t i = 0; i < entry.n_float && i < 4; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (mixed int×%u fp×%u) i0=%lld f0=%g\n",
                    entry.name.c_str(), ni, entry.n_float,
                    static_cast<long long>(iv[0]), fv[0]);
        }
        // NOTE: the first int arg is usually a 64-bit handle
        // (VkCommandBuffer, ...), so it must stay uint64_t. an int32_t
        // cast here chops the upper 32 bits and segfaults the host
        // driver (vkCmdSetDepthBias with a truncated cmd buffer).
        if (ni == 1 && entry.n_float == 1) {
            using Fn = void (*)(uint64_t, float); reinterpret_cast<Fn>(entry.host_fn)(iv[0], fv[0]);
        } else if (ni == 1 && entry.n_float == 2) {
            using Fn = void (*)(uint64_t, float, float); reinterpret_cast<Fn>(entry.host_fn)(iv[0], fv[0], fv[1]);
        } else if (ni == 1 && entry.n_float == 3) {
            using Fn = void (*)(uint64_t, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(iv[0], fv[0], fv[1], fv[2]);
        } else if (ni == 1 && entry.n_float >= 4) {
            using Fn = void (*)(uint64_t, float, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(iv[0], fv[0], fv[1], fv[2], fv[3]);
        } else if (ni == 2 && entry.n_float == 1) {
            using Fn = void (*)(uint64_t, uint32_t, float); reinterpret_cast<Fn>(entry.host_fn)(iv[0], static_cast<uint32_t>(iv[1]), fv[0]);
        } else {
            if (trace) {
                fprintf(stderr, "[display-thunk] dispatch: %s unsupported mixed ABI (int×%u fp×%u) — call dropped\n",
                        entry.name.c_str(), ni, entry.n_float);
            }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Integer/pointer path with optional stack args ─────────────────
    // 16-arg ceiling, mirroring GraphicThunk (glCopyImageSubData takes
    // 15). Anything past the ceiling reads as 0 (silent corruption).
    constexpr int kMaxArgs = 16;
    uint64_t args[kMaxArgs] = {0};
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];
    // AAPCS64: args 8+ live on the guest stack at SP, 8-byte slots.
    if (entry.n_stack && impl_->mem) {
        for (uint8_t i = 0; i < entry.n_stack && (8 + i) < kMaxArgs; i++) {
            uint64_t slot = cpu.sp + static_cast<uint64_t>(i) * 8ull;
            impl_->mem->read(slot, &args[8 + i], sizeof(uint64_t));
        }
    }

    // ── GetProcAddress: return guest trampoline for a registered symbol ─
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        const char* name = nullptr;
        if (args[0] && impl_->mem) {
            uint8_t* hp = impl_->mem->guest_to_host_ptr(args[0]);
            if (hp) {
                name = reinterpret_cast<const char*>(hp);
            } else {
                size_t n = 0;
                for (; n + 1 < sizeof(namebuf); n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(args[0] + n, &c, 1); }
                    catch (...) { break; }
                    namebuf[n] = static_cast<char>(c);
                    if (c == 0) break;
                }
                namebuf[sizeof(namebuf) - 1] = 0;
                name = namebuf;
            }
        }
        uint64_t found = 0;
        if (name && name[0]) {
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == name) { found = e.guest_addr; break; }
                }
                if (found) break;
            }
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] GetProcAddress('%s') → 0x%llx\n",
                    name ? name : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return 0;
    }

    // ── Pointer arg translation ────────────────────────────────────────
    auto translate_ptr = [&](uint64_t& a, int idx,
                             std::vector<uint8_t>* bounce,
                             std::vector<uint8_t>* pristine,
                             uint64_t* guest_orig, bool* need_wb) {
        if (a == 0 || !impl_->mem) return;
        // Always record the original guest address: post-call hooks
        // (e.g. the vkAllocateCommandBuffers handle registry) need it,
        // and the direct branch below overwrites `a` with the host
        // address (reading that back as guest silently yields nothing).
        if (guest_orig) *guest_orig = a;
        uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(a);
        if (host_ptr) { a = reinterpret_cast<uint64_t>(host_ptr); return; }
        // High-stack / sparse-page pointer: bounce through a host buffer.
        // Default 64 KiB covers fixed-size output structs (XEvent, etc.);
        // known big-buffer functions size the bounce from their args so we
        // neither truncate the data nor write 64 KiB of garbage back over a
        // small guest object. The per-symbol SIZE column of the spec
        // overrides it (mirrors the SizeKind sizing in GraphicThunk.cpp); the
        // argument positions are implied by the symbol, so no name compares.
        size_t kBounce = 65536;
        const thunk::SizeKind sk = entry.spec ? entry.spec->size
                                              : thunk::SizeKind::NONE;
        switch (sk) {
        case thunk::SizeKind::X_DRAWSTR:
            // XDrawString/XDrawImageString(display, d, gc, x, y, string, len):
            // the string (arg 5) is sized by the length arg 6.
            if (idx == 5) {
                uint64_t sz = args[6];
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::X_SETWMPROTO:
            // XSetWMProtocols(display, w, protocols, count): the Atom array
            // (arg 2) is sized by the count arg 3 (Atom = 8 bytes).
            if (idx == 2) {
                uint64_t sz = args[3] * sizeof(unsigned long);
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        default:
            break;
        }
        bounce->resize(kBounce);
        try { impl_->mem->read(a, bounce->data(), kBounce); }
        catch (...) { bounce->assign(kBounce, 0); }
        *pristine = *bounce;   // snapshot before the host call
        *guest_orig = a;
        *need_wb = true;
        a = reinterpret_cast<uint64_t>(bounce->data());
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    std::vector<uint8_t> bounce_pristine[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};
    VkStage vk_deep_stage;   // lives until after the host call

    // ── VK_CMD_DEEP / VK_CMD_DEEP_OUT: descriptor-driven staging ────
    // Replaces listed array args with staged host pointers so the plain
    // translate_ptr pass never sees them (it would bounce ONE element
    // while the host driver walks count elements past it).
    // OUT refs (VK_CMD_DEEP_OUT): enumerations stage a 4-byte count
    // bounce + the array; after the host call min(staged, actual)
    // elements AND the actual count are copied back to guest memory.
    uint32_t vk_deep_done = 0;
    struct DeepOutRec {
        int arg = 0;
        int count_arg_idx = -1;
        uint64_t guest_array = 0;
        uint64_t guest_count_ptr = 0;   // 0 = copyback-only
        void* staged = nullptr;
        void* staged_count = nullptr;
        uint32_t staged_elems = 0;
        size_t elem_size = 0;
        bool on_success_only = false;   // OUT handles: write back only
                                        // when the host call succeeded
    };
    DeepOutRec vk_out_recs[4];
    int vk_n_out_recs = 0;
    if (entry.spec &&
        (entry.spec->policy == thunk::Policy::VK_CMD_DEEP ||
         entry.spec->policy == thunk::Policy::VK_CMD_DEEP_OUT) &&
        impl_->mem && entry.host_fn) {
        if (const thunk::VkCmdPlan* plan =
                thunk::vk_find_cmd_plan(entry.name.c_str())) {
            if (trace)
                fprintf(stderr, "[display-thunk] %s: deep plan (%u refs)\n",
                        entry.name.c_str(), plan->nrefs);
            bool ok = true;
            size_t need = 0;
            struct DeepJob { int arg = 0; uint32_t count = 0;
                             uint64_t guest = 0; bool raw = false;
                             const thunk::VkStructDesc* desc = nullptr; };
            DeepJob jobs[8] = {};
            int njobs = 0;
            for (uint8_t ri = 0; ri < plan->nrefs && ok; ri++) {
                const thunk::VkPlanRef& r = plan->refs[ri];
                if (r.arg >= kMaxArgs) continue;

                // ── NULLIFY: force this arg to nullptr at the host call
                if (r.out == 3) {
                    args[r.arg] = 0;
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }
                // ── STRUCT_IN: stage struct(s) recursively ──────────
                if (r.out == 4) {
                    if (!args[r.arg]) continue;
                    uint32_t scount = 1;
                    if (r.count_arg != 0xFF && r.count_arg < kMaxArgs)
                        scount = static_cast<uint32_t>(std::min<uint64_t>(
                            args[r.count_arg], kVkDeepMaxElems));
                    need = (need + 7u) & ~size_t(7);
                    need += vk_deep_size_one(
                        impl_->mem, args[r.arg], r.desc, scount, 0, &ok);
                    if (!ok) break;
                    if (njobs >= 8) { ok = false; break; }
                    jobs[njobs++] = {static_cast<int>(r.arg), scount,
                                     args[r.arg], false, r.desc};
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }
                // ── OUT_HANDLE(S): zeroed bounce + post-call writeback
                if (r.out == 5) {
                    DeepOutRec rec{};
                    rec.arg = r.arg;
                    rec.count_arg_idx = -1;
                    rec.guest_array = args[r.arg];
                    rec.elem_size = 8;
                    uint32_t hcount = 1;
                    if (r.count_arg == 0xFE) {
                        // count = u32 member of the guest STRUCT_IN arg
                        hcount = 0;
                        for (uint8_t rj = 0; rj < plan->nrefs; rj++) {
                            const thunk::VkPlanRef& r4 =
                                plan->refs[rj];
                            if (r4.out == 4 && args[r4.arg] &&
                                    r4.arg < kMaxArgs) {
                                uint32_t m = 0;
                                try {
                                    impl_->mem->read(
                                        args[r4.arg] + r.aux, &m, 4);
                                } catch (...) { m = 0; }
                                hcount = static_cast<uint32_t>(
                                    std::min<uint64_t>(m,
                                                       kVkDeepMaxElems));
                                break;
                            }
                        }
                    } else if (r.count_arg != 0xFF &&
                               r.count_arg < kMaxArgs) {
                        hcount = static_cast<uint32_t>(std::min<uint64_t>(
                            args[r.count_arg], kVkDeepMaxElems));
                    }
                    rec.staged_elems = hcount;
                    rec.on_success_only = true;
                    need = (need + 7u) & ~size_t(7);
                    need += (size_t)hcount * 8;
                    if (vk_n_out_recs >= 4) { ok = false; break; }
                    vk_out_recs[vk_n_out_recs++] = rec;
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }

                // ── OUT refs: enumeration / copyback-only staging ──
                if (r.out == 1 || r.out == 2) {
                    DeepOutRec rec{};
                    rec.arg = r.arg;
                    rec.count_arg_idx = r.count_arg;
                    rec.guest_array = args[r.arg];
                    size_t esz = r.desc ? r.desc->size : r.elem_size;
                    rec.elem_size = esz;
                    if (r.out == 2 && !args[r.arg]) continue;
                    if (r.out == 1) {
                        // count lives behind a guest u32* pointer
                        rec.guest_count_ptr = args[r.count_arg];
                        uint32_t cnt = 0;
                        if (rec.guest_count_ptr) {
                            try { impl_->mem->read(rec.guest_count_ptr,
                                                   &cnt, 4); }
                            catch (...) { cnt = 0; }
                        }
                        if (cnt > kVkDeepMaxElems) cnt = kVkDeepMaxElems;
                        rec.staged_elems = cnt;
                        // count bounce is allocated AFTER reserve (fill
                        // phase below) — never hand out arena pointers
                        // before the final reserve
                        need = (need + 3u) & ~size_t(3);
                        need += 4;
                        vk_deep_done |= 1u << r.count_arg;
                        if (!args[r.arg] || !cnt) {
                            if (vk_n_out_recs >= 4) { ok = false; break; }
                            vk_out_recs[vk_n_out_recs++] = rec;
                            continue;
                        }
                        need = (need + 7u) & ~size_t(7);
                        if (r.desc) {
                            need += vk_deep_size_one(
                                impl_->mem, rec.guest_array, r.desc,
                                cnt, 0, &ok);
                        } else {
                            size_t bytes =
                                (size_t)cnt * esz;
                            if (bytes > kVkDeepMaxBytes) ok = false;
                            need += bytes;
                        }
                    } else {
                        // copyback-only staging: byte counts arrive
                        // directly (pData/dataSize); element counts
                        // multiply by the element size (pResults-style).
                        // Same AAPCS64 rule as IN refs: mask element
                        // counts to u32 first, fail closed on oversize —
                        // a staged-elems < host-count mismatch would make
                        // the host write past the staged region.
                        uint64_t cnt = args[r.count_arg];
                        if (!r.count_in_bytes) {
                            const uint32_t masked =
                                static_cast<uint32_t>(cnt);
                            if (masked > kVkDeepMaxElems) {
                                vk_deep_oversize_count_once(
                                    entry.name.c_str(), r.count_arg,
                                    cnt, masked);
                                ok = false;
                                break;
                            }
                            cnt = masked;
                        }
                        size_t bytes = r.count_in_bytes
                            ? static_cast<size_t>(cnt)
                            : static_cast<size_t>(cnt) *
                                  (r.elem_size ? r.elem_size : 1);
                        if (bytes > kVkDeepMaxBytes) ok = false;
                        rec.elem_size = r.count_in_bytes
                            ? 1 : (r.elem_size ? r.elem_size : 1);
                        rec.staged_elems =
                            static_cast<uint32_t>(cnt);
                        need += bytes;
                    }
                    if (!ok) break;
                    if (njobs >= 8 || vk_n_out_recs >= 4) { ok = false; break; }
                    // raw jobs carry a BYTE count (the fill loop reads/
                    // stages exactly j.count bytes); struct jobs carry
                    // an ELEMENT count
                    jobs[njobs++] = {r.arg,
                                     r.desc ? rec.staged_elems :
                                              static_cast<uint32_t>(
                                                  (size_t)rec.staged_elems *
                                                  esz),
                                     rec.guest_array,
                                     !r.desc, r.desc};
                    vk_out_recs[vk_n_out_recs++] = rec;
                    continue;
                }

                // ── IN refs ─────────────────────────────────────────
                // CRITICAL (vkQuake RADV SIGSEGV): element counts arrive
                // in AAPCS64 registers/stack slots read as full 64-bit
                // words, but callers commit uint32_t args with 32-bit
                // stores — bits [63:32] are legal stale-frame junk
                // (vkQuake's vkCmdPipelineBarrier imageMemoryBarrierCount
                // slot carried a code address up there). Mask to u32
                // FIRST, then FAIL THE PLAN when the masked count exceeds
                // the cap. Never silently stage fewer elements than the
                // host-visible count: the driver reads uninit arena
                // leftovers as array elements and chases garbage pNext.
                // Byte counts (count_in_bytes) are genuine VkDeviceSize /
                // size_t values and keep their full width; the
                // kVkDeepMaxBytes check below fails the plan on oversize.
                uint64_t cnt_arg = args[r.count_arg];
                if (!r.count_in_bytes) {
                    const uint32_t masked =
                        static_cast<uint32_t>(cnt_arg);
                    if (masked > kVkDeepMaxElems) {
                        vk_deep_oversize_count_once(
                            entry.name.c_str(), r.count_arg, cnt_arg,
                            masked);
                        ok = false;
                        break;
                    }
                    cnt_arg = masked;
                }
                const uint32_t n = r.count_in_bytes
                    ? 1   // raw: arg IS a byte count, marshalled below
                    : static_cast<uint32_t>(cnt_arg);
                if (!args[r.arg]) continue;
                // Misaligned u64-handle array: corrupt guest pointer
                // (never legitimately odd). Degrade to an empty array
                // (count 0 + null) — the host sees a legal no-op instead
                // of faulting on garbage. See vk_deep_misaligned_once.
                if (!r.desc && !r.count_in_bytes && r.elem_size == 8 &&
                    (args[r.arg] & 7u)) {
                    vk_deep_misaligned_once(entry.name.c_str(), r.arg,
                                            args[r.arg]);
                    if (r.count_arg < kMaxArgs) args[r.count_arg] = 0;
                    args[r.arg] = 0;
                    vk_deep_done |= 1u << r.arg;
                    // Hunt mode (BIFROST_TRAP_CORRUPT=1): dump guest state
                    // and trap for the debugger instead of limping on.
                    // Catches the FIRST corruption with all threads and
                    // stacks intact (the game dies much later otherwise).
                    static const bool trap = [] {
                        const char* e = getenv("BIFROST_TRAP_CORRUPT");
                        return e && e[0] != '0';
                    }();
                    if (trap) {
                        fprintf(stderr, "[vk-deep] TRAP pc=0x%llx sp=0x%llx "
                                "x0..x7=0x%llx 0x%llx 0x%llx 0x%llx "
                                "0x%llx 0x%llx 0x%llx 0x%llx\n",
                                (unsigned long long)cpu.pc,
                                (unsigned long long)cpu.sp,
                                (unsigned long long)cpu.regs[0],
                                (unsigned long long)cpu.regs[1],
                                (unsigned long long)cpu.regs[2],
                                (unsigned long long)cpu.regs[3],
                                (unsigned long long)cpu.regs[4],
                                (unsigned long long)cpu.regs[5],
                                (unsigned long long)cpu.regs[6],
                                (unsigned long long)cpu.regs[7]);
                        fprintf(stderr, "[vk-deep] TRAP lr=0x%llx fp=0x%llx\n",
                                (unsigned long long)cpu.regs[30],
                                (unsigned long long)cpu.regs[29]);
                        // Full reg file (x8-x28): reconstructs the 0x4333a0
                        // branch inputs (w16=[x21,#56] vs w4) and Task_Worker
                        // x27 provenance (x20+x26*8) offline.
                        for (int r = 8; r < 29; r += 4) {
                            fprintf(stderr, "[vk-deep] TRAP x%d..x%d="
                                    "0x%llx 0x%llx 0x%llx 0x%llx\n",
                                    r, r + 3,
                                    (unsigned long long)cpu.regs[r],
                                    (unsigned long long)cpu.regs[r + 1],
                                    (unsigned long long)cpu.regs[r + 2],
                                    (unsigned long long)cpu.regs[r + 3]);
                        }
                        {
                            // w16 input: u32 at [x21,#56].
                            uint32_t w16 = 0;
                            try {
                                impl_->mem->read(cpu.regs[21] + 56, &w16, 4);
                            } catch (...) {}
                            fprintf(stderr, "[vk-deep] TRAP w4=0x%x w16=0x%x "
                                    "(b.ne taken=%d)\n",
                                    (uint32_t)cpu.regs[4], w16,
                                    w16 != (uint32_t)cpu.regs[4]);
                        }
                        uint64_t fp = cpu.regs[29];
                        for (int i = 0; i < 16 && fp && !(fp & 1u); i++) {
                            uint64_t ra = 0, pfp = 0;
                            try {
                                impl_->mem->read(fp, &pfp, 8);
                                impl_->mem->read(fp + 8, &ra, 8);
                            } catch (...) { break; }
                            fprintf(stderr, "  guest[%2d] fp=0x%llx ra=0x%llx\n",
                                    i, (unsigned long long)fp,
                                    (unsigned long long)ra);
                            if (pfp <= fp) break;
                            fp = pfp;
                        }
                        // Fault context for offline decode: faulting
                        // instructions, guest stack top, and the corrupt
                        // array neighborhood (args[5]=x5, count=args[4]).
                        auto hexdump = [&](const char* tag, uint64_t base,
                                           size_t len) {
                            fprintf(stderr, "  %s @0x%llx:", tag,
                                    (unsigned long long)base);
                            for (size_t k = 0; k < len; k++) {
                                uint8_t b = 0;
                                try { impl_->mem->read(base + k, &b, 1); }
                                catch (...) {
                                    fprintf(stderr, " ??");
                                    continue;
                                }
                                fprintf(stderr, " %02x", b);
                            }
                            fprintf(stderr, "\n");
                        };
                        hexdump("insn", cpu.pc - 32, 48);
                        hexdump("stack", cpu.sp, 64);
                        hexdump("array", cpu.regs[5] & ~7ULL, 32);
                        fflush(stderr);
                        raise(SIGTRAP);
                    }
                    continue;
                }
                DeepJob j{};
                j.arg = r.arg;
                j.guest = args[r.arg];
                j.desc = r.desc ? r.desc : nullptr;
                if (r.count_in_bytes || !r.desc) {
                    // raw buffer: stage verbatim in one block. Element
                    // size comes from the ref (handles 8, enums 4, ...)
                    size_t bytes = r.count_in_bytes
                        ? static_cast<size_t>(cnt_arg)
                        : static_cast<size_t>(cnt_arg) * r.elem_size;
                    if (bytes > kVkDeepMaxBytes) ok = false;
                    j.count = static_cast<uint32_t>(bytes);   // bytes
                    j.raw = true;
                    need += bytes;
                } else {
                    j.count = n;
                    need = (need + 7u) & ~size_t(7);
                    need += vk_deep_size_one(impl_->mem, j.guest,
                                             j.desc, j.count, 0, &ok);
                }
                if (njobs >= 8) { ok = false; break; }
                jobs[njobs++] = j;
            }
            if (ok && need < kVkDeepMaxBytes) {
                VkStage& st = vk_deep_stage;
                st.buf.reserve(std::max<size_t>(need + 4096, 65536));
                // count + OUT-handle bounces FIRST (allocation order
                // mirrors `need`: out-refs contributed their bytes in
                // scan order before any IN-ref bytes)
                for (int oi = 0; oi < vk_n_out_recs; oi++) {
                    DeepOutRec& rec = vk_out_recs[oi];
                    if (rec.guest_count_ptr) {
                        void* cb = st.bytes(4, 4);
                        uint32_t cnt = rec.staged_elems;
                        std::memcpy(cb, &cnt, 4);
                        rec.staged_count = cb;
                        args[rec.count_arg_idx] =
                            reinterpret_cast<uint64_t>(cb);
                    } else if (rec.guest_array && rec.staged_elems &&
                               !rec.guest_count_ptr && rec.on_success_only) {
                        // OUT handle bounce(s), zeroed; host writes the
                        // handle(s) through them
                        void* hb = st.bytes((size_t)rec.staged_elems * 8, 8);
                        std::memset(hb, 0, (size_t)rec.staged_elems * 8);
                        rec.staged = hb;
                        args[rec.arg] =
                            reinterpret_cast<uint64_t>(hb);
                    }
                }
                for (int ji = 0; ji < njobs; ji++) {
                    DeepJob& j = jobs[ji];
                    void* dst;
                    if (j.raw) {
                        dst = st.bytes(j.count, 8);
                        try { impl_->mem->read(j.guest, dst, j.count); }
                        catch (...) { std::memset(dst, 0, j.count); }
                    } else {
                        dst = st.bytes(
                            (size_t)j.count * j.desc->size, 8);
                        for (uint32_t k2 = 0; k2 < j.count; k2++)
                            vk_deep_fill_elem(
                                impl_->mem, st,
                                j.guest + (uint64_t)k2 * j.desc->size,
                                j.desc,
                                reinterpret_cast<uint8_t*>(dst) +
                                    (uint64_t)k2 * j.desc->size, 0);
                    }
                    args[j.arg] = reinterpret_cast<uint64_t>(dst);
                    vk_deep_done |= 1u << j.arg;
                    for (int oi = 0; oi < vk_n_out_recs; oi++) {
                        if (vk_out_recs[oi].arg == j.arg)
                            vk_out_recs[oi].staged = dst;
                    }
                }
            } else {
                // plan failed (garbage counts, oversized staging): fail
                // closed. Falling through to generic translate would hand
                // nested guest pointers to the host driver (SIGSEGV).
                // VK_ERROR_OUT_OF_HOST_MEMORY = -1.
                cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(-1));
                return 0;
            }
        }
    }

    // Submits execute GPU work that reads the host mappings (vkMapMemory
    // / persistent-coherent bounces) — push them before any submit
    // reaches the driver, on BOTH the deep path and the generic fallback.
    if (entry.host_fn && impl_->mem &&
        (entry.name == "vkQueueSubmit" || entry.name == "vkQueueSubmit2" ||
         entry.name == "vkQueueSubmit2KHR")) {
        impl_->vk_sync_push_all();
    }

    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if ((entry.pointer_args & (1u << i)) &&
                !(vk_deep_done & (1u << i))) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_pristine[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    if (trace) {
        fprintf(stderr, "[display-thunk] dispatch[T%lx]: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx "
                "a8=0x%llx ptrs=0x%x stack=%u\n",
                (unsigned long)pthread_self(),
                entry.name.c_str(), entry.host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                static_cast<unsigned long long>(args[8]),
                entry.pointer_args, entry.n_stack);
    }

    // ── Stale-handle fail-closed validation ──────────────────────────
    // The registry is proven accurate (audit: 1 true flag in 90s, zero
    // false positives after the direct-path guest_orig fix), so
    // enforcement is back ON: empty the bind / skip the (void) record
    // call with a loud warn instead of segfaulting the host driver.
    bool vk_skip_call = false;
    auto vk_known = [&](const std::unordered_set<uint64_t>& s, uint64_t h) {
        if (!h) return false;
        std::lock_guard<std::mutex> g(impl_->vk_handles_mu);
        return s.find(h) != s.end();
    };
    if (entry.name == "vkFreeDescriptorSets" ||
        entry.name == "vkFreeCommandBuffers") {
        // plan {3,2}: staged handle array at args[3], count at args[2].
        // (Bookkeeping only — no behavior change.)
        uint32_t n = static_cast<uint32_t>(args[2]);
        if (n > 1024) n = 1024;
        const uint64_t* arr = reinterpret_cast<const uint64_t*>(args[3]);
        if (arr && n) {
            std::lock_guard<std::mutex> g(impl_->vk_handles_mu);
            auto& s = (entry.name == "vkFreeDescriptorSets")
                          ? impl_->vk_desc_sets_
                          : impl_->vk_cmd_bufs_;
            for (uint32_t k = 0; k < n; k++) s.erase(arr[k]);
            if (trace)
                fprintf(stderr, "[vk-reg] -%u %s\n", n,
                        entry.name.c_str());
        }
    }
    if (entry.name == "vkCmdBindDescriptorSets") {
        uint32_t n = static_cast<uint32_t>(args[4]);
        const uint64_t* arr = reinterpret_cast<const uint64_t*>(args[5]);
        bool bad = (n > 0 && !arr);
        for (uint32_t k = 0; !bad && k < n && k < 1024; k++)
            bad = !vk_known(impl_->vk_desc_sets_, arr[k]);
        if (bad) {
            fprintf(stderr, "[vk-deep] %s: stale descriptor set — "
                    "binding empty\n", entry.name.c_str());
            args[4] = 0;
            args[5] = 0;
        }
    }
    if (entry.name.compare(0, 5, "vkCmd", 5) == 0) {
        // All vkCmd* return void: skipping a stale-cmd call only drops
        // one recording step (stale prior state stays bound).
        if (!vk_known(impl_->vk_cmd_bufs_, args[0])) {
            // Real driver command-buffer handles are 8-aligned host
            // pointers (RADV e.g. 0x55e2_2f2c_4280, verified by
            // BIFROST_THUNK_TRACE). An odd value is guest-heap corruption
            // (the handle got byte-shifted), NOT a missed registration —
            // report it as such so a stray "stale" doesn't misdirect the
            // hunt, and under BIFROST_TRAP_CORRUPT stop at the FIRST
            // corruption with guest pc/lr intact (the game otherwise dies
            // much later, far from the writer).
            const bool corrupt_shape = (args[0] & 7u) != 0;
            fprintf(stderr,
                    "[vk-deep] %s: %s command buffer 0x%llx "
                    "(pc=0x%llx lr=0x%llx) — skipping call\n",
                    entry.name.c_str(),
                    corrupt_shape ? "CORRUPT/unaligned" : "stale",
                    (unsigned long long)args[0],
                    (unsigned long long)cpu.pc,
                    (unsigned long long)cpu.regs[30]);
            static const bool trap_corrupt = [] {
                const char* e = getenv("BIFROST_TRAP_CORRUPT");
                return e && e[0] != '0';
            }();
            if (corrupt_shape && trap_corrupt) {
                fflush(stderr);
                raise(SIGTRAP);
            }
            vk_skip_call = true;
        }
    }
    if (vk_skip_call) {
        cpu.regs[0] = 0;
        return 0;
    }

    uint64_t ret = 0;
    if (entry.n_stack >= 7) {
        using Fn15 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn15>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10], args[11],
            args[12], args[13], args[14]);
    } else if (entry.n_stack >= 4) {
        using Fn12 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn12>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10], args[11]);
    } else if (entry.n_stack == 3) {
        using Fn11 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn11>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10]);
    } else if (entry.n_stack == 2) {
        using Fn10 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t);
        ret = reinterpret_cast<Fn10>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9]);
    } else if (entry.n_stack == 1) {
        using Fn9 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t);
        ret = reinterpret_cast<Fn9>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7], args[8]);
    } else {
        using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn8>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7]);
    }

    // Completion waits pull host mappings back into the bounces so the
    // guest sees GPU readback results (vkMapMemory PCWFC discipline).
    if (ret == 0 && impl_->mem &&
        (entry.name == "vkDeviceWaitIdle" ||
         entry.name == "vkQueueWaitIdle" ||
         entry.name == "vkWaitForFences")) {
        impl_->vk_sync_pull_all();
    }

    // ── VK_CMD_DEEP_OUT copyback: enumerations + OUT data ────────────
    // The host wrote the actual count into our 4-byte bounce and filled
    // min(count, staged) elements of the staged array. Publish both to
    // guest memory, clamped to the enclosing live allocation so a stale
    // count can never spray neighbor heap. (VkStage outlives this.)
    // No snapshot when there are no OUT records (the common case).
    if (vk_n_out_recs > 0) {
        auto out_live = impl_->mem ? impl_->mem->allocations_snapshot()
                                   : std::vector<std::pair<uint64_t,uint64_t>>();
        for (int oi = 0; oi < vk_n_out_recs; oi++) {
            const DeepOutRec& rec = vk_out_recs[oi];
            if (rec.on_success_only && ret != 0) continue;  // OUT handles
            if (rec.staged_count && rec.guest_count_ptr) {
                uint32_t actual = 0;
                std::memcpy(&actual, rec.staged_count, 4);
                if (impl_->live_room(out_live, rec.guest_count_ptr, 4) == 4) {
                    try { impl_->mem->write(rec.guest_count_ptr, &actual, 4); }
                    catch (...) { /* unmapped count pointer */ }
                }
            }
            if (rec.staged && rec.guest_array && rec.staged_elems) {
                size_t bytes = (size_t)rec.staged_elems * rec.elem_size;
                size_t have = impl_->live_room(out_live, rec.guest_array, bytes);
                if (!have) continue;
                try { impl_->mem->write(rec.guest_array, rec.staged, have); }
                catch (...) { /* unmapped array */ }
            // Trace-gated OUT writeback log: shows what the guest actually
            // receives (e.g. descriptor-set handles). A zero/stale handle
            // here crashes the driver at first USE, far from this line.
            if (trace && rec.on_success_only && rec.elem_size == 8 &&
                rec.staged_elems <= 16) {
                uint64_t first = 0;
                std::memcpy(&first, rec.staged, 8);
                fprintf(stderr, "[display-thunk] %s: wrote back %u handle(s) "
                        "to guest 0x%llx (first=0x%llx)\n",
                        entry.name.c_str(), rec.staged_elems,
                        static_cast<unsigned long long>(rec.guest_array),
                        static_cast<unsigned long long>(first));
            }
            // Handle registry: every guest-visible descriptor set /
            // command buffer is born here. Recorded for fail-closed
            // use-site validation.
            if (rec.on_success_only && rec.elem_size == 8 &&
                (entry.name == "vkAllocateDescriptorSets" ||
                 entry.name == "vkAllocateCommandBuffers")) {
                std::lock_guard<std::mutex> g(impl_->vk_handles_mu);
                const uint64_t* hs =
                    reinterpret_cast<const uint64_t*>(rec.staged);
                auto& s = (entry.name == "vkAllocateDescriptorSets")
                              ? impl_->vk_desc_sets_
                              : impl_->vk_cmd_bufs_;
                for (uint32_t k = 0; k < rec.staged_elems; k++)
                    if (hs[k]) s.insert(hs[k]);
                if (trace && rec.staged_elems) {
                    fprintf(stderr, "[vk-reg] +%u %s (first=0x%llx)\n",
                            rec.staged_elems,
                            entry.name.c_str(),
                            (unsigned long long)hs[0]);
                }
            }
        }
    }}

    // Write bounced pointer args back into guest memory. Only the byte
    // range the host actually modified is written back — writing the full
    // bounce (64 KiB by default) over a small guest object (e.g. a stack
    // XEvent or XColor) clobbered adjacent
    // guest memory. Diffing against the pre-call snapshot also makes
    // input-only pointers (host never writes them) a no-op writeback.
    // A wrong pointer mask (e.g. an XID marked as a pointer) can bounce an
    // unmapped guest address; Memory::write throws UnmappedMemory there, so
    // guard the writeback or the exception escapes dispatch into the
    // syscall handler.
    // bounced diff writeback with live-range clamp (mirror of the gl
    // safe_writeback_all heap-shredder fix in GraphicThunk.cpp): only bytes inside
    // a tracked live allocation go back, clamped to its end. without this
    // a 64k default bounce over a small out object sprays neighbor heap.
    // Snapshot only when some arg actually bounced (window aliases need
    // no writeback, so the map copy + lock is pure overhead otherwise).
    if (impl_->mem) {
        bool any_bounced = false;
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) { any_bounced = true; break; }
        }
        if (any_bounced) {
        auto live = impl_->mem->allocations_snapshot();
        static int dwb_skip_diag = 4, dwb_clamp_diag = 4;
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) {
                const auto& after = bounce_bufs[i];
                const auto& before = bounce_pristine[i];
                if (after.size() != before.size()) continue;
                size_t first = after.size(), last = 0;
                for (size_t j = 0; j < after.size(); j++) {
                    if (after[j] != before[j]) {
                        if (first > j) first = j;
                        last = j + 1;
                    }
                }
                if (last <= first) continue;  // host didn't touch it
                const uint64_t g = bounce_guest[i];
                size_t room = impl_->live_room(live, g, last);
                if (first >= room) {
                    if (dwb_skip_diag > 0) {
                        --dwb_skip_diag;
                        fprintf(stderr,
                                "[display-thunk] wb-skip %s arg%d @0x%llx: target "
                                "no-safe-room, %zu b dropped\n",
                                entry.name.c_str(), i,
                                (unsigned long long)g, last - first);
                    }
                    continue;
                }
                if (last > room) {
                    if (dwb_clamp_diag > 0) {
                        --dwb_clamp_diag;
                        fprintf(stderr,
                                "[display-thunk] wb-clamp %s arg%d @0x%llx: %zu -> %llu b "
                                "(live end)\n",
                                entry.name.c_str(), i,
                                (unsigned long long)g, last - first,
                                (unsigned long long)(room - first));
                    }
                    last = room;
                }
                try {
                    impl_->mem->write(g + first,
                                      after.data() + first, last - first);
                } catch (...) {
                    // best-effort: host call already happened, don't break flow.
                }
            }
        }
        }
    }

    if (entry.flags & THUNK_RET_STRING) {
        // Cache the host string into guest memory and return the guest address.
        if (ret) {
            ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
        }
    }

    // Trace-gated VkResult logging for object creation/allocation: a failed
    // create (non-zero) hands the guest a garbage handle that segfaults the
    // driver at first USE (bind/draw), far from the real break. Logging the
    // result at the source names the culprit immediately.
    if (trace && ret != 0 &&
        (entry.name.compare(0, 8, "vkCreate") == 0 ||
         entry.name.compare(0, 10, "vkAllocate") == 0)) {
        fprintf(stderr, "[display-thunk] %s → %d\n",
                entry.name.c_str(), static_cast<int32_t>(ret));
    }

    // Handle registry (generic path): vkAllocateCommandBuffers is VULKAN
    // policy so it never enters the deep block; record post-writeback so
    // the registry sees exactly what the guest sees. Count is
    // commandBufferCount (u32 at +28 in VkCommandBufferAllocateInfo; +24
    // is the level enum — reading that records nothing). The OUT array is
    // args[2] (pCommandBuffers): this call takes only (device, info,
    // array) — pool-allocated objects have no pAllocator. The array may
    // have bounced (bounce_guest holds the original guest address).
    if (ret == 0 && entry.name == "vkAllocateCommandBuffers" && impl_->mem) {
        uint64_t g_info = bounce_guest[1] ? bounce_guest[1] : args[1];
        uint64_t g_arr = bounce_guest[2] ? bounce_guest[2] : args[2];
        uint32_t n = 0;
        try { impl_->mem->read(g_info + 28, &n, 4); } catch (...) { n = 0; }
        if (n > 1024) n = 1024;
        if (g_arr && n) {
            std::lock_guard<std::mutex> g(impl_->vk_handles_mu);
            for (uint32_t k = 0; k < n; k++) {
                uint64_t h = 0;
                try { impl_->mem->read(g_arr + (uint64_t)k * 8, &h, 8); }
                catch (...) { break; }
                if (h) impl_->vk_cmd_bufs_.insert(h);
            }
            if (trace) {
                uint64_t first = 0;
                try { impl_->mem->read(g_arr, &first, 8); } catch (...) {}
                fprintf(stderr, "[vk-reg] +%u cmds (first=0x%llx)\n", n,
                        (unsigned long long)first);
            }
        } else if (trace) {
            fprintf(stderr, "[vk-reg] alloc-cmds recorded NOTHING (n=%u)\n",
                    n);
        }
    }

    window_stats::api(entry.name,args,ret);
    cpu.regs[0] = ret;
    return 0;
}
}  // namespace arm64emu
