// AndroidBufferImages.hpp — EGL_NATIVE_BUFFER_ANDROID images over Bifrost
// gralloc buffers (CPU-linear sealed memfds, Include/Bifrost/AndroidGralloc.h).
// The guest adapter validates the ANativeWindowBuffer and holds its
// reference; this host side maps the buffer, gives each GL context a host
// texture with a host EGLImage on it, and refreshes the texture from the
// buffer on every glEGLImageTargetTexture2DOES, where HWUI's update proc and
// libgui's GLConsumer rebind each new frame.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
namespace arm64emu {
class CPU;
class Memory;
struct SymbolEntry;

class AndroidBufferImages {
public:
    ~AndroidBufferImages();
    // eglCreateImage[KHR] for EGL_NATIVE_BUFFER_ANDROID, and
    // eglDestroyImage[KHR] and glEGLImageTarget*OES on those images.
    std::optional<int64_t> dispatch(CPU& cpu, const SymbolEntry& entry, Memory& mem);
    // A destroyed host context's texture names went with it.
    void forget_context(uint64_t context);

private:
    struct ContextImage {
        void* display = nullptr;
        uint32_t texture = 0;
        void* image = nullptr;  // host EGLImage on `texture`
    };
    struct Buffer {
        const uint8_t* mapping = nullptr;
        uint8_t* writable = nullptr;  // the same mapping when the buffer could be mapped read/write
        size_t size = 0;
        int32_t width = 0, height = 0, stride = 0, format = 0;
        std::unordered_map<uint64_t, ContextImage> contexts;
    };
    uint64_t create(Memory& mem, uint64_t buffer, uint64_t attribs, bool wide);
    bool destroy(uint64_t image);
    void bind(uint32_t target, uint64_t image, bool renderbuffer);
    // glTexSubImage2D into a texture whose storage is one of these images: the pixels also go to the buffer's memory.
    void capture_upload(CPU& cpu, Memory& mem);
    void forget_textures(CPU& cpu, Memory& mem);  // glDeleteTextures
    void forget_bound_texture();                 // glTexImage2D on the bound texture
    // Converts to tightly packed RGBA8 in `out`: one scratch buffer shared by every image, not a copy per image.
    void convert(Buffer& buffer, std::vector<uint8_t>& out);

    std::mutex mu_;
    // Per context, the texture names (as the app sees them) bound to an image, to find the buffer an upload targets.
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> textures_;
    std::unordered_map<uint64_t, Buffer> buffers_;  // keyed by guest image handle
    std::vector<uint8_t> scratch_;
    // Textures of images destroyed while another context (or none) was current: they can only be deleted
    // in their own context, so they wait for its next bind. Keyed by host context.
    std::unordered_map<uint64_t, std::vector<uint32_t>> stale_textures_;
    uint64_t next_ = 0;
};
}  // namespace arm64emu
