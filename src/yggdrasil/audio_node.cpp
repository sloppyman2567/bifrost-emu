// yggdrasil/audio_node.cpp — AudioNode implementation.
#include "yggdrasil/audio_node.hpp"
#include "audio/audio.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

AudioNode::AudioNode(Audio* audio, int flags)
    : audio_(audio), flags_(flags) {}

AudioNode::~AudioNode() = default;

ssize_t AudioNode::read(uint64_t /*off*/, void* /*buf*/, size_t /*n*/) {
    // Recording not yet supported — return 0 (EOF) immediately.
    return 0;
}

ssize_t AudioNode::write(uint64_t /*off*/, const void* buf, size_t n) {
    if (!audio_) return static_cast<ssize_t>(n);  // no backend — discard
    return audio_->write(static_cast<const uint8_t*>(buf), n);
}

ssize_t AudioNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}

int AudioNode::fstat(struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;  // character device
    st->st_size = 0;
    return 0;
}

} // namespace arm64emu::yggdrasil
