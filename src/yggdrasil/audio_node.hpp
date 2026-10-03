// yggdrasil/audio_node.hpp — /dev/dsp, /dev/snd, /dev/audio wrapper.
//
// Negotiates OSS playback parameters and owns one independent mixer stream.
// Recording is unsupported; reads return -EBADF.
#pragma once
#include "yggdrasil/node.hpp"
#include <atomic>
#include <mutex>
namespace arm64emu {
    class Audio;
}
namespace arm64emu::yggdrasil {
// ::arm64emu::Audio forward-declared above.
class AudioNode : public Node {
public:
    explicit AudioNode(::arm64emu::Audio* audio, int flags);
    ~AudioNode() override;
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    int ioctl(uint32_t request, uint64_t argp, Memory& mem) override;
    short poll_events(short events);
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_.load(); }
    void    set_flags(int f) override { flags_.store(f); }
    ::arm64emu::Audio* audio_backend() const { return audio_; }
private:
    ::arm64emu::Audio* audio_;
    std::atomic<int> flags_;
    std::mutex mu_;
    int stream_ = 0;
    int format_ = 0x10; // AFMT_S16_LE
    uint32_t rate_ = 44100;
    uint8_t channels_ = 2;
    uint32_t fragment_bytes_ = 4096, fragments_ = 4;
    bool triggered_ = true;
    bool ensure_stream();
    void close_stream();
    uint32_t pcm_format() const;
    size_t frame_bytes() const;
    bool buffer_info(size_t& queued_bytes, size_t& total_bytes);
};
} // namespace arm64emu::yggdrasil
