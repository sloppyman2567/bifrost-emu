// yggdrasil/input_node.hpp — /dev/input/eventX wrapper (Turn 38).
//
// InputNode wraps a FrostInput instance. The guest opens /dev/input/eventX
// (or /dev/input/mice) and reads input_event records from it. Each record
// is 24 bytes (struct input_event on AArch64).
//
// The node is not seekable (lseek returns -ESPIPE). Writes are not
// supported (the device is read-only). The guest can poll/select on
// the fd — but we don't yet implement EPOLL/POLL for virtual nodes,
// so the guest must use blocking reads (which currently return 0
// immediately when the queue is empty; future enhancement would add
// a condvar for true blocking).
#pragma once

#include "yggdrasil/node.hpp"

namespace arm64emu {
    class FrostInput;
}

namespace arm64emu::yggdrasil {

class InputNode : public Node {
public:
    explicit InputNode(::arm64emu::FrostInput* input, int flags)
        : input_(input), flags_(flags) {}
    ~InputNode() override = default;

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_; }

    // No host fd — this is a purely virtual device.
    int host_fd() const override { return -1; }

private:
    ::arm64emu::FrostInput* input_;
    int flags_;
};

} // namespace arm64emu::yggdrasil
