// Android AAudio streams and the guest callback-thread protocol.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
namespace {
// AOSP libaaudio (android-15.0.0_r1): AAudioUtilities.h channel index bit,
// FCC_LIMIT from system/audio.h, and AAudioStreamParameters::validateChannelMask.
constexpr uint32_t AA_CHANNEL_BIT_INDEX = 0x80000000u;
constexpr int32_t AA_FCC_LIMIT = 12;
bool aa_output_mask_valid(int32_t mask) {
    const uint32_t m = static_cast<uint32_t>(mask);
    if (m & AA_CHANNEL_BIT_INDEX) {
        const uint32_t bits = m & ~AA_CHANNEL_BIT_INDEX;
        return bits && bits < (1u << 24) && (bits & (bits + 1)) == 0;  // INDEX_MASK_1..24
    }
    static const uint32_t layouts[] = {
        0x1, 0x3, 0xb, 0x7, 0x103, 0xf, 0xc0003, 0xc000b, 0xc0007, 0xc000f, 0x33, 0x603,
        0x107, 0x37, 0x3f, 0x60f, 0xc003f, 0x2d03f, 0x13f, 0x63f, 0xc063f, 0x2d63f,
        0x302d63f, 0x30ed63f};  // MONO ... 9POINT1POINT6 (AAudio.h)
    for (uint32_t layout : layouts)
        if (m == layout) return true;
    return false;
}
}  // namespace
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::aaudio() {
    // ══ AAudio (Android) ════════════════════════════════════════════
    if (name == "AAudioStreamBuilder_new" || name == "AAudio_createStreamBuilder") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        uint64_t out = R(0);
        if (!out) { tr(-886); return -886; } // AAUDIO_ERROR_NULL
        I.mem->check_access(out, 8, Memory::GUEST_PROT_WRITE);
        const uint64_t h = 0xAA000000ull + ++I.next_aa_builder_;
        I.aa_builders_[h] = AudioThunkImpl::AaBuilder{};
        if (out) wr64(I, out, h);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_delete") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        I.aa_builders_.erase(R(0));
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setFormat") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) it->second.fmt = (int32_t)R(1);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setChannelCount" ||
        name == "AAudioStreamBuilder_setSamplesPerFrame") {
        // AOSP AAudioConvert_channelCountToMask: a count is an index mask.
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        const int32_t count = (int32_t)R(1);
        if (it != I.aa_builders_.end())
            it->second.channel_mask = count < 0 || count > 30 ? -1  // AAUDIO_CHANNEL_INVALID
                : count == 0 ? 0 : (int32_t)(AA_CHANNEL_BIT_INDEX | ((1u << count) - 1));
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setChannelMask" ||
        name == "AAudioStreamBuilder_setContentType" ||
        name == "AAudioStreamBuilder_setSpatializationBehavior" ||
        name == "AAudioStreamBuilder_setIsContentSpatialized" ||
        name == "AAudioStreamBuilder_setAllowedCapturePolicy" ||
        name == "AAudioStreamBuilder_setPrivacySensitive") {
        // Like AOSP, values are stored here and validated by openStream.
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) {
            auto& b = it->second;
            const int32_t value = (int32_t)R(1);
            if (name == "AAudioStreamBuilder_setChannelMask") b.channel_mask = value;
            else if (name == "AAudioStreamBuilder_setContentType") b.content_type = value;
            else if (name == "AAudioStreamBuilder_setSpatializationBehavior") b.spatialization = value;
            else if (name == "AAudioStreamBuilder_setIsContentSpatialized") b.spatialized = (R(1) & 0xff) != 0;
            else if (name == "AAudioStreamBuilder_setAllowedCapturePolicy") b.capture_policy = value;
            else b.privacy = (R(1) & 0xff) ? 2 : 1;
        }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setPackageName" ||
        name == "AAudioStreamBuilder_setAttributionTag") {
        // Identify the app to the audio service's app-ops checks; no public
        // getter exposes them and the host has no such service.
        tr(0); return 0;
    }
    if (name == "AAudio_convertResultToText" || name == "AAudio_convertStreamStateToText") {
        // AOSP AudioGlobal_convert*ToText: the enum name, else "Unrecognized".
        static const std::map<int32_t, const char*> results = {
            {0, "AAUDIO_OK"}, {-899, "AAUDIO_ERROR_DISCONNECTED"},
            {-898, "AAUDIO_ERROR_ILLEGAL_ARGUMENT"}, {-896, "AAUDIO_ERROR_INTERNAL"},
            {-895, "AAUDIO_ERROR_INVALID_STATE"}, {-892, "AAUDIO_ERROR_INVALID_HANDLE"},
            {-890, "AAUDIO_ERROR_UNIMPLEMENTED"}, {-889, "AAUDIO_ERROR_UNAVAILABLE"},
            {-888, "AAUDIO_ERROR_NO_FREE_HANDLES"}, {-887, "AAUDIO_ERROR_NO_MEMORY"},
            {-886, "AAUDIO_ERROR_NULL"}, {-885, "AAUDIO_ERROR_TIMEOUT"},
            {-884, "AAUDIO_ERROR_WOULD_BLOCK"}, {-883, "AAUDIO_ERROR_INVALID_FORMAT"},
            {-882, "AAUDIO_ERROR_OUT_OF_RANGE"}, {-881, "AAUDIO_ERROR_NO_SERVICE"},
            {-880, "AAUDIO_ERROR_INVALID_RATE"}};
        static const std::map<int32_t, const char*> states = {
            {0, "AAUDIO_STREAM_STATE_UNINITIALIZED"}, {1, "AAUDIO_STREAM_STATE_UNKNOWN"},
            {2, "AAUDIO_STREAM_STATE_OPEN"}, {3, "AAUDIO_STREAM_STATE_STARTING"},
            {4, "AAUDIO_STREAM_STATE_STARTED"}, {5, "AAUDIO_STREAM_STATE_PAUSING"},
            {6, "AAUDIO_STREAM_STATE_PAUSED"}, {7, "AAUDIO_STREAM_STATE_FLUSHING"},
            {8, "AAUDIO_STREAM_STATE_FLUSHED"}, {9, "AAUDIO_STREAM_STATE_STOPPING"},
            {10, "AAUDIO_STREAM_STATE_STOPPED"}, {11, "AAUDIO_STREAM_STATE_CLOSING"},
            {12, "AAUDIO_STREAM_STATE_CLOSED"}, {13, "AAUDIO_STREAM_STATE_DISCONNECTED"}};
        const auto& table = name == "AAudio_convertResultToText" ? results : states;
        auto found = table.find((int32_t)R(0));
        const std::string text = found != table.end() ? found->second : "Unrecognized";
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto cached = I.aa_texts_.find(text);
        uint64_t addr = cached != I.aa_texts_.end() ? cached->second : 0;
        if (!addr) {
            // Every string together fits in one page; they stay for the process.
            if (!I.aa_text_block_ || I.aa_text_used_ + text.size() + 1 > 4096) {
                I.aa_text_block_ = I.mem->mmap_alloc(4096);
                I.aa_text_used_ = 0;
            }
            if (!I.aa_text_block_) { tr(0); return 0; }
            addr = I.aa_text_block_ + I.aa_text_used_;
            I.mem->write(addr, text.c_str(), text.size() + 1);
            I.aa_text_used_ += text.size() + 1;
            I.aa_texts_[text] = addr;
        }
        tr((int64_t)addr); return (int64_t)addr;
    }
    if (name == "AAudioStreamBuilder_setSampleRate") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) it->second.rate = (int32_t)R(1);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setDeviceId" ||
        name == "AAudioStreamBuilder_setDirection" ||
        name == "AAudioStreamBuilder_setSharingMode" ||
        name == "AAudioStreamBuilder_setPerformanceMode" ||
        name == "AAudioStreamBuilder_setUsage" ||
        name == "AAudioStreamBuilder_setInputPreset" ||
        name == "AAudioStreamBuilder_setSessionId" ||
        name == "AAudioStreamBuilder_setBufferCapacityInFrames") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) {
            auto& b = it->second;
            int32_t value = static_cast<int32_t>(R(1));
            if (name == "AAudioStreamBuilder_setDeviceId") b.device = value;
            else if (name == "AAudioStreamBuilder_setDirection") b.direction = value;
            else if (name == "AAudioStreamBuilder_setSharingMode") b.sharing = value;
            else if (name == "AAudioStreamBuilder_setPerformanceMode") b.performance = value;
            else if (name == "AAudioStreamBuilder_setUsage") b.usage = value;
            else if (name == "AAudioStreamBuilder_setInputPreset") b.input_preset = value;
            else if (name == "AAudioStreamBuilder_setSessionId") b.session = value;
            else b.capacity = value;
        }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setFramesPerDataCallback") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end())
            it->second.frames_per_cb = std::max<int32_t>(0, (int32_t)R(1));
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setDataCallback") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) { it->second.cb_fn = R(1); it->second.cb_ud = R(2); }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setErrorCallback") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) { it->second.err_fn = R(1); it->second.err_ud = R(2); }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_openStream") {
        uint64_t out = R(1);
        AudioThunkImpl::AaBuilder b;
        {
            std::lock_guard<std::recursive_mutex> g(I.mu);
            auto bit = I.aa_builders_.find(R(0));
            if (bit == I.aa_builders_.end()) { tr(-892); return -892; }
            if (!out) { tr(-886); return -886; }
            b = bit->second;   // copy: another thread may delete the builder
        }
        wr64(I, out, 0);
        // AAudio's setters defer validation until openStream. Zero means
        // unspecified; keep the Android defaults in the opened stream.
        if (b.usage == 0) b.usage = 1;
        if (b.input_preset == 0) b.input_preset = 6;
        const bool valid_usage = (b.usage >= 1 && b.usage <= 6) ||
            (b.usage >= 10 && b.usage <= 14) || b.usage == 16 ||
            (b.usage >= 1000 && b.usage <= 1003);
        const bool valid_preset = b.input_preset == 1 || b.input_preset == 5 ||
            b.input_preset == 6 || b.input_preset == 7 ||
            b.input_preset == 9 || b.input_preset == 10 ||
            b.input_preset == 1997 || b.input_preset == 1999;
        // A channel count comes from the mask (AAudioConvert_channelMaskToCount).
        const int32_t channels = __builtin_popcount(
            static_cast<uint32_t>(b.channel_mask) & ~AA_CHANNEL_BIT_INDEX);
        if (channels > AA_FCC_LIMIT) { tr(-882); return -882; } // OUT_OF_RANGE
        if (!valid_usage || !valid_preset) { tr(-898); return -898; }
        if (b.content_type < 0 || b.content_type > 4 || b.spatialization < 0 ||
            b.spatialization > 2 || b.capture_policy < 0 || b.capture_policy > 3 ||
            (b.channel_mask && !aa_output_mask_valid(b.channel_mask))) {
            tr(-898); return -898;
        }
        if (b.direction == 1) { tr(-890); return -890; } // Input not implemented.
        if (channels > 8) { tr(-889); return -889; } // The host engine mixes up to 8.
        if (b.direction != 0 || b.rate < 0 ||
            b.rate > 384000 || b.frames_per_cb < 0 || b.frames_per_cb > 65536 ||
            b.capacity < 0 || b.sharing < 0 || b.sharing > 1 || b.session < -1 ||
            b.performance < 10 || b.performance > 12) { tr(-898); return -898; }
        if (b.device != 0) { tr(-889); return -889; } // Only the default host device.
        if (b.fmt < 0 || b.fmt > 2) { tr(-883); return -883; } // I16/FLOAT only.
        // Hold pump_mu for the whole arm (same shape as SDL_OpenAudioDevice).
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        const uint64_t h = 0xAA100000ull + ++I.next_aa_stream_;
        auto& s = I.aa_streams_[h];
        s.fmt = (b.fmt == 2) ? PCM_FMT_F32 : PCM_FMT_S16;   // AAUDIO_FORMAT_FLOAT=2, I16=1
        s.size = (s.fmt == PCM_FMT_F32) ? 4 : 2;
        s.ch   = channels > 0 ? (uint8_t)channels : 2;
        s.rate = b.rate > 0 ? (uint32_t)b.rate : 48000;
        s.frames_per_cb = b.frames_per_cb > 0
            ? (uint32_t)b.frames_per_cb : 1024;
        s.aa_session = b.session == 0 ? static_cast<int32_t>(I.next_aa_stream_) : b.session;
        s.aa_usage = b.usage;
        s.aa_input_preset = b.input_preset;
        // AudioStream::open defaults; AudioStreamTrack reports an unspecified
        // mask as the stereo layout it opened, and hides index masks.
        s.aa_channel_mask = b.channel_mask == 0 ? 0x3
            : (static_cast<uint32_t>(b.channel_mask) & AA_CHANNEL_BIT_INDEX) ? 0
            : b.channel_mask;
        s.aa_content_type = b.content_type ? b.content_type : 2;        // MUSIC
        s.aa_spatialization = b.spatialization ? b.spatialization : 1;  // AUTO
        s.aa_spatialized = b.spatialized;
        s.aa_capture_policy = b.capture_policy ? b.capture_policy : 1;  // BY_ALL
        // AudioStreamBuilder::build: camcorder/communication capture defaults on.
        s.aa_privacy_sensitive = b.privacy == 2 || (b.privacy == 0 && b.direction == 1 &&
                                 (b.input_preset == 5 || b.input_preset == 7));
        s.cb_fn = b.cb_fn; s.cb_ud = b.cb_ud;
        s.stream_arg = (int64_t)h;
        s.aaudio = true;
        s.paused = true;  // AAudio callbacks start only after requestStart.
        wr64(I, out, h);
        if (s.cb_fn) {
            s.bounce_bytes = (size_t)s.frames_per_cb * s.ch * s.size;
            s.bounce = I.mem->mmap_alloc(s.bounce_bytes);
            if (!s.bounce) { I.aa_streams_.erase(h); tr(-ENOMEM); return -ENOMEM; }
            // The callback worker owns callback-mode execution. Inline
            // dispatch is the fallback when pump mode is disabled.
            s.cb_scheduled = true;
            s.next_cb_us = 0;
        }
        s.engine_stream = I.engine
            ? I.engine->stream_open(s.fmt, s.rate, s.ch) : 0;
        if (!s.engine_stream) {
            if (s.bounce) I.mem->untrack_allocation(s.bounce, s.bounce_bytes);
            I.aa_streams_.erase(h); wr64(I, out, 0); tr(-889); return -889;
        }
        size_t queued = 0, capacity = 0;
        I.engine->stream_buffer_info(s.engine_stream, s.rate, queued, capacity);
        s.aa_buffer_frames = static_cast<uint32_t>(capacity);
        if (b.capacity > 0 && static_cast<size_t>(b.capacity) > capacity) {
            I.engine->stream_close(s.engine_stream);
            if (s.bounce) I.mem->untrack_allocation(s.bounce, s.bounce_bytes);
            I.aa_streams_.erase(h); wr64(I, out, 0); tr(-889); return -889;
        }
        if (s.engine_stream) I.engine->stream_pause(s.engine_stream, true);
        const bool has_callback = s.cb_fn && s.bounce && s.cb_scheduled;
        pmu.unlock();
        if (has_callback) start_pump();
        I.wake_pump();
        tr(0); return 0;   // AAUDIO_OK
    }
    if (name == "AAudioStream_requestStart") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end() && it->second.aa_state == 11) {
            tr(-895); return -895;  // released: CLOSING
        }
        if (it != I.aa_streams_.end()) {
            it->second.paused = false;
            it->second.aa_state = 4;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, false);
        }
        I.wake_pump();
        I.callback_cv.notify_all();
        tr(0); return 0;
    }
    if (name == "AAudioStream_requestPause" || name == "AAudioStream_requestStop") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end() && it->second.aa_state == 11) {
            tr(-895); return -895;  // released: CLOSING
        }
        if (it != I.aa_streams_.end()) {
            it->second.paused = true;
            it->second.aa_state = name == "AAudioStream_requestPause" ? 6 : 10;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, true);
        }
        I.wake_pump();
        I.callback_cv.notify_all();
        tr(0); return 0;
    }
    if (name == "AAudioStream_requestFlush") {
        // AudioStream::safeFlush and AAudio_isFlushAllowed: output only, not
        // from the data callback, and only while OPEN/PAUSED/STOPPED/FLUSHED.
        // AudioStreamTrack's flush discards queued audio and counts it as read.
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        auto& s = it->second;
        if (s.callback_active && s.callback_host == std::this_thread::get_id()) {
            tr(-895); return -895;
        }
        if (s.aa_state != 2 && s.aa_state != 6 && s.aa_state != 10 && s.aa_state != 8) {
            tr(-895); return -895;
        }
        if (s.engine_stream && I.engine) I.engine->stream_clear(s.engine_stream);
        s.aa_state = 8;  // FLUSHED; the host flush completes immediately
        I.callback_cv.notify_all();
        tr(0); return 0;
    }
    if (name == "AAudioStream_release") {
        // AudioStream::safeRelease: frees the device (state CLOSING) but keeps
        // the handle until close. Illegal from the data callback.
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        if (it->second.callback_active &&
            it->second.callback_host == std::this_thread::get_id()) {
            tr(-895); return -895;
        }
        const uint64_t stream_id = it->first;
        I.callback_cv.wait(pmu, [&] {
            auto cur = I.aa_streams_.find(stream_id);
            return cur == I.aa_streams_.end() || !cur->second.callback_active;
        });
        it = I.aa_streams_.find(stream_id);
        if (it == I.aa_streams_.end()) { tr(0); return 0; }
        auto& s = it->second;
        if (s.aa_state != 11) {
            s.paused = true;
            s.aa_state = 11;
            if (s.engine_stream && I.engine) {
                I.engine->stream_frame_counters(s.engine_stream, s.aa_released_written,
                                                s.aa_released_read);
                I.engine->stream_close(s.engine_stream);
            }
            s.engine_stream = 0;
        }
        I.wake_pump();
        I.callback_cv.notify_all();
        tr(0); return 0;
    }
    if (name == "AAudioStream_getChannelMask" || name == "AAudioStream_getSamplesPerFrame" ||
        name == "AAudioStream_getAllowedCapturePolicy" ||
        name == "AAudioStream_getSpatializationBehavior" ||
        name == "AAudioStream_isContentSpatialized" || name == "AAudioStream_isPrivacySensitive") {
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) {
            const int64_t invalid = name.rfind("AAudioStream_is", 0) == 0 ? 0 : -892;
            tr(invalid); return invalid;
        }
        const auto& s = it->second;
        int64_t result =
            name == "AAudioStream_getChannelMask" ? s.aa_channel_mask
            : name == "AAudioStream_getSamplesPerFrame" ? s.ch  // == getChannelCount
            : name == "AAudioStream_getAllowedCapturePolicy" ? s.aa_capture_policy
            : name == "AAudioStream_getSpatializationBehavior" ? s.aa_spatialization
            : name == "AAudioStream_isContentSpatialized" ? s.aa_spatialized
            : s.aa_privacy_sensitive;
        tr(result); return result;
    }
    if (name == "AAudioStream_close") {
        std::unique_lock<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) {
            if (it->second.callback_active &&
                it->second.callback_host == std::this_thread::get_id()) {
                it->second.close_deferred = true;
                tr(0); return 0;
            }
            const uint64_t stream_id = it->first;
            I.callback_cv.wait(pmu, [&] {
                auto cur = I.aa_streams_.find(stream_id);
                return cur == I.aa_streams_.end() || !cur->second.callback_active;
            });
            it = I.aa_streams_.find(stream_id);
            if (it == I.aa_streams_.end()) { tr(0); return 0; }
            it->second.stop = true;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.aa_streams_.erase(it);
        }
        I.wake_pump();
        I.callback_cv.notify_all();  // a guest callback thread sees the close
        tr(0); return 0;
    }
    // ── AAudio guest callback thread (Android libaaudio adapter) ────────
    // Real libaaudio runs the data callback on a pthread of its own. Under
    // Bionic the pump has no guest TLS, so the adapter (Guest/Android/Adapters/
    // aaudio.c) creates that thread and drives it through these calls.
    if (name == "bifrost_aaudio_claim_callback") {  // (stream, uint64_t info[2])
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        auto& s = it->second;
        if (!s.cb_fn || !s.bounce) { tr(0); return 0; }  // write()-mode stream
        wr64(I, R(1), s.cb_fn);
        wr64(I, R(1) + 8, s.cb_ud);
        s.guest_thread = true;
        tr(1); return 1;
    }
    if (name == "bifrost_aaudio_wait_callback") {  // (stream, int32_t* frames)
        // Returns the bounce once the host ring has room for one callback,
        // 0 after at most 20 ms (the guest loops back through guest code,
        // so a group exit never waits on a host condition variable), or
        // AAUDIO_ERROR_INVALID_HANDLE once the stream is closed. Pacing
        // follows the ring fill, not a timer, so it cannot drift from the
        // host device. Not traced: it runs every period.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(20);
        std::unique_lock<std::mutex> lock(I.pump_mu);
        for (;;) {
            auto it = I.aa_streams_.find(R(0));
            if (it == I.aa_streams_.end() || !it->second.guest_thread ||
                it->second.stop.load()) return -892;
            auto& s = it->second;
            auto sleep = std::chrono::steady_clock::duration::max();
            size_t queued = 0, capacity = 0;
            if (!s.paused && !s.callback_active && s.engine_stream && I.engine &&
                I.engine->stream_buffer_info(s.engine_stream, s.rate, queued, capacity)) {
                // Keep about three callbacks, and at least two host device
                // periods, queued; the 370 ms ring would add its whole length
                // as latency if filled.
                uint32_t host_rate = 0, host_frames = 0;
                I.engine->output_config(host_rate, host_frames);
                const uint64_t host_period = host_rate
                    ? (uint64_t)host_frames * s.rate / host_rate : 0;
                const uint64_t target = std::min<uint64_t>(capacity,
                    std::max<uint64_t>(3ull * s.frames_per_cb,
                                       2 * host_period + s.frames_per_cb));
                if (queued == 0 || queued + s.frames_per_cb <= target) {
                    const size_t bytes = (size_t)s.frames_per_cb * s.ch * s.size;
                    I.mem->store<int32_t>(R(1), static_cast<int32_t>(s.frames_per_cb));
                    std::vector<uint8_t> silence(bytes, 0);
                    I.mem->write(s.bounce, silence.data(), bytes);
                    s.callback_active = true;
                    s.callback_host = std::this_thread::get_id();
                    return static_cast<int64_t>(s.bounce);
                }
                sleep = std::chrono::microseconds(std::max<uint64_t>(1000,
                    (queued + s.frames_per_cb - target) * 1000000ull / s.rate));
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return 0;
            I.callback_cv.wait_for(lock, std::min(sleep, deadline - now));
        }
    }
    if (name == "bifrost_aaudio_finish_callback") {  // (stream, callback result)
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) return -892;
        auto& s = it->second;
        if (!s.callback_active || s.callback_host != std::this_thread::get_id())
            return -895;  // AAUDIO_ERROR_INVALID_STATE
        s.callback_active = false;
        I.callback_cv.notify_all();
        if (s.close_deferred) {  // AAudioStream_close from inside the callback
            if (s.engine_stream && I.engine) I.engine->stream_close(s.engine_stream);
            if (s.bounce) I.mem->untrack_allocation(s.bounce, s.bounce_bytes);
            I.aa_streams_.erase(it);
            I.wake_pump();
            return -892;
        }
        if (static_cast<int32_t>(R(1)) != 0) {  // AAUDIO_CALLBACK_RESULT_STOP
            s.paused = true;
            s.aa_state = 10;
            if (s.engine_stream && I.engine) I.engine->stream_pause(s.engine_stream, true);
            return 0;
        }
        if (s.engine_stream && I.engine) {
            const size_t bytes = (size_t)s.frames_per_cb * s.ch * s.size;
            std::vector<uint8_t> data(bytes);
            I.mem->read(s.bounce, data.data(), bytes);
            I.engine->stream_write(s.engine_stream, s.fmt, s.rate, s.ch,
                                   data.data(), bytes);
        }
        return 0;
    }
    if (name == "AAudioStream_write") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.aa_streams_.end() && I.engine) {
            uint64_t data = R(1);
            int32_t frames = (int32_t)R(2);
            size_t bytes = (size_t)frames * it->second.ch * it->second.size;
            std::vector<uint8_t> tmp(bytes);
            I.mem->read(data, tmp.data(), bytes);
            ssize_t fr = it->second.engine_stream
                ? I.engine->stream_write(it->second.engine_stream,
                                         it->second.fmt, it->second.rate,
                                         it->second.ch, tmp.data(), bytes)
                : -1;
            // Honest frame count (AAudio returns frames written or a
            // negative error): partial acceptance is a short write, so
            // the guest retries the remainder instead of skipping it.
            rc = (fr >= 0) ? fr : -EIO;
        }
        tr(rc); return rc;
    }
    if (name == "AAudioStream_getState") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t st = it == I.aa_streams_.end() ? 12 :
            it->second.stop.load() ? 10 : it->second.aa_state;
        tr(st); return st;
    }
    if (name == "AAudioStream_waitForStateChange") {
        I.mem->check_access(R(2), 4, Memory::GUEST_PROT_WRITE);
        std::unique_lock<std::mutex> lock(I.pump_mu);
        auto state = [&] {
            auto it = I.aa_streams_.find(R(0));
            return it == I.aa_streams_.end() ? 12 :
                it->second.stop.load() ? 10 : it->second.aa_state;
        };
        bool changed = state() != static_cast<int32_t>(R(1));
        if (!changed && static_cast<int64_t>(R(3)) > 0)
            changed = I.callback_cv.wait_for(lock, std::chrono::nanoseconds(R(3)), [&] {
                return state() != static_cast<int32_t>(R(1));
            });
        I.mem->store<int32_t>(R(2), state());
        int64_t result = changed ? 0 : -885; // AAUDIO_ERROR_TIMEOUT
        tr(result); return result;
    }
    if (name == "AAudioStream_getTimestamp") {
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        if (!R(2) || !R(3)) { tr(-886); return -886; }
        // Android clock IDs: CLOCK_MONOTONIC=1 and CLOCK_BOOTTIME=7.
        if (R(1) != 1 && R(1) != 7) { tr(-898); return -898; }
        if (it->second.aa_state != 4) { tr(-895); return -895; }
        // SDL's mixer callback measures consumption into the host queue,
        // not hardware presentation. No presentation timestamp is available
        // from this backend. AAudio permits UNAVAILABLE and requires both
        // output arguments to remain unchanged on any error.
        tr(-889); return -889;
    }
    if (name == "AAudioStream_getDeviceId" || name == "AAudioStream_getSessionId" ||
        name == "AAudioStream_getUsage" || name == "AAudioStream_getInputPreset" ||
        name == "AAudioStream_getXRunCount" || name == "AAudioStream_isMMapUsed" ||
        name == "AAudioStream_getSharingMode" || name == "AAudioStream_getPerformanceMode" ||
        name == "AAudioStream_getDirection" || name == "AAudioStream_getBufferCapacityInFrames") {
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        int64_t result = 0;
        if (name == "AAudioStream_getSessionId") result = it->second.aa_session;
        else if (name == "AAudioStream_getUsage") result = it->second.aa_usage;
        else if (name == "AAudioStream_getInputPreset") result = it->second.aa_input_preset;
        else if (name == "AAudioStream_getSharingMode") result = 1; // Host mixer is shared.
        else if (name == "AAudioStream_getPerformanceMode") result = 10; // No low-latency HAL.
        else if (name == "AAudioStream_getBufferCapacityInFrames") {
            size_t queued, capacity;
            if (I.engine->stream_buffer_info(it->second.engine_stream, it->second.rate, queued, capacity))
                result = capacity;
        }
        tr(result); return result;
    }
    if (name == "AAudioStream_setBufferSizeInFrames") {
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it == I.aa_streams_.end()) { tr(-892); return -892; }
        if (static_cast<int32_t>(R(1)) < 0) { tr(-898); return -898; }
        // The host ring has a fixed capacity. AAudio permits returning a
        // different actual size; report that ring, rather than a fictitious limit.
        tr(it->second.aa_buffer_frames); return it->second.aa_buffer_frames;
    }
    if (name == "AAudioStream_getFormat") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t f = (it != I.aa_streams_.end() && it->second.fmt == PCM_FMT_F32) ? 2 : 1;
        tr(f); return f;
    }
    if (name == "AAudioStream_getChannelCount") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t c = it != I.aa_streams_.end() ? it->second.ch : 2;
        tr(c); return c;
    }
    if (name == "AAudioStream_getSampleRate") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t r = it != I.aa_streams_.end() ? (int64_t)it->second.rate : 48000;
        tr(r); return r;
    }
    if (name == "AAudioStream_getBufferSizeInFrames" ||
        name == "AAudioStream_getFramesPerBurst" ||
        name == "AAudioStream_getFramesPerDataCallback") {
        std::lock_guard<std::mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        int64_t r = it == I.aa_streams_.end() ? -892 :
            name == "AAudioStream_getBufferSizeInFrames" ? it->second.aa_buffer_frames :
            it->second.frames_per_cb;
        tr(r); return r;
    }
    if (name == "AAudioStream_getFramesRead" || name == "AAudioStream_getFramesWritten") {
        std::lock_guard<std::mutex> lock(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        uint64_t written = 0, read = 0;
        if (it != I.aa_streams_.end() && it->second.aa_state == 11) {
            written = it->second.aa_released_written;
            read = it->second.aa_released_read;
        } else if (it == I.aa_streams_.end() || !I.engine ||
            !I.engine->stream_frame_counters(it->second.engine_stream, written, read)) {
            tr(-892); return -892;
        }
        int64_t result = name == "AAudioStream_getFramesWritten" ? written : read;
        tr(result); return result;
    }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
