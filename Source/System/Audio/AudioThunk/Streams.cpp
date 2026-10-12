// AudioDispatch helpers: tracing, callback arguments, format mapping,
// blocking stream writes, queue top-up and inline callback firing.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
void AudioDispatch::tr(int64_t r) const {
    if (trace) fprintf(stderr, "[audio-thunk] %s → %lld\n",
                       name.c_str(), (long long)r);
}
// Shared callback-arg builder: SDL2 callbacks are (userdata, stream,
// len); AAudio data callbacks are (stream, userdata, audioData, frames).
void AudioDispatch::fill_cb_args(const AudioThunkImpl::PumpStream& s,
                                 int64_t* ia, size_t& na, size_t cb_bytes) {
    if (s.aaudio) {
        ia[0] = (int64_t)s.stream_arg;
        ia[1] = (int64_t)s.cb_ud;
        ia[2] = (int64_t)s.bounce;
        ia[3] = (int64_t)s.frames_per_cb;
        na = 4;
    } else {  // SDL2: (userdata, stream, len)
        ia[0] = (int64_t)s.cb_ud;
        ia[1] = (int64_t)s.bounce;
        ia[2] = (int64_t)cb_bytes;
        na = 3;
    }
}
uint32_t AudioDispatch::sdl_fmt_map(uint16_t f) {
    switch (f & 0xFF1F) {
        case 0x0008: return PCM_FMT_U8;
        case 0x8020: return PCM_FMT_S32;   // AUDIO_S32LSB: real 32-bit,
                                           // converted by the engine
        case 0x0120: case 0x8120: case 0x9120: return PCM_FMT_F32;
        case 0x8030: return PCM_FMT_S24;
        default: return PCM_FMT_S16;                   // S16 variants
    }
}
// Guest bytes per sample — the engine's own helper, so accounting can
// never disagree with conversion (the old local copy said 2 for S24
// while the engine converts 4-byte containers).
uint8_t AudioDispatch::fmt_size(uint32_t f) {
    uint8_t s = pcm_fmt_size(f);
    return s ? s : 2;
}
// Blocking write with bounded wait (ALSA writei / Pulse simple
// semantics: the real calls BLOCK until all bytes are accepted, they
// never report a short 0-with-data-pending — guests like the mambo
// test treat w<=0 as fatal and stop feeding). Returns total BYTES
// accepted (short only on budget expiry or dead stream). Frame
// aligned: advances by whole accepted frames only.
size_t AudioDispatch::stream_write_wait(int estream, uint32_t fmt, uint32_t rate, uint8_t ch,
                                        const uint8_t* data, size_t bytes) {
    if (!I.engine || !estream || !data || !bytes) return 0;
    const size_t fsz = (size_t)fmt_size(fmt) * ch;
    if (!fsz || !rate || !ch) return 0;
    size_t off = 0;
    const uint64_t deadline =
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() +
        2000000ull;   // 2 s budget: ~20x a 16 KiB chunk's drain time
    for (;;) {
        ssize_t fr = I.engine->stream_write(estream, fmt, rate, ch,
                                            data + off, bytes - off);
        if (fr < 0) break;   // dead stream — keep partial progress
        off += (size_t)fr * fsz;
        if (off >= bytes) break;
        uint64_t now =
            (uint64_t)std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (now >= deadline) break;
        usleep(1000);   // let the host mixer drain the ring
    }
    return off;
}
// Drain a device's queue-mode backlog into ITS OWN engine stream
// (real-SDL semantics: each logical device has an independent queue;
// the engine mixer sums all devices). Backpressure is per-stream —
// a full stream ring makes stream_write accept 0 and we retry on the
// next dispatch. No global level check: one chatty device can never
// starve another (the old shared-ring design let music permanently
// pin the ring level and silently kill every other device).
// Called from audio-thunk dispatches on the guest thread — no pump
// threads, no async guest callbacks.
void AudioDispatch::top_up_queue(AudioThunkImpl::PumpStream& s) {
    if (!I.engine || !s.engine_stream) return;
    constexpr size_t kChunk = 16 * 1024;
    const size_t frame_sz = (size_t)fmt_size(s.fmt) * s.ch;
    while (!s.paused.load(std::memory_order_relaxed) &&
           s.pending_off < s.pending.size()) {
        size_t n = std::min(kChunk, s.pending.size() - s.pending_off);
        ssize_t fr = I.engine->stream_write(
            s.engine_stream, s.fmt, s.rate, s.ch,
            s.pending.data() + s.pending_off, n);
        if (fr <= 0) break;   // stream ring full — retry next dispatch
        s.pending_off += (size_t)fr * frame_sz;
    }
    // Compact once the consumed prefix gets large; reset when empty.
    if (s.pending_off >= s.pending.size()) {
        s.pending.clear();
        s.pending_off = 0;
    } else if (s.pending_off > 1024 * 1024) {
        s.pending.erase(s.pending.begin(),
                        s.pending.begin() + (long)s.pending_off);
        s.pending_off = 0;
    }
}
// Fire due guest audio callbacks inline when pump mode is disabled.
// The borrow-CPU
// runner runs from inside the syscall path of a SUSPENDED guest (the
// same proven context as dlopen's guest_call_args_), never
// concurrently with executing guest code. Called from frequently-hit
// SDL audio arms; a reentrancy guard covers guest callbacks that
// themselves call back into SDL.
void AudioDispatch::run_due_callbacks() {
    if (I.pump_enabled) return;
    if (audio_in_callbacks || !I.runner || !I.engine) return;
    audio_in_callbacks = true;
    struct Reset { ~Reset() { audio_in_callbacks = false; } } reset_guard;
    const uint64_t now_us = [] {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }();
    // Snapshot device handles under lock: a guest callback may itself
    // call SDL_CloseAudioDevice / AAudioStream_close etc., mutating
    // the maps mid-iteration, and the callback worker scans them
    // concurrently. Each firing snapshots its args under pump_mu,
    // advances next_cb_us BEFORE running guest code (so a concurrent
    // thread can never double-fire the same period), and re-checks
    // liveness under lock before pushing.
    auto pump_due = [this, &now_us](
                        std::map<uint64_t, AudioThunkImpl::PumpStream>&
                            devs,
                        uint64_t h) {
        for (int iter = 0; iter < 4; iter++) {
            uint64_t cb_fn = 0, bounce = 0;
            int64_t ia[4]; size_t na = 0;
            size_t cb_bytes = 0;
            uint32_t fmt = PCM_FMT_S16, rate = 44100;
            uint8_t ch = 2;
            int estream = 0;
            {
                std::lock_guard<std::mutex> g(I.pump_mu);
                auto dit = devs.find(h);
                if (dit == devs.end()) return;
                AudioThunkImpl::PumpStream& s = dit->second;
                if (!s.cb_fn || !s.bounce || !s.cb_scheduled ||
                    s.guest_thread || s.callback_active) return;
                if (s.callback_lock_depth) return;
                if (s.paused.load(std::memory_order_relaxed)) {
                    s.next_cb_us = now_us;   // resume on unpause, no burst
                    return;
                }
                cb_bytes = (size_t)s.frames_per_cb * s.ch * s.size;
                if (!cb_bytes) return;
                const uint64_t period_us = std::max<uint64_t>(
                    (uint64_t)cb_bytes * 1000000ULL /
                        ((uint64_t)s.rate * s.ch * s.size),
                    1000);
                if (now_us < s.next_cb_us) return;
                if (now_us - s.next_cb_us > 500000ULL) {
                    // Fell far behind (long stall) — resync, don't burst.
                    s.next_cb_us = now_us;
                    return;
                }
                fill_cb_args(s, ia, na, cb_bytes);
                cb_fn = s.cb_fn; bounce = s.bounce;
                fmt = s.fmt; rate = s.rate; ch = s.ch;
                estream = s.engine_stream;
                s.callback_active = true;
                s.callback_host = std::this_thread::get_id();
                s.next_cb_us += period_us;
            }
            // Fire OUTSIDE the lock: guest code may call back into
            // audio-thunk dispatches (the stream mutex is released so
            // the guest can safely wait on or mutate other devices).
            try {
                std::vector<uint8_t> silence(cb_bytes,
                    fmt == PCM_FMT_U8 ? 128 : 0);
                I.mem->write(bounce, silence.data(), cb_bytes);
            } catch (...) {
                std::lock_guard<std::mutex> g(I.pump_mu);
                auto failed = devs.find(h);
                if (failed != devs.end())
                    failed->second.callback_active = false;
                I.callback_cv.notify_all();
                return;
            }
            try { I.runner(cpu, cb_fn, ia, na); } catch (...) {}
            // The callback may have closed its own device — re-check
            // liveness before touching the bounce or the stream.
            // The push uses the snapshot (the layout the callback
            // just rendered); the find is purely a liveness check.
            std::lock_guard<std::mutex> g(I.pump_mu);
            auto dit = devs.find(h);
            if (dit == devs.end()) return;
            auto& live = dit->second;
            if (live.close_deferred) {
                if (live.engine_stream && I.engine)
                    I.engine->stream_close(live.engine_stream);
                if (live.bounce)
                    I.mem->untrack_allocation(live.bounce, live.bounce_bytes);
                devs.erase(dit);
                I.callback_cv.notify_all();
                return;
            }
            try {
                std::vector<uint8_t> tmp(cb_bytes);
                I.mem->read(bounce, tmp.data(), cb_bytes);
                if (estream)
                    I.engine->stream_write(estream, fmt, rate, ch,
                                           tmp.data(), cb_bytes);
            } catch (...) { /* unmapped bounce — skip period */ }
            live.callback_active = false;
            I.callback_cv.notify_all();
        }
    };
    std::vector<uint64_t> handles;
    {
        std::lock_guard<std::mutex> g(I.pump_mu);
        handles.reserve(I.sdl_devs_.size());
        for (auto& [h, s] : I.sdl_devs_) { (void)s; handles.push_back(h); }
    }
    for (uint64_t h : handles)
        pump_due(I.sdl_devs_, h);   // re-finds; no-op if closed meanwhile
    handles.clear();
    {
        std::lock_guard<std::mutex> g(I.pump_mu);
        handles.reserve(I.aa_streams_.size());
        for (auto& [h, s] : I.aa_streams_) { (void)s; handles.push_back(h); }
    }
    for (uint64_t h : handles)
        pump_due(I.aa_streams_, h);
}
}  // namespace arm64emu
