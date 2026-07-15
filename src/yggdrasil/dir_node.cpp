// yggdrasil/dir_node.cpp — DirNode implementation.
//
// Synthesizes linux_dirent64 records for virtual directories (/proc,
// /proc/self, /dev). v1.4.5-alpha: NEW. Previously `ls /proc` and
// `ls /dev` under the guest returned nothing because no Node existed
// for the directories themselves — only for specific files under them.
#include "yggdrasil/dir_node.hpp"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
// linux_dirent64 layout (AArch64):
//   u64  d_ino;
//   s64  d_off;
//   u16  d_reclen;
//   u8   d_type;
//   char d_name[];       // NUL-terminated, padded so reclen % 8 == 0
//
// We synthesize one record per entry. d_ino is a hash of the name
// (deterministic across calls so guests that compare inode numbers
// see stable values). d_off is the byte offset of the NEXT record
// (or 0 for the last, signaling end-of-directory). d_reclen is the
// total record length including padding.
static uint64_t hash_ino(const std::string& name) {
    // FNV-1a 64-bit. Good enough for a fake inode number.
    uint64_t h = 1469598103934665603ULL;
    for (char c : name) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h | 0x8000000000000000ULL;  // set high bit to avoid 0 (reserved)
}
ssize_t DirNode::getdents(uint64_t /*off*/, void* buf, size_t n) {
    // The `off` parameter is ignored — we use the internal pos_ (set by
    // lseek) so that the guest's lseek(fd, d_off, SEEK_SET) + getdents
    // sequence works correctly. The guest's readdir loop:
    //   1. opendir → fd = open("/proc", O_DIRECTORY)
    //   2. readdir → getdents64(fd, buf, n) → returns entries at pos_
    //   3. musl internally does lseek(fd, d_off, SEEK_SET) to advance
    //   4. repeat step 2 until getdents returns 0
    //
    // We advance pos_ by the number of entries emitted. The d_off
    // field in each record is the entry index AFTER this record's
    // entry (idx+1), so the guest's lseek(fd, d_off, SEEK_SET) sets
    // pos_ to idx+1 — which is where the next getdents call picks up.
    size_t idx = pos_;
    if (idx >= entries_.size()) return 0;  // end-of-directory
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t total = 0;
    for (; idx < entries_.size(); idx++) {
        const Entry& e = entries_[idx];
        // Compute record length: name + NUL + padding to 8-byte align,
        // plus the 19-byte fixed header. Round up to multiple of 8.
        size_t name_len = e.name.size() + 1;  // include NUL
        size_t reclen = 19 + name_len;
        reclen = (reclen + 7) & ~size_t(7);  // align to 8
        // If this record doesn't fit, stop here. The guest will call
        // again with a larger buffer (or after consuming what we have).
        if (total + reclen > n) {
            if (total == 0) return -EINVAL;  // buffer too small for one entry
            break;  // return what we have; guest calls again
        }
        // Lay out the record.
        // d_ino (8 bytes) — FNV-1a hash of the full path.
        uint64_t ino = hash_ino(name_ + "/" + e.name);
        memcpy(out + total, &ino, 8);
        // d_off (8 bytes) — entry index of the NEXT record (idx+1).
        // The guest passes this back via lseek(fd, d_off, SEEK_SET).
        uint64_t next_off = idx + 1;
        memcpy(out + total + 8, &next_off, 8);
        // d_reclen (2 bytes)
        uint16_t rl = static_cast<uint16_t>(reclen);
        memcpy(out + total + 16, &rl, 2);
        // d_type (1 byte)
        out[total + 18] = e.type;
        // d_name (NUL-terminated, padded)
        memcpy(out + total + 19, e.name.data(), e.name.size());
        // Zero-fill the padding (name NUL + alignment bytes).
        memset(out + total + 19 + e.name.size(), 0,
               reclen - 19 - e.name.size());
        total += reclen;
    }
    // Advance the internal position by the number of entries emitted.
    pos_ = idx;
    return static_cast<ssize_t>(total);
}
} // namespace arm64emu::yggdrasil
