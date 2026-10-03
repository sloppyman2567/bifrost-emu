// Guest OSS playback device, backed by an independent host mixer stream.
#include "yggdrasil/audio_node.hpp"
#include "audio/audio.h"
#include "core/memory.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
namespace arm64emu::yggdrasil {
AudioNode::AudioNode(Audio* audio, int flags) : audio_(audio), flags_(flags) {}
AudioNode::~AudioNode() { close_stream(); }
uint32_t AudioNode::pcm_format() const {
    return format_ == AFMT_U8 ? PCM_FMT_U8 : format_ == AFMT_S8 ? PCM_FMT_S8 : PCM_FMT_S16;
}
size_t AudioNode::frame_bytes() const { return pcm_fmt_size(pcm_format()) * channels_; }
void AudioNode::close_stream() {
    if (audio_ && stream_) audio_->stream_close(stream_);
    stream_ = 0;
}
bool AudioNode::ensure_stream() {
    if (!audio_) return false;
    if (!stream_) {
        stream_ = audio_->stream_open(pcm_format(), rate_, channels_);
        if (stream_) audio_->stream_pause(stream_, !triggered_);
    }
    return stream_ != 0;
}
bool AudioNode::buffer_info(size_t& queued, size_t& total) {
    size_t qframes, capacity;
    if (!ensure_stream() || !audio_->stream_buffer_info(stream_, rate_, qframes, capacity)) return false;
    const size_t cap_bytes = capacity * frame_bytes();
    while (fragment_bytes_ > 16 && cap_bytes / fragment_bytes_ < 2) fragment_bytes_ >>= 1;
    const size_t count = std::min<size_t>(fragments_, cap_bytes / fragment_bytes_);
    if (!count) return false;
    fragments_ = static_cast<uint32_t>(count);
    total = count * fragment_bytes_;
    queued = std::min(total, qframes * frame_bytes());
    return true;
}
ssize_t AudioNode::read(uint64_t, void*, size_t) { return -EBADF; }
ssize_t AudioNode::write(uint64_t, const void* buf, size_t n) {
    if ((flags_.load() & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!n) return 0;
    std::unique_lock<std::mutex> lock(mu_);
    if (n < frame_bytes()) return -EINVAL;
    for (;;) {
        size_t queued, total;
        if (!buffer_info(queued, total)) return -ENODEV;
        size_t bytes = std::min(n, total - queued);
        bytes -= bytes % frame_bytes();
        if (bytes) {
            const ssize_t frames = audio_->stream_write(stream_, pcm_format(), rate_, channels_,
                static_cast<const uint8_t*>(buf), bytes);
            if (frames < 0) return -EIO;
            if (frames) return frames * static_cast<ssize_t>(frame_bytes());
        }
        if (flags_.load() & O_NONBLOCK) return -EAGAIN;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}
short AudioNode::poll_events(short events) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t queued, total;
    if (!buffer_info(queued, total)) return POLLERR;
    return (events & POLLOUT) && total - queued >= fragment_bytes_ ? POLLOUT : 0;
}
int AudioNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    std::unique_lock<std::mutex> lock(mu_);
    auto read_int = [&] {
        if (!argp) throw UnmappedMemory(argp, false);
        return mem.load<int32_t>(argp);
    };
    auto write_int = [&](int32_t v) {
        if (!argp) return -EFAULT;
        mem.store<int32_t>(argp, v);
        return 0;
    };
    try {
        switch (request) {
            case SNDCTL_DSP_RESET:
                if (stream_) audio_->stream_clear(stream_);
                return 0;
            case SNDCTL_DSP_NONBLOCK: flags_.fetch_or(O_NONBLOCK); return 0;
            case FIONBIO:
                if (read_int()) flags_.fetch_or(O_NONBLOCK); else flags_.fetch_and(~O_NONBLOCK);
                return 0;
            case SNDCTL_DSP_GETFMTS: return write_int(AFMT_S16_LE | AFMT_U8 | AFMT_S8);
            case SNDCTL_DSP_SETFMT: {
                int format = read_int();
                if (format == AFMT_QUERY) return write_int(format_);
                if (format != AFMT_U8 && format != AFMT_S8 && format != AFMT_S16_LE) format = AFMT_S16_LE;
                if (format != format_) { close_stream(); format_ = format; }
                return write_int(format_);
            }
            case SNDCTL_DSP_CHANNELS: {
                const int channels = read_int();
                if (channels <= 0) return -EINVAL;
                const uint8_t accepted = channels == 1 ? 1 : 2;
                if (accepted != channels_) { close_stream(); channels_ = accepted; }
                return write_int(channels_);
            }
            case SNDCTL_DSP_STEREO: {
                const uint8_t channels = read_int() ? 2 : 1;
                if (channels != channels_) { close_stream(); channels_ = channels; }
                return write_int(channels_ - 1);
            }
            case SNDCTL_DSP_SPEED: {
                const int rate = read_int();
                if (rate <= 0) return -EINVAL;
                const uint32_t accepted = std::clamp(rate, 8000, 192000);
                if (accepted != rate_) { close_stream(); rate_ = accepted; }
                return write_int(rate_);
            }
            case SNDCTL_DSP_SETFRAGMENT: {
                const uint32_t value = static_cast<uint32_t>(read_int());
                fragment_bytes_ = 1u << std::clamp(value & 0xffffu, 4u, 16u);
                fragments_ = std::clamp(value >> 16, 2u, 64u);
                return 0;
            }
            case SNDCTL_DSP_GETOSPACE: {
                if (!argp) return -EFAULT;
                size_t queued, total;
                if (!buffer_info(queued, total)) return -ENODEV;
                audio_buf_info info{};
                info.fragsize = fragment_bytes_; info.fragstotal = fragments_;
                info.bytes = static_cast<int>(total - queued);
                info.fragments = info.bytes / info.fragsize;
                mem.write(argp, &info, sizeof(info));
                return 0;
            }
            case SNDCTL_DSP_GETBLKSIZE: {
                size_t q, total;
                if (!buffer_info(q, total)) return -ENODEV;
                return write_int(fragment_bytes_);
            }
            case SNDCTL_DSP_GETODELAY: {
                size_t q, total;
                if (!buffer_info(q, total)) return -ENODEV;
                return write_int(static_cast<int32_t>(q));
            }
            case SNDCTL_DSP_GETCAPS: return write_int(DSP_CAP_REALTIME | DSP_CAP_TRIGGER);
            case SNDCTL_DSP_GETTRIGGER: return write_int(triggered_ ? PCM_ENABLE_OUTPUT : 0);
            case SNDCTL_DSP_SETTRIGGER:
                triggered_ = (read_int() & PCM_ENABLE_OUTPUT) != 0;
                if (stream_) audio_->stream_pause(stream_, !triggered_);
                return 0;
            default: return IOCTL_NOT_HANDLED;
        }
    } catch (const UnmappedMemory&) { return -EFAULT; }
}
ssize_t AudioNode::lseek(int64_t, int) { return -ESPIPE; }
int AudioNode::fstat(struct stat* st) {
    std::memset(st, 0, sizeof(*st)); st->st_mode = S_IFCHR | 0666;
    return 0;
}
}
