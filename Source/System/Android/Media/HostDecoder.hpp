#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace arm64emu::yggdrasil {
// Host-owned buffers and opaque decoder state never contain guest addresses.
struct DecodedVideo {
    uint32_t width = 0, height = 0;
    uint64_t pts = 0;
    uint32_t primaries = 0, range = 0, transfer = 0, colorspace = 0;
    std::vector<uint8_t> yuv;
};
class HostDecoder {
public:
    virtual ~HostDecoder() = default;
    virtual int decode(const uint8_t*, size_t, uint64_t) = 0;
    virtual int drain() = 0;
    virtual int image(DecodedVideo&) = 0;
    virtual const char* backend() const = 0;
    static std::unique_ptr<HostDecoder> create(unsigned codec, unsigned width, unsigned height);
    static bool available();
    // Native decoder probes only; realm/service and Surface readiness are
    // reported by their owners rather than inferred from host library presence.
    static std::string capabilities_json();
    static unsigned routing_mask();
};
}
