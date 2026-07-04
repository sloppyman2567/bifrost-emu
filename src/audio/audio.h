// audio/audio.h — Audio backend for bifrost-emu.
//
// Provides a simple PCM audio output device that guest programs can
// write to via /dev/snd (or the ioctl interface on /dev/dsp).
//
// Three backends are supported (selected at build time):
//
//   1. SDL2 (build with USE_SDL2=1) — opens a real SDL_AudioDeviceID
//      with a callback that pulls from the ring buffer. This is the
//      preferred backend for interactive use: low latency, cross-
//      platform, no ALSA/PulseAudio dependencies beyond SDL2.
//
//   2. OSS (/dev/dsp) — if SDL2 isn't available but /dev/dsp exists,
//      we open it for real-time playback. Headless servers typically
//      don't have /dev/dsp, so this rarely fires.
//
//   3. Headless (always available) — buffer PCM data in memory and
//      optionally dump to a WAV file on close. Used for automated
//      testing where no audio device is available.
//
// v1.4.5-alpha (Turn 38): added SDL2 audio backend. Previously the
// audio class only supported OSS + headless buffer. The SDL2 backend
// is preferred when available because it's cross-platform (works on
// Linux, macOS, Windows) and handles device hot-plugging gracefully.
//
// The ring buffer is single-producer (guest writes) / single-consumer
// (SDL2 audio callback reads). Lock-free SPSC with relaxed atomics —
// no mutex on the hot path.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace arm64emu {

class Audio {
public:
    Audio();
    ~Audio();

    // Open the audio device with the given sample rate, channels, and
    // sample size (bytes per sample). Returns true on success.
    // Idempotent — calling open() when already open reconfigures the
    // device with the new params (closing the old device first).
    bool open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size);

    // Close the device and release resources. The WAV dump buffer is
    // preserved so dump_to_wav() can still be called after close().
    void close();

    // Write PCM data. `data` is interleaved samples. Returns the number
    // of bytes actually written (may be less than `len` if the buffer
    // is full — caller should retry the remaining bytes).
    ssize_t write(const uint8_t* data, size_t len);

    // Read (for recording — not yet implemented, returns 0).
    ssize_t read(uint8_t* buf, size_t len);

    // ioctl support: set/get format parameters.
    // Returns 0 on success, -errno on failure.
    int ioctl(uint32_t cmd, uint64_t arg);

    bool ready() const { return opened_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint8_t channels() const { return channels_; }
    uint8_t sample_size() const { return sample_size_; }

    // Which backend is active (diagnostic). Returns one of:
    //   "sdl2"  — SDL2 audio device
    //   "oss"   — /dev/dsp (OSS)
    //   "none"  — headless (buffer only)
    const char* backend_name() const;

    // Dump the audio buffer to a WAV file (for headless testing).
    // Captures ALL bytes written since open(), even if a real audio
    // device was active (useful for regression testing).
    bool dump_to_wav(const std::string& path);

private:
    bool opened_ = false;
    uint32_t sample_rate_ = 44100;
    uint8_t channels_ = 2;
    uint8_t sample_size_ = 2;  // 16-bit
    int fd_ = -1;              // raw PCM output fd (-1 if none)
    std::vector<uint8_t> buffer_;  // accumulated PCM data for WAV dump
    std::mutex mu_;            // protects buffer_, channels_, sample_rate_

    // ── SDL2 audio backend state (Turn 38) ───────────────────────────
    // The SDL2 audio device ID. 0 = no SDL2 device. Stored as uint32_t
    // (not SDL_AudioDeviceID) to avoid pulling SDL2.h into this header.
    uint32_t sdl_audio_dev_ = 0;

    // SPSC ring buffer for the SDL2 audio callback. The guest writes
    // (producer); the SDL2 callback reads (consumer). Relaxed atomics
    // for the head/tail indices — no mutex on the hot path.
    std::vector<uint8_t> ring_;
    std::atomic<size_t> ring_head_{0};  // consumer (SDL2 callback)
    std::atomic<size_t> ring_tail_{0};  // producer (guest write)
    size_t ring_mask_ = 0;              // capacity - 1 (capacity is power of 2)
    std::mutex sdl_mu_;                  // protects sdl_audio_dev_ open/close

    // Open the SDL2 audio device with the current sample_rate/channels/
    // sample_size. Returns true on success. Sets sdl_audio_dev_.
    bool open_sdl2_();

    // Close the SDL2 audio device. No-op if not open.
    void close_sdl2_();

    // SDL2 audio callback (static, called from SDL2's audio thread).
    // Pulls samples from the ring buffer; if empty, writes silence.
    // The `userdata` is the Audio* pointer.
    static void sdl2_audio_callback_(void* userdata, uint8_t* stream, int len);
};

} // namespace arm64emu
