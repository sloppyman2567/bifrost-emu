#include "System/Android/Binder/BinderNode.hpp"
#include "System/Android/Binder/Parcel.hpp"
#include "System/Memory/Memory.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "Bifrost/AndroidGralloc.h"
#include <sys/mman.h>
#include <array>
#include <cstring>
#include <stdexcept>

namespace arm64emu::yggdrasil {
namespace {
constexpr const char *DESCRIPTOR = "android.hardware.graphics.allocator.IAllocator";
constexpr uint32_t STATUS_CODE = 0x08;
struct AllocationFailure { int code; };
struct Description {
    std::array<int32_t, BF_GRALLOC_INTS> info{};
    std::array<char, 128> name{};
    bool supported = false;
};
Description read_description(AndroidParcelReader &input) {
    if (input.read<int32_t>() != 1) throw AllocationFailure{0};
    size_t start = input.position();
    int32_t size = input.read<int32_t>();
    // Frozen V2 descriptor has a fixed byte[128], four ints, two longs and
    // the additionalOptions array. Future fields are skipped by its size.
    if (size < 172 || size > 65536) throw AllocationFailure{0};
    if (input.read<int32_t>() != 128) throw AllocationFailure{0};
    Description result;
    for (char &c : result.name) c = input.read<char>();
    result.name.back() = 0;
    int width = input.read<int32_t>(), height = input.read<int32_t>();
    int layers = input.read<int32_t>(), format = input.read<int32_t>();
    uint64_t usage = input.read<uint64_t>(), reserved = input.read<uint64_t>();
    int32_t options = input.read<int32_t>();
    if (options < 0 || options > 64 || input.position() > start + size) throw AllocationFailure{0};
    // Reject vendor options; do not pretend they have been applied.
    result.supported = !options && bf_gralloc_layout(width, height, layers, format, usage, reserved, result.info.data());
    while (input.position() < start + size) input.read<uint8_t>();
    return result;
}
std::shared_ptr<Node> allocate(const Description &description) {
    int fd = memfd_create("bifrost-gralloc", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) throw AllocationFailure{1};
    auto file = std::make_shared<HostNode>(fd, O_RDWR);
    bf_gralloc_header header{};
    std::memcpy(header.info, description.info.data(), sizeof(header.info));
    std::memcpy(header.name, description.name.data(), sizeof(header.name));
    uint64_t size = bf_gralloc_file_size(header.info);
    if (ftruncate(fd, size) || pwrite(fd, &header, sizeof(header), 0) != sizeof(header)
            || fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
        throw AllocationFailure{1};
    return file;
}
void patch_size(AndroidParcelWriter &out, size_t start) {
    int32_t size = out.bytes.size() - start;
    std::memcpy(out.bytes.data() + start, &size, sizeof(size));
}
} // namespace

int BinderNode::allocator_transaction(const bf_binder_transaction &tx, Memory &mem, int tid) {
    // Reserve the largest supported batch (16 handles) before allocating any
    // storage. Queued replies retain their fds until delivery or disconnection.
    uint64_t free_bytes = 0;
    for (const auto &range : free_) free_bytes += range.second;
    if (!mapping_ || memory_ != &mem || tx.flags & BF_TF_ONE_WAY ||
            replies_[tid].size() + 2 > 256 || queued_bytes_ > free_bytes ||
            4096 > free_bytes - queued_bytes_) {
        fail(tid, BF_BR_FAILED_REPLY, ENOMEM);
        return 0;
    }
    Event reply(BF_BR_REPLY);
    reply.transaction.code = tx.code;
    AndroidParcelWriter out;
    std::vector<uint64_t> offsets;
    try {
        if (tx.offsets_size) throw std::invalid_argument("allocator arguments contain objects");
        std::vector<uint8_t> data(tx.data_size);
        if (!data.empty()) mem.read(tx.buffer, data.data(), data.size());
        AndroidParcelReader input(data);
        if (tx.code == 0x5f504e47) {
            input.finish(); out.write<int32_t>(0); reply.transaction.flags = STATUS_CODE;
        } else if (tx.code == 0x5f4e5446) {
            input.finish(); out.ascii_string(DESCRIPTOR);
        } else {
            input.read<int32_t>(); input.read<int32_t>();
            if (input.read<uint32_t>() != 0x53595354 || input.ascii_string(127) != DESCRIPTOR)
                throw std::invalid_argument("allocator interface token");
            switch (tx.code) {
            case 4: case 16777214: case 16777215:
                input.finish(); out.write<int32_t>(0);
                if (tx.code == 16777215) out.write<int32_t>(2);
                else out.ascii_string(tx.code == 4 ? "bifrost" : "9499fec09c544e9de5be3c87125721600f8ade66");
                break;
            case 1: { // mapper4 descriptors are not a mapper5 wire format
                int32_t length = input.read<int32_t>();
                if (length < 0 || length > 65536) throw AllocationFailure{0};
                for (int32_t i = 0; i < (length + 3) / 4; ++i) input.read<int32_t>();
                input.read<int32_t>(); input.finish();
                throw AllocationFailure{2};
            }
            case 2: case 3: {
                Description description = read_description(input);
                int32_t count = tx.code == 2 ? input.read<int32_t>() : 0;
                input.finish();
                if (tx.code == 3) {
                    out.write<int32_t>(0); out.write<int32_t>(description.supported); break;
                }
                if (count < 1 || count > 16) throw AllocationFailure{0};
                if (!description.supported) throw AllocationFailure{2};
                if (!(tx.flags & BF_TF_ACCEPT_FDS)) throw std::invalid_argument("caller rejects allocator fds");
                std::vector<std::shared_ptr<Node>> files;
                for (int32_t i = 0; i < count; ++i) files.push_back(allocate(description));
                out.write<int32_t>(0); // no exception
                out.write<int32_t>(1); // AllocationResult present
                size_t result_start = out.bytes.size(); out.write<int32_t>(0);
                out.write(description.info[BF_STRIDE]); out.write(count);
                for (const auto &file : files) {
                    out.write<int32_t>(1); // NativeHandle present
                    size_t handle_start = out.bytes.size(); out.write<int32_t>(0);
                    out.write<int32_t>(1); // fds length
                    out.write<int32_t>(1); // ParcelFileDescriptor present
                    out.write<int32_t>(0); // no comm fd
                    offsets.push_back(out.bytes.size());
                    out.write(bf_flat_binder{BF_BINDER_TYPE_FD, 0, 0, 0});
                    reply.descriptors.push_back({offsets.back() + 8, file});
                    out.write<int32_t>(BF_GRALLOC_INTS);
                    for (int32_t value : description.info) out.write(value);
                    patch_size(out, handle_start);
                }
                patch_size(out, result_start);
                break;
            }
            default:
                out.write<int32_t>(-EBADMSG); reply.transaction.flags = STATUS_CODE;
            }
        }
    } catch (const AllocationFailure &error) {
        out.bytes.clear(); reply.descriptors.clear(); offsets.clear();
        out.write<int32_t>(-8); // EX_SERVICE_SPECIFIC (Parcel/Status)
        out.ascii_string("Software buffer allocation failed");
        out.write<int32_t>(0); // no remote stack trace
        out.write<int32_t>(error.code);
    } catch (const std::invalid_argument &) {
        out.bytes.clear(); reply.descriptors.clear(); offsets.clear();
        out.write<int32_t>(-EINVAL); reply.transaction.flags = STATUS_CODE;
    }
    reply.transaction.data_size = out.bytes.size();
    reply.transaction.offsets_size = offsets.size() * sizeof(uint64_t);
    if (!offsets.empty()) {
        while (out.bytes.size() & 7) out.write<uint8_t>(0);
        for (uint64_t offset : offsets) out.write(offset);
    }
    reply.payload = std::move(out.bytes);
    queued_bytes_ += std::max<uint64_t>(8, (reply.payload.size() + 7) & ~uint64_t(7));
    replies_[tid].push_back(Event{BF_BR_TRANSACTION_COMPLETE});
    replies_[tid].push_back(std::move(reply));
    ready_.notify_all();
    return 0;
}
} // namespace arm64emu::yggdrasil
