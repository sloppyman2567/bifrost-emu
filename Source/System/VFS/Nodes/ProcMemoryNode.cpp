// ProcMemoryNode.cpp — see ProcMemoryNode.hpp.
#include "System/VFS/Nodes/ProcMemoryNode.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace arm64emu::yggdrasil {

namespace {
constexpr uint64_t PAGE = 4096;
constexpr uint64_t ADDRESS_LIMIT = 1ull << 48;
constexpr uint64_t PM_PRESENT = 1ull << 63, PM_FILE = 1ull << 61, PM_EXCLUSIVE = 1ull << 56;
}  // namespace

ssize_t ProcMemoryNode::read(uint64_t off, void* buf, size_t n) {
    const bool sequential = off == static_cast<uint64_t>(-1);
    uint64_t at = sequential ? pos_ : off;
    ssize_t done = 0;
    if (kind_ == Kind::Mem) {
        if (at >= ADDRESS_LIMIT) return 0;
        size_t chunk = std::min<size_t>(n, ADDRESS_LIMIT - at);
        while (static_cast<size_t>(done) < chunk) {
            // Stop at the first page that cannot be read; the bytes before it still count.
            const size_t step = std::min<size_t>(chunk - done, PAGE - ((at + done) & (PAGE - 1)));
            if (!read_ || !read_(at + done, static_cast<uint8_t*>(buf) + done, step)) break;
            done += static_cast<ssize_t>(step);
        }
        if (done == 0 && n > 0) return -EIO;
    } else {
        if (at >= (ADDRESS_LIMIT / PAGE) * 8 || n == 0) return 0;
        // Classify the pages the read covers in one call: a scan reads one entry at a time.
        const uint64_t limit = std::min<uint64_t>(at + n, (ADDRESS_LIMIT / PAGE) * 8);
        const uint64_t first = at / 8, pages = (limit - 1) / 8 - first + 1;
        std::vector<uint8_t> kinds(pages, 0);
        if (page_kinds_) page_kinds_(first, pages, kinds.data());
        while (static_cast<size_t>(done) < n && at + done < (ADDRESS_LIMIT / PAGE) * 8) {
            const uint64_t byte = at + done;
            const uint8_t kind = kinds[byte / 8 - first];
            const uint64_t entry = kind == 0 ? 0 : PM_PRESENT | (kind == 2 ? PM_FILE : PM_EXCLUSIVE);
            const size_t skip = byte % 8, take = std::min<size_t>(8 - skip, n - done);
            std::memcpy(static_cast<uint8_t*>(buf) + done, reinterpret_cast<const uint8_t*>(&entry) + skip, take);
            done += static_cast<ssize_t>(take);
        }
    }
    if (sequential) pos_ += static_cast<uint64_t>(done);
    return done;
}

ssize_t ProcMemoryNode::write(uint64_t off, const void* buf, size_t n) {
    if (kind_ != Kind::Mem || (flags_ & O_ACCMODE) == O_RDONLY) return -EBADF;
    const bool sequential = off == static_cast<uint64_t>(-1);
    uint64_t at = sequential ? pos_ : off;
    if (at >= ADDRESS_LIMIT) return -EIO;
    ssize_t done = 0;
    const size_t chunk = std::min<size_t>(n, ADDRESS_LIMIT - at);
    while (static_cast<size_t>(done) < chunk) {
        const size_t step = std::min<size_t>(chunk - done, PAGE - ((at + done) & (PAGE - 1)));
        if (!write_ || !write_(at + done, static_cast<const uint8_t*>(buf) + done, step)) break;
        done += static_cast<ssize_t>(step);
    }
    if (done == 0 && n > 0) return -EIO;
    if (sequential) pos_ += static_cast<uint64_t>(done);
    return done;
}

ssize_t ProcMemoryNode::lseek(int64_t off, int whence) {
    int64_t base = 0;
    if (whence == SEEK_CUR) base = static_cast<int64_t>(pos_);
    else if (whence == SEEK_END) base = static_cast<int64_t>(kind_ == Kind::Mem ? ADDRESS_LIMIT : (ADDRESS_LIMIT / PAGE) * 8);
    else if (whence != SEEK_SET) return -EINVAL;
    if (base + off < 0) return -EINVAL;
    pos_ = static_cast<uint64_t>(base + off);
    return static_cast<ssize_t>(pos_);
}

int ProcMemoryNode::fstat(struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | (kind_ == Kind::Mem ? 0600 : 0400);
    st->st_nlink = 1;
    return 0;
}

}  // namespace arm64emu::yggdrasil
