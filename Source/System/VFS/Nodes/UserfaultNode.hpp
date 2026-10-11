// yggdrasil/uffd_node.hpp — userfaultfd(2) in SIGBUS mode.
//
// Implements the subset ART's concurrent mark-compact GC uses on Android
// 15 (art/runtime/gc/collector/mark_compact.cc): the UFFDIO_API handshake
// advertising UFFD_FEATURE_SIGBUS, MISSING-mode register/unregister, and
// UFFDIO_COPY / UFFDIO_ZEROPAGE / UFFDIO_WAKE, following Linux 6.12
// fs/userfaultfd.c and mm/userfaultfd.c. Missing-page state lives in
// Memory's direct-window page flags; touching a missing registered page
// raises SIGBUS BUS_ADRERR from the interpreter or the JIT memory guard.
//
// Not implemented: fault events read() from the descriptor (a context
// without UFFD_FEATURE_SIGBUS cannot register), write-protect and minor
// modes, UFFDIO_MOVE/CONTINUE/POISON, fork/remap/remove events and shmem
// ranges. Registration is per process, not per descriptor.
#pragma once
#include "System/VFS/Nodes/Node.hpp"
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace arm64emu::yggdrasil {

class UffdNode : public Node {
public:
    // UFFD_FEATURE_SIGBUS: the only feature this implementation offers.
    static constexpr uint64_t FEATURES = 1u << 7;
    explicit UffdNode(int flags) : flags_(flags) {}
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t, const void*, size_t) override { return -EINVAL; }
    ssize_t lseek(int64_t, int) override { return -ESPIPE; }
    int fstat(struct stat* st) override;
    int ioctl(uint32_t request, uint64_t argp, Memory& mem) override;
    bool seekable() const override { return false; }
    int flags() const override { return flags_; }
    void set_flags(int f) override { flags_ = f; }

private:
    int api(uint64_t argp, Memory& mem);
    int register_range(uint64_t argp, Memory& mem, bool registered);
    int fill(uint64_t argp, Memory& mem, bool copy);

    int flags_;
    // 0 until UFFDIO_API succeeds; then the requested features | INITIALIZED.
    std::atomic<uint64_t> features_{0};
};

} // namespace arm64emu::yggdrasil
