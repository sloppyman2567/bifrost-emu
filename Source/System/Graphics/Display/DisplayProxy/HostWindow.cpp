// DisplayProxy HostWindow: guest handles and host display forwarding.
#include "System/Graphics/Display/DisplayProxy/DisplayProxyInternal.hpp"
namespace arm64emu {
DisplayProxy::DisplayProxy() = default;
DisplayProxy::~DisplayProxy() { shutdown(); }
void DisplayProxy::shutdown() {
#if defined(BIFROST_USE_SDL2)
    SDL_Window* w = (SDL_Window*)window_;
    SDL_Renderer* r = (SDL_Renderer*)renderer_;
    SDL_Texture* t = (SDL_Texture*)texture_;
    if (t) SDL_DestroyTexture(t);
    if (r) SDL_DestroyRenderer(r);
    if (w) { window_stats::forget(w); SDL_DestroyWindow(w); }
#endif
    handles_.clear();
    next_guest_addr_ = 0;
    handle_page_end_ = 0;
    window_ = renderer_ = texture_ = nullptr;
}
bool DisplayProxy::init(uint32_t width, uint32_t height, Memory* mem) {
    mem_ = mem;
    width_ = width ? width : 640;
    height_ = height ? height : 480;
    return init_sdl2_();
}
bool DisplayProxy::init_sdl2_() {
#if defined(BIFROST_USE_SDL2)
    // Prevent SDL from installing its own signal handlers, which would
    // interfere with the emulator's signal delivery (sigint, sigsuspend, etc.).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[display-proxy] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    window_ = SDL_CreateWindow("bifrost-emu proxy",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        (int)width_, (int)height_,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        fprintf(stderr, "[display-proxy] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    renderer_ = SDL_CreateRenderer((SDL_Window*)window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer((SDL_Window*)window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer_) {
        fprintf(stderr, "[display-proxy] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        window_stats::forget(window_); SDL_DestroyWindow((SDL_Window*)window_); window_ = nullptr;
        SDL_Quit();
        return false;
    }
    texture_ = SDL_CreateTexture((SDL_Renderer*)renderer_, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING, (int)width_, (int)height_);
    if (!texture_) {
        fprintf(stderr, "[display-proxy] SDL_CreateTexture failed: %s\n", SDL_GetError());
    }
    return true;
#else
    (void)width_; (void)height_;
    fprintf(stderr, "[display-proxy] SDL2 not available\n");
    return false;
#endif
}
void DisplayProxy::present() {
#if defined(BIFROST_USE_SDL2)
    if (window_ && renderer_) {
        SDL_SetRenderDrawColor((SDL_Renderer*)renderer_, 0, 0, 0, 255);
        SDL_RenderClear((SDL_Renderer*)renderer_);
        if (texture_) {
            SDL_RenderCopy((SDL_Renderer*)renderer_, (SDL_Texture*)texture_, nullptr, nullptr);
        }
        SDL_RenderPresent((SDL_Renderer*)renderer_);
        window_stats::sdl_present(window_,"Display proxy");
    }
    // Always pump SDL events (even without a window) so the host window
    // never looks frozen and X11 input keeps flowing. x_pump_ owns the
    // drain now — present() no longer drops input on the floor.
    x_pump_();
#else
    (void)window_; (void)renderer_; (void)texture_;
#endif
}

bool DisplayProxy::ensure_sdl() {
    if (ready()) return true;
    return init_sdl2_();
}

uint64_t DisplayProxy::alloc_handle(uint32_t type) {
    if (!mem_ || handles_.size() >= MAX_HANDLES) return 0;
    // Reserve real guest storage instead of writing into a fixed, unmapped
    // address (which also overlapped the sigreturn trampoline). Pack handles
    // into pages and keep their addresses distinct after free_handle().
    if (next_guest_addr_ == handle_page_end_) {
        uint64_t page = mem_->mmap_alloc(Memory::PAGE_SIZE, 0, false,
            Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
        if (!page) return 0;
        next_guest_addr_ = page;
        handle_page_end_ = page + Memory::PAGE_SIZE;
    }
    uint64_t guest_addr = next_guest_addr_;
    HandleHdr hdr;
    hdr.type = type;
    hdr.index = (uint32_t)handles_.size();
    mem_->write(guest_addr, &hdr, sizeof(hdr));
    handles_.push_back({guest_addr, type});
    next_guest_addr_ += HANDLE_STEP;
    return guest_addr;
}
void DisplayProxy::free_handle(uint64_t guest_addr) {
    if (!mem_ || guest_addr == 0) return;
    for (size_t i = 0; i < handles_.size(); ++i) {
        if (handles_[i].guest_addr == guest_addr) {
            handles_.erase(handles_.begin() + (ptrdiff_t)i);
            break;
        }
    }
}
void* DisplayProxy::handle_to_host(uint64_t guest_addr) const {
    if (!mem_ || guest_addr == 0) return nullptr;
    uint8_t* hp = mem_->guest_to_host_ptr(guest_addr);
    if (hp) return hp;
    return nullptr;
}
} // namespace arm64emu
