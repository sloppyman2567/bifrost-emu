// yggdrasil/audio_node.hpp — /dev/dsp, /dev/snd, /dev/audio wrapper.
//
// Delegates PCM writes to the Audio backend, which buffers them and
// can dump to a WAV file on close. Reads return 0 (recording not
// supported).
#pragma once
#include "yggdrasil/node.hpp"
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
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_; }
    ::arm64emu::Audio* audio_backend() const { return audio_; }
private:
    ::arm64emu::Audio* audio_;
    int flags_;
};
} // namespace arm64emu::yggdrasil
