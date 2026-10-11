// ALSA PCM subset used by games, mapped onto AudioEngine streams.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::alsa() {
    // ══ ALSA (subset games actually use) ════════════════════════════
    if (name == "snd_pcm_open") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        uint64_t pcmp = R(0);
        std::string dev = rd_cstr(I, R(1));
        if (!pcmp) { tr(-EFAULT); return -EFAULT; }
        const uint64_t h = 0xA6000000ull + ++I.next_alsa_pcm_;
        I.alsa_pcms_[h] = AudioThunkImpl::AlsaPcm{};
        wr64(I, pcmp, h);
        if (dbg().thunk_trace)
            fprintf(stderr, "[audio-thunk] snd_pcm_open(\"%s\") → 0x%llx\n",
                    dev.c_str(), (unsigned long long)h);
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_malloc") {
        uint64_t hp = R(0);
        if (!hp) { tr(-EFAULT); return -EFAULT; }
        wr64(I, hp, 0xA6100000ull + (uint64_t)(uintptr_t)&I);  // stable fake ptr
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_free" || name == "snd_pcm_hw_params_any" ||
        name == "snd_pcm_hw_params_set_access" || name == "snd_pcm_hw_params") {
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_format") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) {
            switch ((int)R(2)) {   // snd_pcm_format_t (values from alsa/pcm.h)
                case 1:  it->second.fmt = PCM_FMT_U8; break;   // U8
                case 6:  case 7: it->second.fmt = PCM_FMT_S24; break;  // S24_LE/BE
                case 10: case 11: it->second.fmt = PCM_FMT_S32; break; // S32_LE/BE
                case 14: case 15: it->second.fmt = PCM_FMT_F32; break; // FLOAT_LE/BE
                default: it->second.fmt = PCM_FMT_S16; break;  // S16_* / others
            }
        }
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_channels") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) it->second.ch = (uint8_t)R(2);
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_rate") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) it->second.rate = (uint32_t)R(2);
        uint64_t dir = R(3);
        if (dir) wr32(I, dir, 0);
        tr(0); return 0;
    }
    if (name == "snd_pcm_writei") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.alsa_pcms_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.alsa_pcms_.end() && I.engine) {
            uint64_t buf = R(1);
            int64_t frames = (int64_t)R(2);
            // Negative frame counts would wrap to a gigantic size_t below
            // (guest bug or garbage reg) and OOM the host on the vector.
            if (frames < 0) { tr(-EINVAL); return -EINVAL; }
            size_t fsz = fmt_size(it->second.fmt);
            size_t bytes = (size_t)frames * it->second.ch * fsz;
            // Clamp one write to 64 MiB; the excess stays unwritten and the
            // short frame count tells the guest to feed the rest (ALSA
            // permits short writes).
            constexpr size_t kMaxBytes = 64u << 20;
            const size_t frame_sz = (size_t)it->second.ch * fsz;
            if (frame_sz && bytes > kMaxBytes) {
                frames = (int64_t)(kMaxBytes / frame_sz);
                bytes = (size_t)frames * frame_sz;
            }
            if (buf && bytes) {
                std::vector<uint8_t> tmp;
                try {
                    tmp.resize(bytes);
                    I.mem->read(buf, tmp.data(), bytes);
                } catch (...) { tr(-EFAULT); return -EFAULT; }
                // Lazily create this pcm's engine stream (fmt/rate/ch are
                // only final once hw_params have been set).
                if (!it->second.engine_stream)
                    it->second.engine_stream = I.engine->stream_open(
                        it->second.fmt, it->second.rate, it->second.ch);
                // Blocking semantics: wait out a full ring (bounded) so
                // the guest's pointer advances only past played frames.
                // A short return here would skip the remainder (chipmunk).
                size_t acc = it->second.engine_stream
                    ? stream_write_wait(it->second.engine_stream,
                                        it->second.fmt, it->second.rate,
                                        it->second.ch, tmp.data(), bytes)
                    : 0;
                rc = it->second.engine_stream
                    ? (int64_t)(acc / ((size_t)fsz * it->second.ch)) : -EIO;
            } else rc = frames;
        }
        tr(rc); return rc;
    }
    if (name == "snd_pcm_avail_update") { tr(4096); return 4096; }
    if (name == "snd_pcm_delay") {
        uint64_t p = R(1);
        if (p) wr64(I, p, 0);
        tr(0); return 0;
    }
    if (name == "snd_pcm_drain" || name == "snd_pcm_drop" ||
        name == "snd_pcm_pause" || name == "snd_pcm_recover" ||
        name == "snd_pcm_prepare" || name == "snd_pcm_start") {
        tr(0); return 0;
    }
    if (name == "snd_pcm_close") {
        const uint64_t h = R(0);
        int estream = 0;
        {
            // Snapshot the stream under lock, then DRAIN WITHOUT the lock:
            // the loop sleeps up to 5 s and holding I.mu that long stalls
            // every other audio dispatch (writei spins on stream_write_wait
            // needing the same mutex). Re-find under lock before closing —
            // a concurrent close may have taken the handle meanwhile.
            std::lock_guard<std::recursive_mutex> g(I.mu);
            auto it = I.alsa_pcms_.find(h);
            if (it != I.alsa_pcms_.end()) estream = it->second.engine_stream;
        }
        if (estream && I.engine) {
            // Drain-on-close: let the already-queued tail play out
            // before destroying the stream, or the end of the sound
            // is cut off. Bounded, and bails early if the mixer
            // isn't draining (e.g. headless — no silent 5 s stall).
            size_t last = SIZE_MAX;
            int still = 0;
            for (int i = 0; i < 500; i++) {   // 500 x 10 ms = 5 s max
                size_t q = I.engine->stream_queued_frames(estream);
                if (q == 0) break;
                if (q == last && ++still >= 20) break;
                if (q != last) { last = q; still = 0; }
                usleep(10000);
            }
        }
        {
            std::lock_guard<std::recursive_mutex> g(I.mu);
            auto it = I.alsa_pcms_.find(h);
            if (it != I.alsa_pcms_.end()) {
                if (it->second.engine_stream && I.engine)
                    I.engine->stream_close(it->second.engine_stream);
                I.alsa_pcms_.erase(it);
            }
        }
        tr(0); return 0;
    }
    if (name == "snd_strerror") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        static uint64_t strbuf = 0;
        if (!strbuf) strbuf = I.mem->mmap_alloc(64);
        const char* s = "audio-thunk";
        I.mem->write(strbuf, s, strlen(s) + 1);
        tr((int64_t)strbuf); return (int64_t)strbuf;
    }
    if (name == "snd_pcm_readi") { tr(-EAGAIN); return -EAGAIN; }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
