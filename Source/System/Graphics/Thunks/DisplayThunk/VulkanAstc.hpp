// VulkanAstc.hpp — ASTC LDR textures for Vulkan guests on a host GPU without them.
//
// Android engines upload ASTC when the device reports textureCompressionASTC_LDR; desktop AMD and NVIDIA drivers
// do not. When the host lacks ASTC but has BC7 this layer tells the guest ASTC works and keeps every ASTC image as
// a BC7 image on the host: image and view creation use the BC7 format, a buffer-to-image copy is redirected to a
// BC7 scratch buffer, and the ASTC data is decoded and re-encoded (TextureCodec) when the command buffer is
// submitted, when the guest's staging memory is final. Everything else about the image is the host driver's.
//
// Not covered: reading an ASTC image back, copying or blitting one, ASTC as a render target, 3D and HDR ASTC
// (none is offered or used by engines for streamed textures).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace arm64emu {

struct DisplayThunkImpl;

class VulkanAstc {
public:
    // Which commands the dispatcher hands to before()/after(); assigned once per symbol at registration.
    enum Hook : uint8_t {
        kNone = 0, kCreateDevice, kDestroyDevice, kFeatures, kFormatProperties, kFormatProperties2,
        kImageFormatProperties, kImageFormatProperties2, kCreateImage, kDestroyImage, kCreateImageView,
        kBindBufferMemory, kBindBufferMemory2, kDestroyBuffer, kCopyBufferToImage, kCopyBufferToImage2,
        kQueueSubmit, kQueueSubmit2, kAllocateCommandBuffers, kFreeCommandBuffers, kResetCommandBuffer,
        kResetCommandPool, kDestroyCommandPool, kExecuteCommands,
    };
    static uint8_t hook_for(const std::string& name);

    // The ASTC image a vkCreateImage call is making, carried from before() to after().
    struct ImageInfo {
        bool valid = false;
        bool srgb = false;
        uint64_t device = 0;
        uint8_t block_w = 0, block_h = 0;
        uint32_t width = 0, height = 0, mips = 1, layers = 1;
    };
    // Per-call state the dispatcher owns for one thunk call. Made on every call, so it stays small.
    struct Call {
        ImageInfo image;
        uint64_t result = 0;       // returned to the guest when before() asks to skip the host call
    };

    explicit VulkanAstc(DisplayThunkImpl& impl);
    ~VulkanAstc();

    // Runs before the host call, on staged (host-memory) arguments. False: do not call the host; reply with
    // call.result.
    bool before(uint8_t hook, uint64_t* args, Call& call);
    // Runs after the host call.
    void after(uint8_t hook, const uint64_t* args, uint64_t ret, Call& call);

    // A chain node the host just filled for vkGetPhysicalDeviceFeatures2: sets the ASTC bit when emulated.
    void patch_features2(uint64_t physical, uint8_t* node);
    // vkBeginCommandBuffer resets the buffer: its pending transcodes are over.
    void begin_command_buffer(uint64_t cmd);
    // Whether `physical` gets ASTC through BC7 (the host lacks ASTC and has BC7).
    bool emulates(uint64_t physical);

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace arm64emu
