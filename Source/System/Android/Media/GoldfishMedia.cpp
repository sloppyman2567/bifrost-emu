#include "System/Android/Media/GoldfishMedia.hpp"
#include "System/Android/Media/HostDecoder.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/DebugFlags.hpp"
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <sys/mman.h>
#include <sys/ioctl.h>
namespace arm64emu::yggdrasil {
namespace {
struct Allocate { uint64_t size, offset, physical; };
struct Ping { uint64_t offset, size, metadata; uint32_t resource, wait_fd, wait_flags, direction; };
static_assert(sizeof(Allocate) == 24 && sizeof(Ping) == 40);
constexpr uint32_t ALLOCATE = _IOWR('G', 10, Allocate);
constexpr uint32_t DEALLOCATE = _IOWR('G', 11, uint64_t);
constexpr uint32_t PING = _IOWR('G', 12, Ping);
constexpr uint64_t MAX_ARENA = 64 * 1024 * 1024;
struct Decoder {
    unsigned codec;
    std::unique_ptr<HostDecoder> host;
    std::optional<DecodedVideo> pending;
};
class GoldfishMedia final : public Node {
    int fd_, flags_;
    uint64_t size_ = 0, next_handle_ = 1;
    uint8_t* mapping_ = nullptr;
    bool media_ = false;
    std::mutex mutex_;
    // VP8 and VP9's guest modules each allocate their own context IDs. The
    // protocol carries the codec family as part of every operation's identity.
    std::map<std::pair<unsigned, uint64_t>, Decoder> decoders_;
    bool range(uint64_t offset, uint64_t length) const { return offset <= size_ && length <= size_ - offset; }
    template<class T> void put(uint64_t offset, T value) { std::memcpy(mapping_ + offset, &value, sizeof(value)); }
    int operation(const Ping& ping) {
        unsigned type = ping.metadata >> 56, op = ping.metadata & 255;
        uint64_t slot = ((ping.metadata >> 8) & 31) << 20;
        if (ping.offset || (ping.metadata & UINT64_C(0x00ffffffffffe000)) || type >= 4 || op > 6
                || !range(slot, 4096)) return -EINVAL;
        // Snapshot all parameters before returning anything through this arena.
        uint64_t p[32]; std::memcpy(p, mapping_ + slot, sizeof(p));
        const uint64_t result = slot + 256;
        std::memset(mapping_ + result, 0, 128);
        const bool vpx = type < 2;
        if (op == 0) {
            if (decoders_.size() >= 32) return -ENOSPC;
            if ((!vpx && (p[0] != 100 && p[0] != 200)) || (vpx && !p[0])) return -EINVAL;
            uint64_t handle = vpx ? p[0] : next_handle_++;
            if (!handle || decoders_.count({type, handle})) return -EINVAL;
            unsigned width = vpx ? 0 : p[1], height = vpx ? 0 : p[2];
            if (!vpx && (p[1] > 4096 || p[2] > 4096 || p[3] > 4096 || p[4] > 4096 || p[5] != 0)) return -EINVAL;
            auto host = HostDecoder::create(type, width, height);
            if (!host) return -ENODEV;
            decoders_.emplace(std::make_pair(type, handle), Decoder{type, std::move(host), {}});
            if (!vpx) put<uint64_t>(result, handle);
            return 0;
        }
        auto found = decoders_.find({type, p[0]});
        if (found == decoders_.end() || found->second.codec != type) return -ENOENT;
        auto& decoder = found->second;
        if (op == 1) { decoders_.erase(found); return 0; }
        if (op == 5) {
            if (p[1] > 4096 || p[2] > 4096) return -EINVAL;
            auto host = HostDecoder::create(type, p[1], p[2]);
            if (!host) return -ENODEV;
            decoder.host = std::move(host); decoder.pending.reset(); return 0;
        }
        if (op == 6) return 0; // Output color metadata comes from the decoded bitstream.
        if (op == 4) return decoder.host->drain();
        if (op == 2) {
            if (p[1] > size_ - slot || p[2] > 16 * 1024 * 1024 || !range(slot + p[1], p[2])) return -EINVAL;
            std::vector<uint8_t> packet(p[2]);
            if (!packet.empty()) std::memcpy(packet.data(), mapping_ + slot + p[1], packet.size());
            int error = decoder.host->decode(packet.data(), packet.size(), p[3]);
            if (dbg().media_trace) fprintf(stderr, "[media] input handle=%llu bytes=%zu pts=%llu result=%d\n",
                    (unsigned long long)p[0], packet.size(), (unsigned long long)p[3], error);
            if (!vpx) { put<uint64_t>(result, p[2]); put<int32_t>(result + 8, error); }
            else put<int32_t>(result, error);
            return 0;
        }
        // A host color-buffer ID is not a Bifrost gralloc/native-window handle.
        // Preserve the frame on unsupported output or insufficient shared memory.
        if ((!vpx && p[2] != UINT64_MAX) || (vpx && static_cast<int64_t>(p[6]) > 0)) {
            put<int32_t>(result, -ENOTSUP); return 0;
        }
        if (!decoder.pending) {
            DecodedVideo video;
            int error = decoder.host->image(video);
            if (error) { put<int32_t>(result, error); return 0; }
            decoder.pending.emplace(std::move(video));
        }
        auto& image = *decoder.pending;
        if (dbg().media_trace) fprintf(stderr, "[media] image handle=%llu %ux%u pts=%llu\n",
                (unsigned long long)p[0], image.width, image.height, (unsigned long long)image.pts);
        const uint64_t output = vpx ? p[7] : p[1];
        if (output > size_ - slot) return -EINVAL;
        if (!vpx) {
            if (!range(slot + output, image.yuv.size())) { put<int32_t>(result, -ENOSPC); return 0; }
            std::memcpy(mapping_ + slot + output, image.yuv.data(), image.yuv.size());
            put<uint32_t>(result + 8, image.width); put<uint32_t>(result + 16, image.height);
            put<uint64_t>(result + 24, image.pts);
            put<uint32_t>(result + 32, image.primaries); put<uint32_t>(result + 40, image.range);
            put<uint32_t>(result + 48, image.transfer); put<uint32_t>(result + 56, image.colorspace);
        } else {
            // Like the emulator's MediaVpxDecoderGeneric::getImage, write a
            // packed frame at the decoded size. The guest's output
            // width/height are its stale expectation: it learns d_w/d_h from
            // this reply and then reads the frame with those strides. The
            // pinned C2GoldfishVpxDec (android-15.0.0_r36 source, matching the
            // provisioned binary) reads it as YV12: Y, then V, then U.
            if (!range(slot + output, image.yuv.size())) { put<int32_t>(result, -ENOSPC); return 0; }
            const size_t luma = size_t(image.width) * image.height;
            const size_t chroma = (image.yuv.size() - luma) / 2;
            auto dest = mapping_ + slot + output;
            std::memcpy(dest, image.yuv.data(), luma);
            std::memcpy(dest + luma, image.yuv.data() + luma + chroma, chroma);
            std::memcpy(dest + luma + chroma, image.yuv.data() + luma, chroma);
            put<uint32_t>(result + 8, 0x102); // VPX_IMG_FMT_I420
            put<uint32_t>(result + 16, image.width); put<uint32_t>(result + 24, image.height);
            put<uint64_t>(result + 32, image.pts);
        }
        put<int32_t>(result, 0);
        decoder.pending.reset();
        return 0;
    }
public:
    GoldfishMedia(int fd, int flags) : fd_(fd), flags_(flags) {}
    ~GoldfishMedia() override { if (mapping_) ::munmap(mapping_, size_); ::close(fd_); }
    ssize_t read(uint64_t, void*, size_t) override { return -EINVAL; }
    ssize_t write(uint64_t, const void*, size_t) override { return -EINVAL; }
    ssize_t lseek(int64_t, int) override { return -ESPIPE; }
    bool seekable() const override { return false; }
    int flags() const override { return flags_; }
    int host_fd() const override { return fd_; }
    int fstat(struct stat* value) override {
        int result = ::fstat(fd_, value); if (result) return -errno;
        value->st_mode = S_IFCHR | 0600; return 0;
    }
    int ioctl(uint32_t request, uint64_t address, Memory& memory) override {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            if (request == ALLOCATE) {
                auto block = memory.load<Allocate>(address);
                if (size_ || block.size < 4096 || block.size > MAX_ARENA || block.offset || block.physical) return -EINVAL;
                memory.check_access(address, sizeof(block), Memory::GUEST_PROT_WRITE);
                if (::ftruncate(fd_, block.size)) return -errno;
                void* mapped = ::mmap(nullptr, block.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
                if (mapped == MAP_FAILED) return -errno;
                if (::fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)) { int error = errno; ::munmap(mapped, block.size); return -error; }
                mapping_ = static_cast<uint8_t*>(mapped); size_ = block.size;
                block.offset = 0; block.physical = 4096; // opaque virtual-device address, never a host pointer
                memory.store(address, block); return 0;
            }
            if (request == DEALLOCATE) {
                if (memory.load<uint64_t>(address) || !size_) return -EINVAL;
                decoders_.clear(); ::munmap(mapping_, size_); mapping_ = nullptr; size_ = 0; media_ = false;
                return 0;
            }
            if (request != PING) return -ENOTTY;
            auto ping = memory.load<Ping>(address);
            if (!mapping_) return -EINVAL;
            if (!media_) { if (ping.metadata != 1 || ping.offset) return -ENOTSUP; media_ = true; return 0; }
            int result = operation(ping);
            if (result && dbg().media_trace) fprintf(stderr, "[media] operation=%#llx result=%d\n",
                    (unsigned long long)ping.metadata, result);
            return result;
        } catch (const EmuError&) { return -EFAULT; }
          catch (const std::bad_alloc&) { return -ENOMEM; }
    }
};
}
std::unique_ptr<Node> open_goldfish_media(int flags, int* error) {
    if ((flags & O_ACCMODE) != O_RDWR || !HostDecoder::available()) { *error = -ENODEV; return nullptr; }
    int fd = ::memfd_create("bifrost-codec-media", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { *error = -errno; return nullptr; }
    try { return std::make_unique<GoldfishMedia>(fd, flags); }
    catch (const std::bad_alloc&) { ::close(fd); *error = -ENOMEM; return nullptr; }
}
}
