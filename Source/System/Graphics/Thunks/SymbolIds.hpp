// SymbolIds.hpp — the thunked-symbol names the GraphicThunk dispatcher and its helpers compare on every call.
//
// A thunk call used to run dozens of `std::string == "name"` comparisons (each a strlen and a memcmp) before it
// reached the driver; in a 2026-11-16 profile of Minecraft Weekend the comparisons and the dispatcher around them were
// about 39% of the process's user CPU, more than the translated game code. Each GraphicThunk entry now carries
// its id (`SymbolEntry::sym`), computed once at registration from the list below, and the pixel-transfer families
// (`glTexImage*`, `glReadPixels`, ...) carry two bits. A name outside the list is `Sym::None`. A comparison against a
// name that is not in the list does not compile (`Sym::glNew` is an error), so add the name here when dispatch starts
// to test it. The ids are valid only for entries GraphicThunk registered (`GraphicThunk::register_symbol`).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
namespace arm64emu {
#define BIFROST_GRAPHIC_SYMS(X) \
 X(eglCreateImageKHR) X(eglCreateImage) X(eglDestroyImageKHR) X(eglDestroyImage) X(glTexSubImage2D) \
 X(glDeleteTextures) X(glTexImage2D) X(glEGLImageTargetTexture2DOES) X(glEGLImageTargetRenderbufferStorageOES) \
 X(eglDupNativeFenceFDANDROID) X(eglCreateSyncKHR) X(eglCreateSync) X(SDL_UpdateWindowSurface) \
 X(SDL_UpdateWindowSurfaceRects) X(SDL_DestroyWindow) X(SDL_SetWindowSize) X(SDL_Quit) X(eglGetDisplay) \
 X(eglCreateWindowSurface) X(glMapBufferRange) X(SDL_PushEvent) X(SDL_PollEvent) X(SDL_WaitEvent) \
 X(SDL_WaitEventTimeout) X(SDL_GetMouseState) X(SDL_GetRelativeMouseState) X(SDL_GetWindowPosition) \
 X(SDL_GetWindowSize) X(SDL_GetDisplayDPI) X(SDL_GetWindowDisplayMode) X(SDL_SetTextInputRect) \
 X(SDL_GetWindowGammaRamp) X(glDrawPixels) X(glBitmap) X(glReadPixels) X(glReadnPixels) X(glDeleteProgram) \
 X(glDeleteVertexArrays) X(glUseProgram) X(glActiveTexture) X(glBindTexture) X(glBindVertexArray) \
 X(glBindBuffer) X(glDrawRangeElements) X(glDrawRangeElementsBaseVertex) X(glDrawElementsIndirect) \
 X(eglSwapBuffers) X(eglDestroySurface) X(eglMakeCurrent) X(eglGetError) X(eglTerminate) X(eglDestroyContext) \
 X(SDL_GL_SwapWindow) X(SDL_PumpEvents) X(SDL_PeepEvents) X(SDL_ResetKeyboard) X(SDL_CreateSemaphore) \
 X(SDL_DestroySemaphore) X(SDL_CreateCond) X(SDL_DestroyCond) X(SDL_SetRelativeMouseMode) X(SDL_SetWindowGrab) \
 X(glGetError) X(SDL_CalculateGammaRamp) X(SDL_RenderSetScale) X(SDL_RenderCopyEx) X(glSampleCoverage) \
 X(SDL_GetKeyboardState) X(SDL_ThreadID) X(SDL_SetEventFilter)
enum class Sym : uint8_t {
    None = 0,
#define X(name) name,
    BIFROST_GRAPHIC_SYMS(X)
#undef X
    Count
};
static_assert(static_cast<unsigned>(Sym::Count) <= 255, "Sym must fit SymbolEntry::sym");
inline Sym sym_from_name(const std::string& name) {
    static const std::unordered_map<std::string, Sym> table = [] {
        std::unordered_map<std::string, Sym> t;
        unsigned next = 1;
#define X(name) t.emplace(#name, static_cast<Sym>(next++));
        BIFROST_GRAPHIC_SYMS(X)
#undef X
        return t;
    }();
    const auto it = table.find(name);
    return it == table.end() ? Sym::None : it->second;
}
// Transfer calls whose pointer argument is a byte offset into the bound pixel buffer when one is bound: the
// uploads (unpack) and the readbacks (pack). These are the two families Dispatch.cpp used to match by prefix.
enum : uint8_t { kPixelUnpack = 1, kPixelPack = 2 };
inline uint8_t pixel_transfer_flags(const std::string& name) {
    const auto starts_with = [&](const char* prefix) { return name.compare(0, std::strlen(prefix), prefix) == 0; };
    const bool unpack =
        starts_with("glTexImage") || starts_with("glTexSubImage") || starts_with("glCompressedTexImage") ||
        starts_with("glCompressedTexSubImage") || starts_with("glTextureSubImage") ||
        starts_with("glCompressedTextureSubImage") || name == "glDrawPixels" || name == "glBitmap" ||
        starts_with("glColorTable");
    const bool pack =
        name == "glReadPixels" || starts_with("glGetTexImage") || starts_with("glGetCompressedTexImage") ||
        starts_with("glGetnTexImage") || name == "glReadnPixels" || starts_with("glGetTextureImage") ||
        starts_with("glGetCompressedTextureImage");
    return static_cast<uint8_t>((unpack ? kPixelUnpack : 0) | (pack ? kPixelPack : 0));
}
} // namespace arm64emu
