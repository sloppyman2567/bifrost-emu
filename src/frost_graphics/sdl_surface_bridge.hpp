#pragma once
#if defined(BIFROST_THUNK_HAVE_SDL2)
#include "core/memory.h"
#include "core/cpu.h"
#include <SDL2/SDL.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace arm64emu {
// SDL_Surface and SDL_PixelFormat are public structs, not opaque cookies.
// Keep host objects private and publish copies containing guest pointers.
class SdlSurfaceBridge {
public:
    ~SdlSurfaceBridge();
    uint64_t dispatch(Memory& mem, CPU& cpu, const std::string& name);
    void forget_window(Memory& mem, uint64_t window);
    void clear(Memory& mem);
private:
    struct Surface {
        SDL_Surface* host = nullptr;
        uint64_t meta = 0, pixels = 0, owned_pixels = 0, window = 0;
        size_t pixel_bytes = 0;
        // From-surfaces retain their pixel pointer for their entire lifetime.
        std::vector<uint8_t> external_host_pixels;
    };
    static constexpr size_t kMetaBytes = 4096;
    static constexpr size_t kFormatOff = 128, kPaletteOff = 192, kColorsOff = 224;
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<Surface>> surfaces_;
    Surface* lookup(uint64_t guest);
    SDL_PixelFormat* format(uint64_t guest);
    void push(Memory& mem, Surface& s);
    void publish(Memory& mem, Surface& s, bool write_pixels);
    uint64_t wrap(Memory& mem, SDL_Surface* host, uint64_t window = 0,
                  uint64_t external_guest = 0, std::vector<uint8_t> external = {});
    void erase(Memory& mem, uint64_t guest, bool free_host);
};
} // namespace arm64emu
#endif
