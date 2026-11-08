// ProcMemoryNode.hpp — /proc/self/mem and /proc/self/pagemap over the guest's address space.
//
// mem: the offset is a guest virtual address; reads and writes go through guest memory (EIO where
// nothing is mapped, as the kernel does). pagemap: one 64-bit entry per 4 KiB page, at offset
// (address / 4096) * 8. An entry has bit 63 (present) for a mapped page, bit 61 for a file-backed
// one and bit 56 (exclusively mapped) for an anonymous one; the page frame number (bits 0-54)
// stays 0, as it does for a process without CAP_SYS_ADMIN. Programs use these to check that
// their own code and data are what they expect.
#pragma once
#include "System/VFS/Nodes/Node.hpp"
#include "System/VFS/VFS.hpp"
#include <functional>

namespace arm64emu::yggdrasil {

class ProcMemoryNode : public Node {
public:
    enum class Kind { Mem, Pagemap };
    using Reader = std::function<bool(uint64_t addr, void* buf, size_t n)>;
    using Writer = std::function<bool(uint64_t addr, const void* buf, size_t n)>;
    // One byte per page from `first_page`: 0 unmapped, 1 anonymous, 2 file-backed.
    using PageKinds = std::function<void(uint64_t first_page, size_t count, uint8_t* kinds)>;
    ProcMemoryNode(Kind kind, int flags, Reader read, Writer write, PageKinds page_kinds)
        : kind_(kind), flags_(flags), read_(std::move(read)), write_(std::move(write)),
          page_kinds_(std::move(page_kinds)) {}
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int fstat(struct stat* st) override;
    int flags() const override { return flags_; }
    void set_flags(int f) override { flags_ = f; }
private:
    Kind kind_;
    int flags_;
    Reader read_;
    Writer write_;
    PageKinds page_kinds_;
    uint64_t pos_ = 0;
};

}  // namespace arm64emu::yggdrasil
