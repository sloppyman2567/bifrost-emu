// audio/audio.cpp — Audio backend implementation.
//
// Uses a simple approach: buffer PCM data in memory and optionally dump
// to a WAV file on close. For real-time playback, we write to a pipe or
// /dev/null (headless). A future version could link against ALSA for
// actual sound output.
#include "audio/audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>

namespace arm64emu {

Audio::Audio() = default;

Audio::~Audio() {
    close();
}

bool Audio::open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    sample_size_ = sample_size;

    // Try to open /dev/dsp (OSS) for real-time playback.
    // If that fails (headless environment), we buffer in memory.
    fd_ = ::open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (fd_ >= 0) {
        // Configure OSS: set format, channels, sample rate.
        int fmt = AFMT_S16_NE;
        int ch = channels;
        int sr = static_cast<int>(sample_rate);
        ::ioctl(fd_, SNDCTL_DSP_SETFMT, &fmt);
        ::ioctl(fd_, SNDCTL_DSP_CHANNELS, &ch);
        ::ioctl(fd_, SNDCTL_DSP_SPEED, &sr);
    }

    opened_ = true;
    buffer_.clear();
    return true;
}

void Audio::close() {
    if (!opened_) return;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    opened_ = false;
}

ssize_t Audio::write(const uint8_t* data, size_t len) {
    if (!opened_) return -1;

    // Buffer the data for potential WAV dump.
    buffer_.insert(buffer_.end(), data, data + len);

    // If we have a real audio device, write to it (non-blocking).
    if (fd_ >= 0) {
        ssize_t written = ::write(fd_, data, len);
        if (written < 0) written = 0;  // would block — buffer it
        return written;
    }

    // No real device — pretend we wrote everything (buffered in memory).
    return static_cast<ssize_t>(len);
}

ssize_t Audio::read(uint8_t* buf, size_t len) {
    // Recording not yet implemented.
    (void)buf;
    (void)len;
    return 0;
}

int Audio::ioctl(uint32_t cmd, uint64_t arg) {
    // Forward to the real OSS device if we have one.
    if (fd_ >= 0) {
        return ::ioctl(fd_, cmd, reinterpret_cast<void*>(arg));
    }
    // Headless: return sensible defaults for common queries.
    switch (cmd) {
        case SNDCTL_DSP_GETFMTS: {
            int* p = reinterpret_cast<int*>(arg);
            if (p) *p = AFMT_S16_NE;
            return 0;
        }
        case SNDCTL_DSP_SETFMT: {
            int* p = reinterpret_cast<int*>(arg);
            if (p && *p == AFMT_S16_NE) return 0;
            return -EINVAL;
        }
        case SNDCTL_DSP_CHANNELS: {
            int* p = reinterpret_cast<int*>(arg);
            if (p) {
                channels_ = static_cast<uint8_t>(*p);
                return 0;
            }
            return -EINVAL;
        }
        case SNDCTL_DSP_SPEED: {
            int* p = reinterpret_cast<int*>(arg);
            if (p) {
                sample_rate_ = static_cast<uint32_t>(*p);
                return 0;
            }
            return -EINVAL;
        }
        default:
            return -ENOSYS;
    }
}

bool Audio::dump_to_wav(const std::string& path) {
    if (buffer_.empty()) return false;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    // Write WAV header.
    uint32_t data_size = static_cast<uint32_t>(buffer_.size());
    uint32_t byte_rate = sample_rate_ * channels_ * sample_size_;
    uint16_t block_align = channels_ * sample_size_;
    uint16_t bits_per_sample = sample_size_ * 8;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    uint32_t riff_size = 36 + data_size;
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1;  // PCM
    fwrite(&audio_format, 2, 1, f);
    uint16_t channels_u16 = channels_;  // promote uint8_t → uint16_t
    fwrite(&channels_u16, 2, 1, f);
    fwrite(&sample_rate_, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(buffer_.data(), 1, buffer_.size(), f);

    fclose(f);
    return true;
}

} // namespace arm64emu
