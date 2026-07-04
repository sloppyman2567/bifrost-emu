// frost/thunk.hpp — GraphicThunk class definition (graphic API thunking).
//
// v1.4.5-alpha (Turn 36): NEW. The GraphicThunk class is forward-
// declared in frost/graphics.hpp (so the header doesn't pull in dlfcn.h
// / GL / EGL / SDL2 headers). This header provides the full class
// definition, needed by:
//   - frost_graphics/graphics.cpp (FrostGraphics destructor destroys
//     the unique_ptr<GraphicThunk> member)
//   - frost_graphics/thunk.cpp (the class implementation)
//   - src/frontend/dynamic_linker.cpp (calls GraphicThunk::resolve)
//
// External consumers (libbifrost users) do NOT need to include this
// header — they use FrostGraphics::thunk() which returns a
// GraphicThunk* (forward-declared).
#pragma once

#include <memory>
#include <string>

namespace arm64emu {

// Forward-declare the pimpl.
struct GraphicThunkImpl;

class GraphicThunk {
public:
    GraphicThunk();
    ~GraphicThunk();

    // Non-copyable, non-movable (owns host-side GL/EGL/SDL2 state).
    GraphicThunk(const GraphicThunk&) = delete;
    GraphicThunk& operator=(const GraphicThunk&) = delete;

    // Whether thunking is enabled (BIFROST_THUNK_GRAPHICS=1 env var).
    bool enabled() const;

    // Resolve a graphic API symbol from the guest's perspective.
    // `lib` is the library basename (e.g. "libGL.so.1").
    // `sym` is the symbol name (e.g. "glClear").
    // Returns a host function pointer, or nullptr if not thunked.
    void* resolve(const std::string& lib, const std::string& sym);

private:
    void* resolve_gl_(const std::string& sym);
    void* resolve_egl_(const std::string& sym);
    void* resolve_sdl_(const std::string& sym);

    // pimpl — keeps GL/EGL/SDL2 headers out of this public header.
    // The unique_ptr needs the full type to destroy, so the destructor
    // must be out-of-line (defined in thunk.cpp).
    std::unique_ptr<GraphicThunkImpl> impl_;
};

} // namespace arm64emu
