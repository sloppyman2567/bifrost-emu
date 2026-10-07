#include "yggdrasil/uffd_node.hpp"
#include "core/memory.h"
#include <cstring>
#include <vector>

namespace arm64emu::yggdrasil {
namespace {
// include/uapi/linux/userfaultfd.h: _IOWR/_IOR(0xAA, nr, struct).
constexpr uint32_t ioc(uint32_t dir, uint32_t nr, uint32_t size) {
    return dir << 30 | size << 16 | 0xAAu << 8 | nr;
}
constexpr uint32_t UFFDIO_API = ioc(3, 0x3F, 24);
constexpr uint32_t UFFDIO_REGISTER = ioc(3, 0x00, 32);
constexpr uint32_t UFFDIO_UNREGISTER = ioc(2, 0x01, 16);
constexpr uint32_t UFFDIO_WAKE = ioc(2, 0x02, 16);
constexpr uint32_t UFFDIO_COPY = ioc(3, 0x03, 40);
constexpr uint32_t UFFDIO_ZEROPAGE = ioc(3, 0x04, 32);
constexpr uint64_t UFFD_API = 0xAA;
constexpr uint64_t INITIALIZED = 1ull << 63;  // private, never reported
constexpr uint64_t REGISTER_MODE_MISSING = 1, REGISTER_MODE_WP = 2,
                   REGISTER_MODE_MINOR = 4;
constexpr uint64_t COPY_MODE_DONTWAKE = 1, COPY_MODE_WP = 2;
// UFFD_API_IOCTLS: register, unregister and API.
constexpr uint64_t API_IOCTLS = 1ull << 0x00 | 1ull << 0x01 | 1ull << 0x3F;
// The range ioctls implemented here: wake, copy and zeropage.
constexpr uint64_t RANGE_IOCTLS = 1ull << 0x02 | 1ull << 0x03 | 1ull << 0x04;
constexpr uint64_t PAGE = Memory::PAGE_SIZE;
constexpr uint64_t TASK_SIZE = 1ull << 48;  // AArch64 48-bit user space
constexpr uint64_t MMAP_MIN_ADDR = 4096;

// fs/userfaultfd.c validate_unaligned_range / validate_range.
int validate_unaligned(uint64_t start, uint64_t len) {
    if ((len & (PAGE - 1)) || !len || start < MMAP_MIN_ADDR || start >= TASK_SIZE ||
        len > TASK_SIZE - start)
        return -EINVAL;
    return 0;
}
int validate(uint64_t start, uint64_t len) {
    return (start & (PAGE - 1)) ? -EINVAL : validate_unaligned(start, len);
}
} // namespace

ssize_t UffdNode::read(uint64_t, void*, size_t) {
    if (!features_.load(std::memory_order_acquire)) return -EINVAL;
    // SIGBUS mode queues no fault events; event delivery is not implemented.
    return -EAGAIN;
}

int UffdNode::fstat(struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IRUSR | S_IWUSR;  // anon_inode: no file type bits
    st->st_nlink = 1;
    st->st_blksize = PAGE;
    return 0;
}

int UffdNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    try {
        if (request == UFFDIO_API) return api(argp, mem);
        if (!features_.load(std::memory_order_acquire)) return -EINVAL;
        switch (request) {
            case UFFDIO_REGISTER: return register_range(argp, mem, true);
            case UFFDIO_UNREGISTER: return register_range(argp, mem, false);
            case UFFDIO_WAKE: {
                // No thread ever waits in SIGBUS mode; only validate.
                uint64_t range[2];
                mem.read(argp, range, sizeof(range));
                return validate(range[0], range[1]);
            }
            case UFFDIO_COPY: return fill(argp, mem, true);
            case UFFDIO_ZEROPAGE: return fill(argp, mem, false);
            default: return -EINVAL;
        }
    } catch (const UnmappedMemory&) {
        return -EFAULT;
    }
}

int UffdNode::api(uint64_t argp, Memory& mem) {
    uint64_t api[3];  // api, features, ioctls
    mem.read(argp, api, sizeof(api));
    const uint64_t requested = api[1];
    auto fail = [&](int error) {
        std::memset(api, 0, sizeof(api));
        mem.write(argp, api, sizeof(api));
        return error;
    };
    if (api[0] != UFFD_API || (requested & ~FEATURES)) return fail(-EINVAL);
    api[1] = FEATURES;
    api[2] = API_IOCTLS;
    mem.write(argp, api, sizeof(api));
    uint64_t expected = 0;
    if (!features_.compare_exchange_strong(expected, requested | INITIALIZED,
                                           std::memory_order_acq_rel))
        return fail(-EINVAL);  // UFFDIO_API succeeds once per descriptor
    return 0;
}

int UffdNode::register_range(uint64_t argp, Memory& mem, bool registered) {
    if (!registered) {
        uint64_t range[2];
        mem.read(argp, range, sizeof(range));
        if (int error = validate(range[0], range[1])) return error;
        return mem.uffd_register(range[0], range[1], false);
    }
    uint64_t reg[3];  // start, len, mode (ioctls is output only)
    mem.read(argp, reg, sizeof(reg));
    const uint64_t mode = reg[2];
    if (!mode || (mode & ~(REGISTER_MODE_MISSING | REGISTER_MODE_WP | REGISTER_MODE_MINOR)))
        return -EINVAL;
    // Write-protect and minor faults are not implemented.
    if (mode & (REGISTER_MODE_WP | REGISTER_MODE_MINOR)) return -EINVAL;
    if (int error = validate(reg[0], reg[1])) return error;
    // Missing faults are delivered only as SIGBUS here.
    if (!(features_.load(std::memory_order_acquire) & FEATURES)) return -EINVAL;
    if (int error = mem.uffd_register(reg[0], reg[1], true)) return error;
    const uint64_t ioctls = RANGE_IOCTLS;
    mem.write(argp + 24, &ioctls, sizeof(ioctls));
    return 0;
}

int UffdNode::fill(uint64_t argp, Memory& mem, bool copy) {
    // uffdio_copy {dst, src, len, mode, copy} / uffdio_zeropage
    // {start, len, mode, zeropage}: the result field is written back.
    uint64_t args[4] = {};
    mem.read(argp, args, copy ? 32 : 24);
    const uint64_t dst = args[0], src = copy ? args[1] : 0;
    const uint64_t len = copy ? args[2] : args[1], mode = copy ? args[3] : args[2];
    const uint64_t result_at = argp + (copy ? 32 : 24);
    if (copy) {
        if (int error = validate_unaligned(src, len)) return error;
    }
    if (int error = validate(dst, len)) return error;
    if (mode & ~(copy ? (COPY_MODE_DONTWAKE | COPY_MODE_WP) : COPY_MODE_DONTWAKE))
        return -EINVAL;
    int64_t result;
    if (copy && (mode & COPY_MODE_WP)) {
        result = -EINVAL;  // mfill_atomic: WP copy needs a WP registration
    } else if (copy) {
        // Read the source page by page; a fault ends the copy early.
        std::vector<uint8_t> data(len);
        uint64_t readable = 0;
        for (; readable < len; readable += PAGE) {
            try {
                mem.read(src + readable, data.data() + readable, PAGE);
            } catch (const UnmappedMemory&) {
                break;
            }
        }
        result = readable ? mem.uffd_fill(dst, data.data(), readable) : -EFAULT;
    } else {
        result = mem.uffd_fill(dst, nullptr, len);
    }
    mem.write(result_at, &result, sizeof(result));
    if (result < 0) return static_cast<int>(result);
    return static_cast<uint64_t>(result) == len ? 0 : -EAGAIN;
}

} // namespace arm64emu::yggdrasil
