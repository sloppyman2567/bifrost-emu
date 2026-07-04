// yggdrasil/input_node.cpp — InputNode implementation (Turn 38).
//
// Reads return input_event records (24 bytes each on AArch64) from the
// FrostInput ring buffer. When the queue is empty, reads return 0 (EOF)
// — the guest should poll or sleep and retry.
#include "yggdrasil/input_node.hpp"
#include "frost/input.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

ssize_t InputNode::read(uint64_t /*off*/, void* buf, size_t n) {
    if (!input_) return -ENODEV;
    if (n == 0) return 0;
    // FrostInput::read returns bytes read (multiple of 24) or -errno.
    return input_->read(static_cast<uint8_t*>(buf), n, /*blocking=*/false);
}

ssize_t InputNode::write(uint64_t /*off*/, const void* /*buf*/, size_t /*n*/) {
    return -EACCES;  // input devices are read-only
}

ssize_t InputNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}

int InputNode::fstat(struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0400;  // char device, read-only
    st->st_size = 0;
    st->st_nlink = 1;
    return 0;
}

} // namespace arm64emu::yggdrasil
