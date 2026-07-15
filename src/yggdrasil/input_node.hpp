// yggdrasil/input_node.hpp — /dev/input/eventX + /dev/input/js0 wrapper (Turn 38-39).
//
// InputNode wraps a FrostInput instance. The guest opens /dev/input/eventX
// (or /dev/input/js0) and reads event records from it. The record format
// depends on which device was opened:
//   - /dev/input/eventX: 24-byte input_event records (EV_KEY/EV_REL/EV_ABS)
//   - /dev/input/js0:    8-byte js_event records (JS_EVENT_BUTTON/AXIS)
//   - /dev/input/mice:   4-byte ImPS/2 packets (not yet implemented)
//
// The node is not seekable (lseek returns -ESPIPE). Writes are not
// supported (the device is read-only).
#pragma once
#include "yggdrasil/node.hpp"
#include "frost/input.hpp"
namespace arm64emu::yggdrasil {
class InputNode : public Node {
public:
    // `dev` selects the event format returned by read().
    explicit InputNode(::arm64emu::FrostInput* input,
                       ::arm64emu::InputDevice dev, int flags)
        : input_(input), dev_(dev), flags_(flags) {}
    ~InputNode() override = default;
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_; }
    int host_fd() const override { return -1; }
private:
    ::arm64emu::FrostInput* input_;
    ::arm64emu::InputDevice dev_;
    int flags_;
};
} // namespace arm64emu::yggdrasil
