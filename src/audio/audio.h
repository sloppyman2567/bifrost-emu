// audio/audio.h — Audio backend for bifrost-emu.
//
// Provides a simple PCM audio output device that guest programs can
// write to via /dev/snd (or the ioctl interface on /dev/dsp).
//
// The backend uses ALSA if available, otherwise falls back to writing
// raw PCM to a file (for headless testing). The design is intentionally
// minimal: the guest writes interleaved samples, and we buffer them
// into period-sized chunks for the backend.
#pragma once

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
    bool open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size);

    // Close the device and release resources.
    void close();

    // Write PCM data. `data` is interleaved samples. Returns the number
    // of bytes actually written (may be less than `len` if the buffer
    // is full).
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

    // Dump the audio buffer to a WAV file (for headless testing).
    bool dump_to_wav(const std::string& path);

private:
    bool opened_ = false;
    uint32_t sample_rate_ = 44100;
    uint8_t channels_ = 2;
    uint8_t sample_size_ = 2;  // 16-bit
    int fd_ = -1;              // raw PCM output fd (-1 if none)
    std::vector<uint8_t> buffer_;  // accumulated PCM data for WAV dump
    std::mutex mu_;            // protects buffer_, channels_, sample_rate_
};

} // namespace arm64emu
