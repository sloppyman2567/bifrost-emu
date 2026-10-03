#include "sdl_surface_bridge.hpp"
#if defined(BIFROST_THUNK_HAVE_SDL2)
#include <cstring>
#include <stdexcept>

namespace arm64emu {
// Both supported ABIs are little-endian LP64. Assert every public layout
// used for these copies; private host pointers are always cleared below.
static_assert(sizeof(SDL_Surface) == 96 && offsetof(SDL_Surface, pixels) == 32 &&
              offsetof(SDL_Surface, format) == 8 && offsetof(SDL_Surface, w) == 16);
static_assert(sizeof(SDL_PixelFormat) == 56 && offsetof(SDL_PixelFormat, palette) == 8);
static_assert(sizeof(SDL_Palette) == 24 && offsetof(SDL_Palette, colors) == 8);

SdlSurfaceBridge::~SdlSurfaceBridge() {
    for (auto& kv : surfaces_)
        if (!kv.second->window) SDL_FreeSurface(kv.second->host);
}
SdlSurfaceBridge::Surface* SdlSurfaceBridge::lookup(uint64_t guest) {
    auto it = surfaces_.find(guest);
    return it == surfaces_.end() ? nullptr : it->second.get();
}
SDL_PixelFormat* SdlSurfaceBridge::format(uint64_t guest) {
    if (guest < kFormatOff) return nullptr;
    auto* s = lookup(guest - kFormatOff);
    return s ? s->host->format : nullptr;
}
void SdlSurfaceBridge::push(Memory& mem, Surface& s) {
    SDL_Surface image{};
    mem.read(s.meta, &image, sizeof(image));
    s.pixels = reinterpret_cast<uint64_t>(image.pixels);
    s.host->userdata = image.userdata;  // guest cookie, never dereferenced
    if (image.refcount > 0) s.host->refcount = image.refcount;
    if (s.pixel_bytes && s.host->pixels && s.pixels)
        mem.read(s.pixels, s.host->pixels, s.pixel_bytes);
}
void SdlSurfaceBridge::publish(Memory& mem, Surface& s, bool write_pixels) {
    SDL_Surface image = *s.host;
    image.format = reinterpret_cast<SDL_PixelFormat*>(s.meta + kFormatOff);
    image.pixels = reinterpret_cast<void*>(s.pixels);
    image.list_blitmap = nullptr;
    image.map = nullptr;
    mem.write(s.meta, &image, sizeof(image));
    SDL_PixelFormat fmt = *s.host->format;
    fmt.next = nullptr;
    if (fmt.palette) {
        SDL_Palette pal = *fmt.palette;
        if (pal.ncolors < 0 || pal.ncolors > 256)
            throw std::runtime_error("SDL palette exceeds guest bridge capacity");
        pal.colors = reinterpret_cast<SDL_Color*>(s.meta + kColorsOff);
        mem.write(s.meta + kPaletteOff, &pal, sizeof(pal));
        mem.write(s.meta + kColorsOff, fmt.palette->colors,
                  static_cast<size_t>(pal.ncolors) * sizeof(SDL_Color));
        fmt.palette = reinterpret_cast<SDL_Palette*>(s.meta + kPaletteOff);
    }
    mem.write(s.meta + kFormatOff, &fmt, sizeof(fmt));
    if (write_pixels && s.pixel_bytes && s.host->pixels && s.pixels)
        mem.write(s.pixels, s.host->pixels, s.pixel_bytes);
}
uint64_t SdlSurfaceBridge::wrap(Memory& mem, SDL_Surface* host, uint64_t window,
                              uint64_t external_guest, std::vector<uint8_t> external) {
    if (!host) return 0;
    // A window may replace its backing surface after resize. Never retain
    // the invalid host pointer, even when its allocator reuses the address.
    if (window) {
        for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it) {
            if (it->second->window != window) continue;
            Surface& old = *it->second;
            if (old.host == host && old.pixel_bytes ==
                    static_cast<size_t>(host->pitch) * static_cast<size_t>(host->h)) {
                push(mem, old);
                publish(mem, old, false);
                return old.meta;
            }
            erase(mem, it->first, false);
            break;
        }
    }
    auto s = std::make_unique<Surface>();
    s->host = host; s->window = window;
    s->external_host_pixels = std::move(external);
    constexpr size_t kMaxPixels = 64u << 20;
    if (host->pitch < 0 || host->h < 0 ||
        static_cast<uint64_t>(host->pitch) * host->h > kMaxPixels) {
        if (!window) SDL_FreeSurface(host);
        return 0;
    }
    s->pixel_bytes = static_cast<size_t>(host->pitch) * host->h;
    s->meta = mem.mmap_alloc(kMetaBytes);
    if (s->meta && !external_guest && s->pixel_bytes)
        s->owned_pixels = mem.mmap_alloc(s->pixel_bytes);
    s->pixels = external_guest ? external_guest : s->owned_pixels;
    if (!s->meta || (s->pixel_bytes && !s->pixels)) {
        if (s->meta) mem.untrack_allocation(s->meta, kMetaBytes);
        if (!window) SDL_FreeSurface(host);
        return 0;
    }
    uint64_t guest = s->meta;
    surfaces_.emplace(guest, std::move(s));
    // External pixels already contain the caller's data, which may be read-only.
    // Only newly allocated guest pixel buffers need initialization.
    try { publish(mem, *surfaces_.at(guest), !external_guest); }
    catch (...) { erase(mem, guest, !window); throw; }
    return guest;
}
void SdlSurfaceBridge::erase(Memory& mem, uint64_t guest, bool free_host) {
    auto it = surfaces_.find(guest);
    if (it == surfaces_.end()) return;
    Surface& s = *it->second;
    if (free_host) SDL_FreeSurface(s.host);
    if (s.owned_pixels) mem.untrack_allocation(s.owned_pixels, s.pixel_bytes);
    mem.untrack_allocation(s.meta, kMetaBytes);
    surfaces_.erase(it);
}
void SdlSurfaceBridge::forget_window(Memory& mem, uint64_t window) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it)
        if (it->second->window == window) { erase(mem, it->first, false); break; }
}
void SdlSurfaceBridge::clear(Memory& mem) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!surfaces_.empty()) {
        auto it = surfaces_.begin();
        erase(mem, it->first, !it->second->window);
    }
}

uint64_t SdlSurfaceBridge::dispatch(Memory& mem, CPU& cpu, const std::string& n) {
    std::lock_guard<std::mutex> lock(mu_);
    auto a = [&](int i) { return cpu.regs[i]; };
    auto window = [&] { return reinterpret_cast<SDL_Window*>(a(0)); };
    auto surface = [&](int i) -> Surface* {
        Surface* s = lookup(a(i));
        if (s) push(mem, *s);
        return s;
    };
    // Typed local buffers keep small IN/OUT structs off the default 64KiB bounce.
    SDL_Rect r1{}, r2{};
    auto rect = [&](int i, SDL_Rect& r) -> SDL_Rect* {
        if (!a(i)) return nullptr;
        mem.read(a(i), &r, sizeof(r)); return &r;
    };
    if (n == "SDL_CreateRGBSurface")
        return wrap(mem, SDL_CreateRGBSurface(a(0), a(1), a(2), a(3), a(4), a(5), a(6), a(7)));
    if (n == "SDL_CreateRGBSurfaceWithFormat")
        return wrap(mem, SDL_CreateRGBSurfaceWithFormat(a(0), a(1), a(2), a(3), a(4)));
    if (n == "SDL_CreateRGBSurfaceFrom" || n == "SDL_CreateRGBSurfaceWithFormatFrom") {
        const int h = static_cast<int>(a(2)), pitch = static_cast<int>(a(4));
        if (!a(0) || h < 0 || pitch < 0 || static_cast<uint64_t>(h) * pitch > (64u << 20)) return 0;
        std::vector<uint8_t> pixels(static_cast<size_t>(h) * pitch);
        mem.read(a(0), pixels.data(), pixels.size());
        SDL_Surface* host;
        if (n == "SDL_CreateRGBSurfaceFrom") {
            uint64_t alpha = 0; mem.read(cpu.sp, &alpha, 8);
            host = SDL_CreateRGBSurfaceFrom(pixels.data(), a(1), h, a(3), pitch, a(5), a(6), a(7), alpha);
        } else {
            host = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), a(1), h, a(3), pitch, a(5));
        }
        return wrap(mem, host, 0, a(0), std::move(pixels));
    }
    if (n == "SDL_LoadBMP_RW")
        return wrap(mem, SDL_LoadBMP_RW(reinterpret_cast<SDL_RWops*>(a(0)), a(1)));
    if (n == "SDL_GetWindowSurface") return wrap(mem, SDL_GetWindowSurface(window()), a(0));
    if (n == "SDL_UpdateWindowSurface" || n == "SDL_UpdateWindowSurfaceRects") {
        // Reacquire first: an asynchronously resized window can invalidate
        // the previous host surface before the guest retrieves its replacement.
        SDL_Surface* current = SDL_GetWindowSurface(window());
        Surface* s = nullptr;
        for (auto& kv : surfaces_) if (kv.second->window == a(0) && kv.second->host == current) s = kv.second.get();
        if (s) push(mem, *s);
        int rc;
        if (n == "SDL_UpdateWindowSurface") rc = SDL_UpdateWindowSurface(window());
        else {
            int count = static_cast<int>(a(2));
            if (count < 0 || count > 65536) return static_cast<uint64_t>(-1);
            std::vector<SDL_Rect> rects(static_cast<size_t>(count));
            if (count) mem.read(a(1), rects.data(), rects.size() * sizeof(SDL_Rect));
            rc = SDL_UpdateWindowSurfaceRects(window(), rects.data(), count);
        }
        if (s) publish(mem, *s, true);  // includes back-buffer pixel pointer changes
        return static_cast<int64_t>(rc);
    }
    if (n == "SDL_MapRGB" || n == "SDL_MapRGBA" || n == "SDL_GetRGB" || n == "SDL_GetRGBA") {
        bool get = n == "SDL_GetRGB" || n == "SDL_GetRGBA";
        SDL_PixelFormat* f = format(a(get ? 1 : 0));
        if (!f) return get ? 0 : static_cast<uint64_t>(-1);
        if (n == "SDL_MapRGB") return SDL_MapRGB(f, a(1), a(2), a(3));
        if (n == "SDL_MapRGBA") return SDL_MapRGBA(f, a(1), a(2), a(3), a(4));
        Uint8 red, green, blue, alpha;
        SDL_GetRGBA(a(0), f, &red, &green, &blue, &alpha);
        if (a(2)) mem.write(a(2), &red, 1);
        if (a(3)) mem.write(a(3), &green, 1);
        if (a(4)) mem.write(a(4), &blue, 1);
        if (n == "SDL_GetRGBA" && a(5)) mem.write(a(5), &alpha, 1);
        return 0;
    }
    int si = (n == "SDL_CreateTextureFromSurface" || n == "SDL_SetWindowIcon") ? 1 : 0;
    Surface* s = surface(si);
    if (!s) return n == "SDL_CreateTextureFromSurface" || n == "SDL_DuplicateSurface" ||
                   n == "SDL_ConvertSurface" || n == "SDL_ConvertSurfaceFormat"
                   ? 0 : static_cast<uint64_t>(-1);
    SDL_Surface* h = s->host;
    int rc = 0;
    if (n == "SDL_FreeSurface") {
        if (s->window) return 0;  // window owns its surface
        if (h->refcount <= 1) erase(mem, s->meta, true);
        else { SDL_FreeSurface(h); publish(mem, *s, false); }
        return 0;
    }
    if (n == "SDL_DuplicateSurface") return wrap(mem, SDL_DuplicateSurface(h));
    if (n == "SDL_ConvertSurfaceFormat") return wrap(mem, SDL_ConvertSurfaceFormat(h, a(1), a(2)));
    if (n == "SDL_ConvertSurface") {
        // The format is a public value struct. Callers such as Neverball
        // copy it onto the guest stack and change its masks; it need not
        // be the format pointer published with a bridged surface.
        if (!a(1)) return 0;
        SDL_PixelFormat f{};
        mem.read(a(1), &f, sizeof(f));
        SDL_Palette palette{};
        std::vector<SDL_Color> colors;
        if (f.palette) {
            mem.read(reinterpret_cast<uint64_t>(f.palette), &palette, sizeof(palette));
            if (palette.ncolors < 0 || palette.ncolors > 256) return 0;
            colors.resize(static_cast<size_t>(palette.ncolors));
            if (!colors.empty())
                mem.read(reinterpret_cast<uint64_t>(palette.colors), colors.data(),
                         colors.size() * sizeof(SDL_Color));
            palette.colors = colors.data();
            f.palette = &palette;
        }
        f.next = nullptr;
        return wrap(mem, SDL_ConvertSurface(h, &f, a(2)));
    }
    if (n == "SDL_CreateTextureFromSurface")
        return reinterpret_cast<uint64_t>(SDL_CreateTextureFromSurface(reinterpret_cast<SDL_Renderer*>(a(0)), h));
    if (n == "SDL_SetWindowIcon") SDL_SetWindowIcon(window(), h);
    else if (n == "SDL_LockSurface") rc = SDL_LockSurface(h);
    else if (n == "SDL_UnlockSurface") SDL_UnlockSurface(h);
    else if (n == "SDL_SetColorKey") rc = SDL_SetColorKey(h, a(1), a(2));
    else if (n == "SDL_HasColorKey") rc = SDL_HasColorKey(h);
    else if (n == "SDL_SetSurfaceAlphaMod") rc = SDL_SetSurfaceAlphaMod(h, a(1));
    else if (n == "SDL_SetSurfaceColorMod") rc = SDL_SetSurfaceColorMod(h, a(1), a(2), a(3));
    else if (n == "SDL_SetSurfaceBlendMode") rc = SDL_SetSurfaceBlendMode(h, static_cast<SDL_BlendMode>(a(1)));
    else if (n == "SDL_FillRect") rc = SDL_FillRect(h, rect(1, r1), a(2));
    else if (n == "SDL_SaveBMP_RW") rc = SDL_SaveBMP_RW(h, reinterpret_cast<SDL_RWops*>(a(1)), a(2));
    else if (n == "SDL_UpperBlit" || n == "SDL_LowerBlit") {
        Surface* dst = surface(2);
        if (!dst) return static_cast<uint64_t>(-1);
        SDL_Rect* sr = rect(1, r1), *dr = rect(3, r2);
        rc = n == "SDL_UpperBlit" ? SDL_UpperBlit(h, sr, dst->host, dr) : SDL_LowerBlit(h, sr, dst->host, dr);
        if (sr) mem.write(a(1), sr, sizeof(*sr));
        if (dr) mem.write(a(3), dr, sizeof(*dr));
        publish(mem, *dst, true);
    } else return static_cast<uint64_t>(-1);
    // Blits modify only the destination. Icon, lock and metadata operations
    // must not overwrite caller-owned source pixels (e.g. a const icon).
    publish(mem, *s, n == "SDL_FillRect");
    return static_cast<int64_t>(rc);
}
} // namespace arm64emu
#endif
