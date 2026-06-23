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
    std::lock_guard<std::mutex> lock(mu_);

    // Buffer the data for potential WAV dump.
    buffer_.insert(buffer_.end(), data, data + len);

    // If we have a real audio device, write to it (non-blocking).
    // We always report 'len' bytes written so the guest doesn't retry
    // the same bytes (which would duplicate them in buffer_). The WAV
    // dump already has the full data; partial writes to /dev/dsp just
    // drop samples in the non-blocking case.
    if (fd_ >= 0) {
        ssize_t written = ::write(fd_, data, len);
        if (written < 0) {
            // EAGAIN/EWOULDBLOCK — drop for real-time, keep in buffer.
        }
        // Report full write so guest doesn't re-send partial data.
    }

    return static_cast<ssize_t>(len);
}

ssize_t Audio::read(uint8_t* buf, size_t len) {
    // Recording not yet implemented.
    (void)buf;
    (void)len;
    return 0;
}

int Audio::ioctl(uint32_t cmd, uint64_t arg) {
    std::lock_guard<std::mutex> lock(mu_);
    // Headless: handle common OSS ioctls by updating internal state.
    // We do NOT forward `arg` as a host pointer — it's a guest address.
    // The caller (ioctls.cpp) is responsible for marshaling pointer
    // arguments through host-side buffers if needed.
    switch (cmd) {
        case SNDCTL_DSP_GETFMTS:
            // arg would be int* in guest memory; caller handles marshaling.
            return 0;  // AFMT_S16_NE available
        case SNDCTL_DSP_SETFMT:
            // arg is the format value (not a pointer for SETFMT)
            if (static_cast<int>(arg) == AFMT_S16_NE) return 0;
            return -EINVAL;
        case SNDCTL_DSP_CHANNELS:
            channels_ = static_cast<uint8_t>(static_cast<int>(arg));
            return 0;
        case SNDCTL_DSP_SPEED:
            sample_rate_ = static_cast<uint32_t>(static_cast<int>(arg));
            return 0;
        default:
            // If we have a real device, forward the ioctl with the raw
            // arg value. This is safe for integer-arg ioctls; pointer-arg
            // ioctls need marshaling (not yet implemented).
            if (fd_ >= 0) {
                return ::ioctl(fd_, cmd, arg);
            }
            return -ENOSYS;
    }
}

bool Audio::dump_to_wav(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
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
