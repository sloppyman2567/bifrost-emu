// SDL2 audio (queue and callback modes) on the AudioEngine.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::sdl() {

    // ══ SDL2 ════════════════════════════════════════════════════════
    // Guest SDL_AudioSpec layout (AAPCS64): freq@0 i32, format@4 u16,
    // channels@6 u8, silence@7 u8, samples@8 u16, padding@10 u16,
    // size@12 u32, pad@14, callback@16 ptr, userdata@24 ptr. Size 32.
    if (name == "SDL_OpenAudioDevice" || name == "SDL_OpenAudio") {
        const bool is_dev = (name == "SDL_OpenAudioDevice");
        const uint64_t desired  = is_dev ? R(2) : R(0);
        const uint64_t obtained = is_dev ? R(3) : R(1);
        // BIFROST_AUDIO_FAIL_OPEN=1: report "no device" (return 0) so the
        // guest disables audio through its own graceful path — used to
        // isolate the heap corruption that fires when audio is active.
        static const bool fail_open_ = getenv("BIFROST_AUDIO_FAIL_OPEN") != nullptr;
        if (fail_open_) {
            if (obtained) {
                uint8_t z[32] = {0};
                I.mem->write(obtained, z, 32);
            }
            tr(0); return 0;   // SDL_OpenAudioDevice: 0 = no device
        }
        if (!desired || !I.engine) { tr(-1); return -1; }
        const int32_t  freq = (int32_t)rd32(I, desired + 0);
        const uint16_t ftag = rd16(I, desired + 4);
        uint8_t chb = 0; I.mem->read(desired + 6, &chb, 1);
        const uint8_t ch = chb ? chb : 2;
        const uint16_t samples_per_cb = rd16(I, desired + 8);
        const uint64_t cb_fn = rd64(I, desired + 16);
        const uint64_t cb_ud = rd64(I, desired + 24);
        const uint32_t fmt = sdl_fmt_map(ftag);
        const uint8_t  sz  = fmt_size(fmt);
        // Hold pump_mu for the whole arm: the callback worker scans this map
        // concurrently, and `slot` must stay valid through engine open.
        // Lock order is always pump_mu → mem/engine locks, never reverse.
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        if (!is_dev && I.sdl_devs_.count(1)) {
            tr(-1);
            return -1;  // SDL_OpenAudio: legacy global device already open.
        }
        // SDL reserves ID 1 for the legacy SDL_OpenAudio global device.
        const uint64_t h = is_dev ? ++I.next_sdl_dev_ : 1;
        auto& slot = I.sdl_devs_[h];
        slot.fmt = fmt; slot.rate = (freq > 0) ? (uint32_t)freq : 44100;
        slot.ch = ch; slot.size = sz;
        slot.cb_fn = cb_fn; slot.cb_ud = cb_ud;
        slot.frames_per_cb = samples_per_cb ? samples_per_cb : 1024;
        slot.paused = true;  // SDL devices open paused; caller explicitly resumes.
        if (cb_fn && !getenv("BIFROST_AUDIO_NO_CB")) {
            slot.bounce_bytes = (size_t)slot.frames_per_cb * slot.ch * slot.size;
            slot.bounce = I.mem->mmap_alloc(slot.bounce_bytes);
            if (!slot.bounce) { I.sdl_devs_.erase(h); tr(-ENOMEM); return -ENOMEM; }
            // Callbacks run from the TLS-aware worker, or from dispatch
            // when the worker is explicitly disabled.
            slot.cb_scheduled = true;
            slot.next_cb_us = 0;   // due immediately; pacing takes over
        }
        // pmu still held: slot stays valid through engine open + obtained.
        slot.engine_stream = I.engine
            ? I.engine->stream_open(fmt, slot.rate, slot.ch) : 0;
        if (slot.engine_stream) I.engine->stream_pause(slot.engine_stream, true);
        if (I.engine && !slot.engine_stream) {
            if (slot.bounce) {
                I.mem->untrack_allocation(slot.bounce, slot.bounce_bytes);
                slot.bounce = 0;
            }
            I.sdl_devs_.erase(h);
            tr(-ENOMEM);
            return -ENOMEM;
        }
        if (obtained) {
            wr32(I, obtained + 0, slot.rate);
            I.mem->write(obtained + 4, &ftag, 2);
            I.mem->write(obtained + 6, &slot.ch, 1);
            uint8_t silence = (fmt == PCM_FMT_U8) ? 128 : 0;
            I.mem->write(obtained + 7, &silence, 1);
            uint16_t samples = (uint16_t)slot.frames_per_cb;
            I.mem->write(obtained + 8, &samples, 2);
            uint16_t padw = 0; I.mem->write(obtained + 10, &padw, 2);
            wr32(I, obtained + 12,
                 (uint32_t)(slot.frames_per_cb * slot.ch * slot.size));
            wr64(I, obtained + 16, cb_fn);
            wr64(I, obtained + 24, cb_ud);
        }
        const bool has_callback_stream = slot.cb_fn && slot.bounce &&
                                         slot.cb_scheduled;
        pmu.unlock();
        if (has_callback_stream) start_pump();
        I.wake_pump();
        tr(is_dev ? (int64_t)h : 0);
        return is_dev ? (int64_t)h : 0;   // SDL_OpenAudio returns 0 on success
    }
    if (name == "SDL_CloseAudioDevice") {
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(R(0));
        if (it != I.sdl_devs_.end()) {
            if (it->second.callback_active &&
                it->second.callback_host == std::this_thread::get_id()) {
                it->second.close_deferred = true;
                tr(0); return 0;
            }
            const uint64_t dev = it->first;
            I.callback_cv.wait(pmu, [&] {
                auto cur = I.sdl_devs_.find(dev);
                return cur == I.sdl_devs_.end() || !cur->second.callback_active;
            });
            it = I.sdl_devs_.find(dev);
            if (it == I.sdl_devs_.end()) { tr(0); return 0; }
            it->second.stop = true;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.sdl_devs_.erase(it);
            I.wake_pump();
        }
        tr(0); return 0;
    }
    if (name == "SDL_PauseAudioDevice" || name == "SDL_PauseAudio") {
        // "SDL_PauseAudioDevice" has 'D' at index 14 (SDL_PauseAudio
        // uses the implicit device 1). The legacy 1-arg form carries
        // pause_on in R(0); the device form in R(1).
        const bool is_dev = (name[14] == 'D');
        uint64_t dev = is_dev ? R(0) : 1;
        uint8_t pause = (uint8_t)(is_dev ? R(1) : R(0));
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(dev);
        if (it != I.sdl_devs_.end()) {
            it->second.paused = pause != 0;
            // Per-device gate in the engine mixer too (real SDL: paused
            // logical devices are skipped, others keep playing).
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, pause != 0);
            if (pause && it->second.callback_active &&
                it->second.callback_host != std::this_thread::get_id()) {
                I.callback_cv.wait(pmu, [&] {
                    auto cur = I.sdl_devs_.find(dev);
                    return cur == I.sdl_devs_.end() ||
                           !cur->second.callback_active;
                });
                it = I.sdl_devs_.find(dev);
                if (it == I.sdl_devs_.end()) { tr(0); return 0; }
            }
            if (!pause) {
                // Resume cleanly: no callback burst, drain backlog now.
                // run_due_callbacks runs guest code that may close THIS
                // device — re-find before touching the iterator again.
                it->second.next_cb_us =
                    (uint64_t)std::chrono::duration_cast<
                        std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                pmu.unlock();
                run_due_callbacks();
                pmu.lock();
                it = I.sdl_devs_.find(dev);
                if (it != I.sdl_devs_.end()) top_up_queue(it->second);
            }
            I.wake_pump();
        }
        tr(0); return 0;
    }
    if (name == "SDL_QueueAudio") {
        uint64_t dev = R(0), data = R(1);
        uint32_t len = (uint32_t)R(2);
        // Fire callbacks BEFORE the find: guest code may close devices.
        // Everything below only touches this device (no runner calls),
        // so the iterator stays valid under the lock.
        run_due_callbacks();
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(dev);
        int64_t rc = -1;
        if (it != I.sdl_devs_.end() && data && len && I.engine) {
            // Real-SDL semantics: buffer the whole request on the device
            // backlog and let top_up_queue_ feed the ring in order. No
            // samples are dropped, and GetQueuedAudioSize reports the
            // true remaining backlog (guests key their refill logic off
            // it). Defensive 256 MiB cap — real SDL is unbounded, but a
            // runaway guest shouldn't OOM the host.
            constexpr size_t kMaxPending = 256u << 20;
            auto& s = it->second;
            top_up_queue(s);
            if (s.pending.size() - s.pending_off + len <= kMaxPending) {
                std::vector<uint8_t> tmp(len);
                I.mem->read(data, tmp.data(), len);
                s.pending.insert(s.pending.end(), tmp.begin(), tmp.end());
                top_up_queue(s);
                rc = 0;
            } else {
                rc = 0;  // over cap: drop like a full device
            }
        }
        tr(rc); return rc;
    }
    if (name == "SDL_GetQueuedAudioSize") {
        uint64_t dev = R(0);
        // Same ordering as QueueAudio: callbacks first, then locked find.
        run_due_callbacks();
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(dev);
        int64_t r = 0;
        if (it != I.sdl_devs_.end()) {
            top_up_queue(it->second);
            // Honest unplayed backlog: our pending buffer PLUS whatever
            // already sits in the device's engine stream ring (converted
            // back to guest input bytes). Real SDL reports everything
            // queued-but-unplayed; under-reporting makes guests that
            // pace generation off this value over-produce.
            r = (int64_t)(it->second.pending.size() - it->second.pending_off);
            if (it->second.engine_stream && I.engine) {
                const size_t frame_sz =
                    (size_t)fmt_size(it->second.fmt) * it->second.ch;
                r += (int64_t)(I.engine->stream_queued_frames(
                                   it->second.engine_stream) * frame_sz);
            }
        }
        tr(r); return r;
    }
    if (name == "SDL_ClearQueuedAudio") {
        uint64_t dev = R(0);
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(dev);
        if (it != I.sdl_devs_.end()) {
            it->second.pending.clear();
            it->second.pending_off = 0;
            // PER-DEVICE clear (real SDL): drop only this device's
            // queued frames. The old global engine-ring wipe let one
            // device's clear destroy every other device's buffered audio.
            if (it->second.engine_stream && I.engine)
                I.engine->stream_clear(it->second.engine_stream);
        }
        tr(0); return 0;
    }
    if (name == "SDL_LockAudioDevice" || name == "SDL_UnlockAudioDevice") {
        const uint64_t dev = R(0);
        {
            std::unique_lock<std::mutex> pmu(I.pump_mu);
            auto it = I.sdl_devs_.find(dev);
            if (it != I.sdl_devs_.end()) {
                if (name == "SDL_LockAudioDevice") {
                    ++it->second.callback_lock_depth;
                    if (it->second.callback_host != std::this_thread::get_id())
                        I.callback_cv.wait(pmu, [&] {
                            auto cur = I.sdl_devs_.find(dev);
                            return cur == I.sdl_devs_.end() ||
                                   !cur->second.callback_active;
                        });
                    it = I.sdl_devs_.find(dev);
                    if (it == I.sdl_devs_.end()) {
                        tr(0); return 0;
                    }
                } else if (it->second.callback_lock_depth) {
                    --it->second.callback_lock_depth;
                }
            }
        }
        // SDL guarantees the callback cannot run between Lock and Unlock.
        // Deliver overdue work only after the outermost unlock.
        if (name == "SDL_UnlockAudioDevice") {
            I.wake_pump();
            run_due_callbacks();
        }
        tr(0); return 0;
    }
    if (name == "SDL_DequeueAudio" || name == "SDL_AudioInit" ||
        name == "SDL_AudioQuit" || name == "SDL_MixAudioFormat") {
        tr(0); return 0;
    }
    // Legacy SDL1-style global-device variants (vkQuake uses these).
    if (name == "SDL_LockAudio" || name == "SDL_UnlockAudio") {
        {
            std::unique_lock<std::mutex> pmu(I.pump_mu);
            auto it = I.sdl_devs_.find(1);
            if (it != I.sdl_devs_.end()) {
                if (name == "SDL_LockAudio") {
                    ++it->second.callback_lock_depth;
                    if (it->second.callback_host != std::this_thread::get_id())
                        I.callback_cv.wait(pmu, [&] {
                            auto cur = I.sdl_devs_.find(1);
                            return cur == I.sdl_devs_.end() ||
                                   !cur->second.callback_active;
                        });
                } else if (it->second.callback_lock_depth) {
                    --it->second.callback_lock_depth;
                }
            }
        }
        if (name == "SDL_UnlockAudio") {
            I.wake_pump();
            run_due_callbacks();
        }
        tr(0); return 0;
    }
    if (name == "SDL_CloseAudio") {
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(1);
        if (it != I.sdl_devs_.end()) {
            if (it->second.callback_active &&
                it->second.callback_host == std::this_thread::get_id()) {
                it->second.close_deferred = true;
                tr(0); return 0;
            }
            I.callback_cv.wait(pmu, [&] {
                auto cur = I.sdl_devs_.find(1);
                return cur == I.sdl_devs_.end() || !cur->second.callback_active;
            });
            it = I.sdl_devs_.find(1);
            if (it == I.sdl_devs_.end()) { tr(0); return 0; }
            it->second.stop = true;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.sdl_devs_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "SDL_GetCurrentAudioDriver") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        static uint64_t drvbuf = 0;
        if (!drvbuf) drvbuf = I.mem->mmap_alloc(32);
        if (!drvbuf) { tr(-ENOMEM); return -ENOMEM; }
        const char* n = "bifrost";
        I.mem->write(drvbuf, n, strlen(n) + 1);
        tr((int64_t)drvbuf); return (int64_t)drvbuf;
    }
    if (name == "SDL_GetNumAudioDevices") { tr(0); return 1; }
    if (name == "SDL_GetAudioDeviceName") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        static uint64_t namebuf = 0;
        if (!namebuf) namebuf = I.mem->mmap_alloc(64);
        const char* n = "bifrost-audio";
        I.mem->write(namebuf, n, strlen(n) + 1);
        tr((int64_t)namebuf); return (int64_t)namebuf;
    }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
