// yggdrasil/dir_node.hpp — synthetic directory entry.
//
// v1.4.5-alpha: NEW. Previously, `ls /proc` or `ls /dev` under the
// guest returned nothing because there was no Node for the directory
// itself — only for specific files under it. getdents64 fell through
// to the host passthrough, which has no /proc or /dev entries to list.
//
// DirNode holds a list of (name, type) pairs and synthesizes
// linux_dirent64 records on getdents64. The procfs and devfs resolvers
// construct DirNodes for "/proc", "/proc/self", "/dev", and (optionally)
// other virtual directories.
//
// The dirent format is the AArch64 linux_dirent64:
//   struct linux_dirent64 {
//       u64  d_ino;        // inode number (we use a hash of the name)
//       s64  d_off;        // offset to next entry (we use byte offset)
//       u16  d_reclen;     // length of this record
//       u8   d_type;       // DT_DIR / DT_REG / DT_LNK / ...
//       char d_name[];     // NUL-terminated name, padded to 8 bytes
//   };
#pragma once

#include "yggdrasil/node.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

class DirNode : public Node {
public:
    struct Entry {
        std::string name;
        uint8_t     type;   // DT_DIR, DT_REG, DT_LNK, DT_CHR, ...
    };

    // Construct with an explicit entry list. The DirNode tracks its
    // own read position (advanced by getdents, reset by lseek to 0).
    DirNode(std::string name, std::vector<Entry> entries, int flags)
        : name_(std::move(name)), entries_(std::move(entries)),
          flags_(flags), pos_(0) {}

    // Node interface — read/write/lseek return -EISDIR / -ESPIPE.
    ssize_t read(uint64_t, void*, size_t) override { return -EISDIR; }
    ssize_t write(uint64_t, const void*, size_t) override { return -EISDIR; }
    ssize_t lseek(int64_t off, int whence) override {
        // Support SEEK_SET (and SEEK_CUR with off=0) to reset/advance
        // the position. SEEK_END goes to the end (entries_.size()).
        // The guest's readdir loop does lseek(fd, 0, SEEK_SET) before
        // re-reading, and lseek(fd, d_off, SEEK_SET) between getdents
        // calls — we honor both by updating pos_.
        size_t new_pos;
        if (whence == SEEK_SET) {
            new_pos = static_cast<size_t>(off);
        } else if (whence == SEEK_CUR) {
            new_pos = pos_ + static_cast<size_t>(off);
        } else if (whence == SEEK_END) {
            new_pos = entries_.size() + static_cast<size_t>(off);
        } else {
            return -EINVAL;
        }
        if (new_pos > entries_.size()) return -EINVAL;
        pos_ = new_pos;
        return static_cast<ssize_t>(pos_);
    }
    int fstat(struct stat* st) override {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0755;
        st->st_size = static_cast<off_t>(entries_.size());
        st->st_nlink = 2;
        return 0;
    }
    bool is_dir() const override { return true; }
    int  flags() const override { return flags_; }

    // getdents reads from the current pos_ and advances it. The `off`
    // parameter (from the syscall layer) is ignored — we use the
    // internal pos_ so that lseek + getdents work correctly together.
    ssize_t getdents(uint64_t off, void* buf, size_t n) override;

    const std::string& name() const { return name_; }
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::string name_;
    std::vector<Entry> entries_;
    int flags_;
    size_t pos_;  // current read position (entry index)
};

} // namespace arm64emu::yggdrasil
