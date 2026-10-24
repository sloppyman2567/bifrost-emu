// ProcMemoryNode.cpp — see ProcMemoryNode.hpp.
#include "System/VFS/Nodes/ProcMemoryNode.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace arm64emu::yggdrasil {

namespace {
constexpr uint64_t PAGE = 4096;
constexpr uint64_t ADDRESS_LIMIT = 1ull << 48;
constexpr uint64_t PM_PRESENT = 1ull << 63, PM_FILE = 1ull << 61, PM_EXCLUSIVE = 1ull << 56;
}  // namespace

uint64_t ProcMemoryNode::entry_for_page(const std::vector<Yggdrasil::MapEntry>& maps, uint64_t page) const {
    const uint64_t address = page * PAGE;
    auto it = std::upper_bound(maps.begin(), maps.end(), address,
                               [](uint64_t a, const Yggdrasil::MapEntry& m) { return a < m.start; });
    if (it == maps.begin()) return 0;
    --it;
    if (address >= it->end) return 0;
    const bool file_backed = it->inode != 0 || (!it->label.empty() && it->label[0] == '/');
    return PM_PRESENT | (file_backed ? PM_FILE : PM_EXCLUSIVE);
}

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
        if (at >= (ADDRESS_LIMIT / PAGE) * 8) return 0;
        const auto maps = maps_ ? maps_() : std::vector<Yggdrasil::MapEntry>{};
        while (static_cast<size_t>(done) < n && at + done < (ADDRESS_LIMIT / PAGE) * 8) {
            const uint64_t byte = at + done;
            const uint64_t entry = entry_for_page(maps, byte / 8);
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
