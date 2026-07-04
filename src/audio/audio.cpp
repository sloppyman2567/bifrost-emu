// audio/audio.cpp — Audio backend implementation.
//
// v1.4.5-alpha (Turn 38): added SDL2 audio backend. The class now
// supports three backends, tried in order:
//   1. SDL2 (if USE_SDL2 was set at build time)
//   2. OSS /dev/dsp (if available)
//   3. Headless (always available — buffer + WAV dump)
//
// The SDL2 backend uses a callback that pulls from a lock-free SPSC
// ring buffer. The guest's write() is the producer; SDL2's audio
// thread is the consumer. This avoids mutex contention on the hot
// path and gives predictable latency.
//
// The OSS backend is the original implementation: write() forwards
// to ::write(fd_, ...) with non-blocking I/O. Drop-on-EAGAIN.
//
// The headless backend just accumulates bytes in `buffer_` for a
// later dump_to_wav() call. Used when no audio device is available.
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

// Ring buffer capacity: 64 KiB. At 44100 Hz stereo 16-bit (176400 B/s),
// this is ~370 ms of audio — plenty to absorb guest write bursts.
static constexpr size_t RING_CAPACITY = 65536;

// ── Constructor / Destructor ───────────────────────────────────────────
Audio::Audio() = default;

Audio::~Audio() {
    close();
}

// ── open() — open the audio device with the given format ───────────────
bool Audio::open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size) {
    // Close any existing device first (idempotent re-open).
    close();

    sample_rate_ = sample_rate;
    channels_ = channels;
    sample_size_ = sample_size;

    // Try SDL2 first (preferred backend — cross-platform, low latency).
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
        int ch = channels;
        int sr = static_cast<int>(sample_rate);
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

// ── close() — release resources ────────────────────────────────────────
void Audio::close() {
    if (!opened_) return;
#if defined(BIFROST_USE_SDL2)
    close_sdl2_();
#endif
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    opened_ = false;
}

// ── write() — push PCM data into the active backend ────────────────────
ssize_t Audio::write(const uint8_t* data, size_t len) {
    if (!opened_) return -1;
    std::lock_guard<std::mutex> lock(mu_);

    // Always buffer the data for potential WAV dump.
    buffer_.insert(buffer_.end(), data, data + len);

#if defined(BIFROST_USE_SDL2)
    if (sdl_audio_dev_ != 0) {
        // Push into the SPSC ring buffer. The SDL2 callback will pull
        // from the other side. We use relaxed atomics — the SPSC
        // invariant (single producer, single consumer) is maintained
        // because only the guest thread calls write(), and only the
        // SDL2 audio thread calls the callback.
        size_t head = ring_head_.load(std::memory_order_relaxed);
        size_t tail = ring_tail_.load(std::memory_order_relaxed);
        size_t free_bytes = ring_mask_ + 1 - (tail - head);
        size_t to_write = std::min(len, free_bytes);

        for (size_t i = 0; i < to_write; i++) {
            ring_[(tail + i) & ring_mask_] = data[i];
        }
        ring_tail_.store(tail + to_write, std::memory_order_release);

        // Report full write so the guest doesn't re-send partial data.
        // Dropped samples (when free_bytes < len) are silent — the
        // guest would just retry otherwise, increasing latency.
        return static_cast<ssize_t>(len);
    }
#endif

    // OSS path.
    if (fd_ >= 0) {
        ssize_t written = ::write(fd_, data, len);
        if (written < 0) {
            // EAGAIN/EWOULDBLOCK — drop for real-time, keep in buffer.
        }
        // Report full write so guest doesn't re-send partial data.
    }

    return static_cast<ssize_t>(len);
}

// ── read() — recording (not yet implemented) ───────────────────────────
ssize_t Audio::read(uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

// ── ioctl() — OSS-style audio ioctls ───────────────────────────────────
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

// ── backend_name() — diagnostic ────────────────────────────────────────
const char* Audio::backend_name() const {
#if defined(BIFROST_USE_SDL2)
    if (sdl_audio_dev_ != 0) return "sdl2";
#endif
    if (fd_ >= 0) return "oss";
    return "none";
}

// ── dump_to_wav() — write the accumulated PCM to a WAV file ────────────
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

// ── SDL2 backend (Turn 38) ─────────────────────────────────────────────
#if defined(BIFROST_USE_SDL2)

bool Audio::open_sdl2_() {
    std::lock_guard<std::mutex> g(sdl_mu_);

    // Initialize SDL2 audio subsystem if not already done. We use
    // SDL_InitSubSystem (not SDL_Init) so we don't clobber any video
    // subsystem that FrostGraphics may have already initialized.
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            if (getenv("BIFROST_AUDIO_VERBOSE")) {
                fprintf(stderr, "[audio] SDL_InitSubSystem(AUDIO) failed: %s\n",
                        SDL_GetError());
            }
            return false;
        }
    }

    // Configure the audio spec. SDL2 expects signed 16-bit (or float)
    // samples; we map our sample_size to the closest SDL format.
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(sample_rate_);
    want.channels = channels_;
    want.samples = 1024;  // ~23 ms at 44100 Hz — low latency
    want.callback = sdl2_audio_callback_;
    want.userdata = this;

    if (sample_size_ == 1) {
        want.format = AUDIO_U8;       // 8-bit unsigned
    } else if (sample_size_ == 2) {
        want.format = AUDIO_S16SYS;   // 16-bit signed (host endian)
    } else if (sample_size_ == 4) {
        want.format = AUDIO_F32SYS;   // 32-bit float
    } else {
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

    // Initialize the SPSC ring buffer. Power-of-2 capacity for fast
    // masking. We use the larger of RING_CAPACITY and 4× the SDL
    // buffer size to ensure we never starve the callback.
    size_t sdl_buf_bytes = static_cast<size_t>(got.size);
    size_t cap = RING_CAPACITY;
    while (cap < sdl_buf_bytes * 4) cap *= 2;
    ring_.assign(cap, 0);
    ring_mask_ = cap - 1;
    ring_head_.store(0, std::memory_order_relaxed);
    ring_tail_.store(0, std::memory_order_relaxed);

    // Update our format to match what SDL2 actually gave us (may differ
    // from what we asked for).
    sample_rate_ = static_cast<uint32_t>(got.freq);
    channels_ = static_cast<uint8_t>(got.channels);
    if (got.format == AUDIO_U8) sample_size_ = 1;
    else if (got.format == AUDIO_S16SYS) sample_size_ = 2;
    else if (got.format == AUDIO_F32SYS) sample_size_ = 4;

    // Start playback.
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
        SDL_CloseAudioDevice(sdl_audio_dev_);
        sdl_audio_dev_ = 0;
    }
}

// ── sdl2_audio_callback_ — pull samples from the ring buffer ───────────
// Called from SDL2's audio thread. Must be fast and lock-free.
void Audio::sdl2_audio_callback_(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<Audio*>(userdata);
    if (!self || len <= 0) return;

    size_t head = self->ring_head_.load(std::memory_order_relaxed);
    size_t tail = self->ring_tail_.load(std::memory_order_acquire);
    size_t avail = tail - head;

    if (avail >= static_cast<size_t>(len)) {
        // Enough data — copy from the ring buffer.
        for (int i = 0; i < len; i++) {
            stream[i] = self->ring_[(head + i) & self->ring_mask_];
        }
        self->ring_head_.store(head + len, std::memory_order_release);
    } else {
        // Partial — copy what we have, silence the rest. This is the
        // correct behavior for a real audio device when the producer
        // is too slow (underrun).
        size_t i = 0;
        for (; i < avail; i++) {
            stream[i] = self->ring_[(head + i) & self->ring_mask_];
        }
        memset(stream + i, 0, static_cast<size_t>(len) - i);
        self->ring_head_.store(head + avail, std::memory_order_release);
    }
}

#endif  // BIFROST_USE_SDL2

} // namespace arm64emu
