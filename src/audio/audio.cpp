// audio/audio.cpp — Audio backend implementation.
//
// 1.5.5-alpha REWRITE: multi-stream mixer modeled on real SDL2/SDL3.
// One physical host device; each guest audio device gets its own logical
// stream with an independent float ring (converted + resampled at push
// time). The device callback sums all non-paused streams — so a music
// queue and an sfx queue play SIMULTANEOUSLY the way real SDL mixes,
// instead of interleaving sequentially in one shared ring (the old
// design starved sfx whenever music kept the global level pinned, and
// ClearQueuedAudio wiped every device at once).
#include "audio/audio.h"
#if defined(BIFROST_USE_SDL2)
#  include <SDL2/SDL.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
namespace arm64emu {
// ── Constructor / Destructor ───────────────────────────────────────────
Audio::Audio() = default;
Audio::~Audio() {
    close();
}
// ── sample conversion helpers ──────────────────────────────────────────
namespace {
inline float load_sample(uint32_t fmt, const uint8_t* p) {
    switch (fmt) {
        case PCM_FMT_U8:  return (static_cast<int>(p[0]) - 128) / 128.0f;
        case PCM_FMT_S16: {
            int16_t v;
            memcpy(&v, p, 2);
            return v / 32768.0f;
        }
        case PCM_FMT_S24: {
            // signed 24-bit in low bytes of a 32-bit container
            int32_t v = static_cast<int32_t>(p[0]) |
                        (static_cast<int32_t>(p[1]) << 8) |
                        (static_cast<int32_t>(p[2]) << 16);
            if (v & 0x800000) v -= 0x1000000;
            return v / 8388608.0f;
        }
        case PCM_FMT_S32: {
            int32_t v;
            memcpy(&v, p, 4);
            return v / 2147483648.0f;
        }
        case PCM_FMT_F32: {
            float v;
            memcpy(&v, p, 4);
            return v;
        }
        default: return 0.0f;
    }
}
inline void store_sample(uint8_t dev_fmt, uint8_t* p, float s) {
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    switch (dev_fmt) {
        case 1: p[0] = static_cast<uint8_t>(s * 127.0f + 128.0f); break;
        case 2: {
            int16_t v = static_cast<int16_t>(lrintf(s * 32767.0f));
            memcpy(p, &v, 2);
            break;
        }
        case 4: memcpy(p, &s, 4); break;
        default: break;
    }
}
}  // namespace
// convert_to_f32_ — input frames → interleaved float frames at the device
// rate (linear resample). Caller holds mu_ (uses conv_ scratch only for
// nothing — writes straight to `out`).
void Audio::convert_to_f32_(uint32_t fmt, uint32_t rate, uint8_t ch,
                            const uint8_t* data, size_t frames_in,
                            std::vector<float>& out) {
    const uint8_t fsz = pcm_fmt_size(fmt);
    out.clear();
    if (!fsz || !ch || !rate || !frames_in) return;
    if (rate == sample_rate_) {
        out.reserve(frames_in * channels_);
        for (size_t i = 0; i < frames_in; i++) {
            for (uint8_t c = 0; c < channels_; c++) {
                uint8_t sc = (c < ch) ? c : static_cast<uint8_t>(ch - 1);
                out.push_back(load_sample(fmt, data + (i * ch + sc) * fsz));
            }
        }
        return;
    }
    // Linear resample input rate → device rate.
    size_t frames_out = static_cast<size_t>(
        static_cast<uint64_t>(frames_in) * sample_rate_ / rate);
    out.resize(frames_out * channels_);
    double step = static_cast<double>(rate) / sample_rate_;
    for (size_t o = 0; o < frames_out; o++) {
        double pos = o * step;
        size_t i0 = static_cast<size_t>(pos);
        if (i0 >= frames_in) i0 = frames_in - 1;
        size_t i1 = (i0 + 1 < frames_in) ? i0 + 1 : i0;
        float frac = static_cast<float>(pos - i0);
        for (uint8_t c = 0; c < channels_; c++) {
            uint8_t sc = (c < ch) ? c : static_cast<uint8_t>(ch - 1);
            float a = load_sample(fmt, data + (i0 * ch + sc) * fsz);
            float b = load_sample(fmt, data + (i1 * ch + sc) * fsz);
            out[o * channels_ + c] = a + (b - a) * frac;
        }
    }
}
// ── physical device ────────────────────────────────────────────────────
bool Audio::open_device_locked_() {
    if (opened_) return true;
#if defined(BIFROST_USE_SDL2)
    if (open_sdl2_()) {
        opened_ = true;
        buffer_.clear();
        return true;
    }
#endif
    // Fall back to OSS /dev/dsp.
    fd_ = ::open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (fd_ >= 0) {
        int fmt = AFMT_S16_NE;
        int ch = channels_;
        int sr = static_cast<int>(sample_rate_);
        if (::ioctl(fd_, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_NE) {
            ::close(fd_); fd_ = -1;
        } else {
            ::ioctl(fd_, SNDCTL_DSP_CHANNELS, &ch);
            ::ioctl(fd_, SNDCTL_DSP_SPEED, &sr);
        }
    }
    opened_ = true;
    buffer_.clear();
    return true;
}
void Audio::close_device_locked_() {
#if defined(BIFROST_USE_SDL2)
    close_sdl2_();
#endif
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    opened_ = false;
}
// ── logical streams ────────────────────────────────────────────────────
int Audio::stream_open(uint32_t fmt, uint32_t rate, uint8_t ch) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!rate || !ch || !pcm_fmt_size(fmt)) return 0;
    if (!opened_) {
        // First stream fixes the physical device format.
        sample_rate_ = rate;
        channels_ = ch;
        sample_size_ = pcm_fmt_size(fmt);
        if (sample_size_ == 3) sample_size_ = 4;  // S24-in-32 container
        open_device_locked_();
    }
    Stream s;
    s.fmt = fmt; s.rate = rate; s.ch = ch;
    s.cap_frames = 1;
    while (s.cap_frames * channels_ * sizeof(float) < kStreamRingBytes)
        s.cap_frames <<= 1;
    s.ring.assign(s.cap_frames * channels_, 0.0f);
    const int id = ++next_stream_id_;
    streams_.emplace(id, std::move(s));
    return id;
}
void Audio::stream_close(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    streams_.erase(id);
}
void Audio::stream_pause(int id, bool paused) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(id);
    if (it != streams_.end()) it->second.paused = paused;
}
bool Audio::stream_paused(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(id);
    return it != streams_.end() && it->second.paused;
}
void Audio::stream_clear(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(id);
    if (it != streams_.end()) {
        it->second.head = 0;
        it->second.tail = 0;
    }
}
size_t Audio::stream_queued_frames(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(id);
    return it == streams_.end() ? 0 : (it->second.tail - it->second.head);
}
ssize_t Audio::stream_write(int id, uint32_t fmt, uint32_t rate, uint8_t ch,
                            const uint8_t* data, size_t bytes) {
    const uint8_t fsz = pcm_fmt_size(fmt);
    if (!fsz || !ch || !rate) return -1;
    const size_t frame_sz_in = static_cast<size_t>(fsz) * ch;
    const size_t frames_in = bytes / frame_sz_in;
    if (!frames_in) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = streams_.find(id);
    if (it == streams_.end()) return -1;
    return push_frames_locked_(it->second, fmt, rate, ch, data, frames_in);
}
// ── legacy single-device API (/dev/dsp VFS node) ───────────────────────
int Audio::ensure_legacy_stream_locked_(uint32_t rate, uint8_t ch,
                                        uint8_t size) {
    if (legacy_stream_) return legacy_stream_;
    if (!opened_) {
        sample_rate_ = rate;
        channels_ = ch;
        sample_size_ = size;
        open_device_locked_();
    }
    Stream s;
    s.rate = rate; s.ch = ch;
    s.cap_frames = 1;
    while (s.cap_frames * channels_ * sizeof(float) < kStreamRingBytes)
        s.cap_frames <<= 1;
    s.ring.assign(s.cap_frames * channels_, 0.0f);
    legacy_stream_ = ++next_stream_id_;
    streams_.emplace(legacy_stream_, std::move(s));
    return legacy_stream_;
}
bool Audio::open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!opened_) {
        sample_rate_ = sample_rate;
        channels_ = channels;
        sample_size_ = sample_size;
        open_device_locked_();
    }
    ensure_legacy_stream_locked_(sample_rate, channels, sample_size);
    return true;
}
void Audio::close() {
    std::lock_guard<std::mutex> lock(mu_);
    streams_.clear();
    legacy_stream_ = 0;
    if (opened_) close_device_locked_();
}
// push_frames_locked_ — convert input frames and append to a stream ring
// (all-or-nothing). mu_ held. Returns INPUT frames accepted (0 = full).
// Shared by stream_write() and the legacy write() path.
ssize_t Audio::push_frames_locked_(Stream& s, uint32_t fmt, uint32_t rate,
                                   uint8_t ch, const uint8_t* data,
                                   size_t frames_in) {
    if (!frames_in) return 0;
    const uint8_t fsz = pcm_fmt_size(fmt);
    if (!fsz || !ch || !rate) return -1;
    const size_t frame_sz_in = static_cast<size_t>(fsz) * ch;
    // PARTIAL acceptance: take as many whole INPUT frames as fit in the
    // ring after conversion (guests may push multi-second buffers in one
    // write — all-or-nothing would deadlock a write-retry loop).
    const size_t queued = s.tail - s.head;
    const size_t free_frames = s.cap_frames - queued;
    if (!free_frames) return 0;
    size_t accept_in = frames_in;
    if (rate != sample_rate_) {
        // conservative bound: out(accept) = accept*drate/rate (floor)
        accept_in = free_frames * rate / sample_rate_;
    } else {
        accept_in = free_frames;
    }
    if (accept_in > frames_in) accept_in = frames_in;
    if (!accept_in) return 0;
    convert_to_f32_(fmt, rate, ch, data, accept_in, conv_);
    const size_t out_frames = conv_.size() / channels_;
    if (!out_frames || out_frames > free_frames) {
        if (accept_in > 1) accept_in--;          // rounding safety
        if (!accept_in) return 0;
        convert_to_f32_(fmt, rate, ch, data, accept_in, conv_);
        if (conv_.size() / channels_ > free_frames) return 0;
    }
    const size_t mask = s.cap_frames * channels_ - 1;
    for (size_t i = 0; i < conv_.size(); i++)
        s.ring[(s.tail * channels_ + i) & mask] = conv_[i];
    s.tail += out_frames;
    bytes_pushed_.fetch_add(accept_in * frame_sz_in, std::memory_order_relaxed);
    // Optional WAV dump records the guest's own bytes (pre-conversion),
    // matching the old headless behavior.
    static const bool wav_dump = [] {
        const char* e = getenv("BIFROST_AUDIO_DUMP");
        return e && e[0] == '1';
    }();
    if (wav_dump)
        buffer_.insert(buffer_.end(), data, data + accept_in * frame_sz_in);
    return static_cast<ssize_t>(accept_in);
}

ssize_t Audio::read(uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}
// Legacy single-device write (/dev/dsp VFS node): route onto the legacy
// stream so the data plays through the same mixer as everything else.
ssize_t Audio::write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mu_);
    const int id = ensure_legacy_stream_locked_(sample_rate_, channels_,
                                                sample_size_);
    auto it = streams_.find(id);
    if (it == streams_.end()) return -1;
    const uint8_t fsz = sample_size_ ? sample_size_ : 2;
    const size_t frame_sz = static_cast<size_t>(fsz) * channels_;
    ssize_t fr = push_frames_locked_(it->second, PCM_FMT_S16, sample_rate_,
                                     channels_, data, len / frame_sz);
    if (fr < 0) return -1;
    return fr * static_cast<ssize_t>(frame_sz);   // bytes accepted
}
int Audio::ioctl(uint32_t cmd, uint64_t arg) {
    std::lock_guard<std::mutex> lock(mu_);
    switch (cmd) {
        case SNDCTL_DSP_GETFMTS:
            return 0;  // AFMT_S16_NE available
        case SNDCTL_DSP_SETFMT:
            if (static_cast<int>(arg) == AFMT_S16_NE) return 0;
            return -EINVAL;
        case SNDCTL_DSP_CHANNELS:
            channels_ = static_cast<uint8_t>(static_cast<int>(arg));
            return 0;
        case SNDCTL_DSP_SPEED:
            sample_rate_ = static_cast<uint32_t>(static_cast<int>(arg));
            return 0;
        default:
            if (fd_ >= 0) {
                return ::ioctl(fd_, cmd, arg);
            }
            return -ENOSYS;
    }
}
const char* Audio::backend_name() const {
#if defined(BIFROST_USE_SDL2)
    if (sdl_audio_dev_ != 0) return "sdl2";
#endif
    if (fd_ >= 0) return "oss";
    return "none";
}
bool Audio::dump_to_wav(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (buffer_.empty()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t write_size = buffer_.size();
    if (write_size > 0xFFFFFFFFULL) write_size = 0xFFFFFFFFULL;
    uint32_t data_size = static_cast<uint32_t>(write_size);
    uint32_t byte_rate = sample_rate_ * channels_ * sample_size_;
    uint16_t block_align = channels_ * sample_size_;
    uint16_t bits_per_sample = sample_size_ * 8;
    fwrite("RIFF", 1, 4, f);
    uint32_t riff_size = 36 + data_size;
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1;  // PCM
    fwrite(&audio_format, 2, 1, f);
    uint16_t channels_u16 = channels_;
    fwrite(&channels_u16, 2, 1, f);
    fwrite(&sample_rate_, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(buffer_.data(), 1, write_size, f);
    fclose(f);
    return true;
}
// ── device callback: the MIXER ─────────────────────────────────────────
// Runs on the SDL2 audio thread (~46×/s). Sums every non-paused stream's
// float ring into one mix buffer, clips, converts once to the device
// format. A starved stream contributes silence for its missing frames —
// exactly real-SDL underrun behavior.
void Audio::mixer_callback_(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<Audio*>(userdata);
    if (!self || len <= 0) return;
    static const bool stats = [] { const char* e=getenv("BIFROST_AUDIO_STATS"); return e&&e[0]=='1'; }();
    static uint32_t stat_tick = 0;
    ++stat_tick;
    if (stats && stat_tick % 92 == 1) {
        // ~2 s heartbeat: callback count + per-stream backlog
        fprintf(stderr, "[audio-hb] cbs=%u streams=", stat_tick);
        std::lock_guard<std::mutex> lk(self->mu_);
        for (auto& [id, st] : self->streams_)
            fprintf(stderr, "%d:%llums%s ", id,
                    (unsigned long long)((st.tail - st.head) * 1000ull /
                                         self->sample_rate_),
                    st.paused ? "(p)" : "");
        fprintf(stderr, "\n");
    }
    const uint8_t dfz = self->sample_size_;
    const uint8_t dch = self->channels_;
    const size_t frames_needed = static_cast<size_t>(len) / (dfz * dch);
    std::lock_guard<std::mutex> lock(self->mu_);
    self->mix_.assign(frames_needed * dch, 0.0f);
    std::vector<float>& mix = self->mix_;
    for (auto& [id, s] : self->streams_) {
        (void)id;
        if (s.paused) continue;
        // Stream rings hold DEVICE-channel interleaved float frames
        // (convert_to_f32_ expands at push time).
        const size_t avail = s.tail - s.head;
        const size_t n = std::min(avail, frames_needed);
        const size_t mask = s.cap_frames * dch - 1;
        const float* rp = s.ring.data();
        for (size_t f = 0; f < n; f++) {
            const size_t rb = ((s.head + f) * dch) & mask;
            for (uint8_t c = 0; c < dch; c++)
                mix[f * dch + c] += rp[rb + c];
        }
        if (stats && n < avail) { /* partial consume */ }
        if (stats && stat_tick % 46 == 0)
            fprintf(stderr, "[mixer] st=%d paused=%d queued=%llums fed=%llums cap=%llums\n",
                    id, (int)s.paused,
                    (unsigned long long)((s.tail-s.head)*1000ull/self->sample_rate_),
                    (unsigned long long)(n*1000ull/self->sample_rate_),
                    (unsigned long long)(s.cap_frames*1000ull/self->sample_rate_));
        s.head += n;
    }
    // Convert + clip into the output stream.
    size_t oi = 0;
    for (size_t f = 0; f < frames_needed; f++) {
        for (uint8_t c = 0; c < dch; c++) {
            store_sample(dfz, stream + oi, mix[f * dch + c]);
            oi += dfz;
        }
    }
}
// ── SDL2 backend ─────────────────────────────────────────────
#if defined(BIFROST_USE_SDL2)
bool Audio::open_sdl2_() {
    std::lock_guard<std::mutex> g(sdl_mu_);
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            if (getenv("BIFROST_AUDIO_VERBOSE")) {
                fprintf(stderr, "[audio] SDL_InitSubSystem(AUDIO) failed: %s\n",
                        SDL_GetError());
            }
            return false;
        }
    }
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(sample_rate_);
    want.channels = channels_;
    want.samples = 1024;  // ~23 ms at 44100 Hz — low latency
    want.callback = mixer_callback_;
    want.userdata = this;
    if (sample_size_ == 1)       want.format = AUDIO_U8;
    else if (sample_size_ == 2)  want.format = AUDIO_S16SYS;
    else if (sample_size_ == 4)  want.format = AUDIO_F32SYS;
    else {
        if (getenv("BIFROST_AUDIO_VERBOSE")) {
            fprintf(stderr, "[audio] unsupported sample_size=%u\n",
                    sample_size_);
        }
        return false;
    }
    SDL_AudioSpec got{};
    sdl_audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (sdl_audio_dev_ == 0) {
        if (getenv("BIFROST_AUDIO_VERBOSE")) {
            fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n",
                    SDL_GetError());
        }
        return false;
    }
    sample_rate_ = static_cast<uint32_t>(got.freq);
    channels_ = static_cast<uint8_t>(got.channels);
    if (got.format == AUDIO_U8) sample_size_ = 1;
    else if (got.format == AUDIO_S16SYS) sample_size_ = 2;
    else if (got.format == AUDIO_F32SYS) sample_size_ = 4;
    SDL_PauseAudioDevice(sdl_audio_dev_, 0);
    if (getenv("BIFROST_AUDIO_VERBOSE")) {
        fprintf(stderr, "[audio] SDL2 audio device opened: %u Hz, %u ch, "
                "%u bytes/sample (device=%u)\n",
                sample_rate_, channels_, sample_size_, sdl_audio_dev_);
    }
    return true;
}
void Audio::close_sdl2_() {
    std::lock_guard<std::mutex> g(sdl_mu_);
    if (sdl_audio_dev_ != 0) {
        fprintf(stderr, "[audio] HOST DEVICE CLOSE dev=%u\n", sdl_audio_dev_);
        SDL_CloseAudioDevice(sdl_audio_dev_);
        sdl_audio_dev_ = 0;
    }
}
#endif  // BIFROST_USE_SDL2
} // namespace arm64emu
