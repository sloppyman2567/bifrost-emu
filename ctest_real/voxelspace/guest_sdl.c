// guest_sdl.c — guest-side SDL2 shim for the VoxelSpace game.
//
// The game is a static musl ELF (no libSDL2.a available for aarch64-musl),
// so SDL_* symbols are resolved at runtime against the emulator's thunked
// host libSDL2 using the internal dlopen/dlsym syscalls (0x1002/0x1003) —
// the same mechanism as test_sdl_gl_triangle.c.
#include <stdint.h>
#include <stdio.h>
#include <SDL.h>

static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static uint64_t bifrost_dlsym(uint64_t handle, const char* name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

typedef int      (*fn_SDL_Init)(uint32_t);
typedef void     (*fn_SDL_Quit)(void);
typedef void*    (*fn_SDL_CreateWindow)(const char*, int, int, int, int, uint32_t);
typedef void     (*fn_SDL_DestroyWindow)(void*);
typedef void*    (*fn_SDL_CreateRenderer)(void*, int, uint32_t);
typedef void     (*fn_SDL_DestroyRenderer)(void*);
typedef void*    (*fn_SDL_CreateTexture)(void*, uint32_t, int, int, int);
typedef void     (*fn_SDL_DestroyTexture)(void*);
typedef int      (*fn_SDL_UpdateTexture)(void*, const void*, const void*, int);
typedef int      (*fn_SDL_RenderCopy)(void*, void*, const void*, const void*);
typedef void     (*fn_SDL_RenderPresent)(void*);
typedef uint32_t (*fn_SDL_GetTicks)(void);
typedef void     (*fn_SDL_Delay)(uint32_t);
typedef int      (*fn_SDL_PollEvent)(void*);

static uint64_t sdl_handle = 0;
static fn_SDL_Init            sdl_SDL_Init;
static fn_SDL_Quit            sdl_SDL_Quit;
static fn_SDL_CreateWindow    sdl_SDL_CreateWindow;
static fn_SDL_DestroyWindow   sdl_SDL_DestroyWindow;
static fn_SDL_CreateRenderer  sdl_SDL_CreateRenderer;
static fn_SDL_DestroyRenderer sdl_SDL_DestroyRenderer;
static fn_SDL_CreateTexture   sdl_SDL_CreateTexture;
static fn_SDL_DestroyTexture  sdl_SDL_DestroyTexture;
static fn_SDL_UpdateTexture   sdl_SDL_UpdateTexture;
static fn_SDL_RenderCopy      sdl_SDL_RenderCopy;
static fn_SDL_RenderPresent   sdl_SDL_RenderPresent;
static fn_SDL_GetTicks        sdl_SDL_GetTicks;
static fn_SDL_Delay           sdl_SDL_Delay;
static fn_SDL_PollEvent       sdl_SDL_PollEvent;

static int sdl_ready = 0;

#define LOAD(name) do { \
    sdl_##name = (fn_##name)(uintptr_t)bifrost_dlsym(sdl_handle, #name); \
    if (!sdl_##name) { \
        fprintf(stderr, "voxelspace: dlsym(%s) failed\n", #name); \
        return -1; \
    } \
} while (0)

static int init_sdl(void) {
    if (sdl_ready) return 0;
    sdl_handle = bifrost_dlopen("libSDL2.so", 1);
    if (!sdl_handle) sdl_handle = bifrost_dlopen("libSDL2-2.0.so.0", 1);
    if (!sdl_handle) {
        fprintf(stderr, "voxelspace: libSDL2 thunk unavailable\n");
        return -1;
    }
    LOAD(SDL_Init);
    LOAD(SDL_Quit);
    LOAD(SDL_CreateWindow);
    LOAD(SDL_DestroyWindow);
    LOAD(SDL_CreateRenderer);
    LOAD(SDL_DestroyRenderer);
    LOAD(SDL_CreateTexture);
    LOAD(SDL_DestroyTexture);
    LOAD(SDL_UpdateTexture);
    LOAD(SDL_RenderCopy);
    LOAD(SDL_RenderPresent);
    LOAD(SDL_GetTicks);
    LOAD(SDL_Delay);
    LOAD(SDL_PollEvent);
    sdl_ready = 1;
    return 0;
}

int    SDL_Init(Uint32 flags)          { if (init_sdl()) return -1; return sdl_SDL_Init(flags); }
void   SDL_Quit(void)                  { if (!sdl_ready) return; sdl_SDL_Quit(); }
SDL_Window* SDL_CreateWindow(const char* t, int x, int y, int w, int h, Uint32 f)
                                      { if (init_sdl()) return NULL; return (SDL_Window*)sdl_SDL_CreateWindow(t, x, y, w, h, f); }
void   SDL_DestroyWindow(SDL_Window* w){ if (sdl_ready) sdl_SDL_DestroyWindow(w); }
SDL_Renderer* SDL_CreateRenderer(SDL_Window* w, int i, Uint32 f)
                                      { if (init_sdl()) return NULL; return (SDL_Renderer*)sdl_SDL_CreateRenderer(w, i, f); }
void   SDL_DestroyRenderer(SDL_Renderer* r){ if (sdl_ready) sdl_SDL_DestroyRenderer(r); }
SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, Uint32 fmt, int acc, int w, int h)
                                      { if (init_sdl()) return NULL; return (SDL_Texture*)sdl_SDL_CreateTexture(r, fmt, acc, w, h); }
void   SDL_DestroyTexture(SDL_Texture* t){ if (sdl_ready) sdl_SDL_DestroyTexture(t); }
int    SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* r, const void* p, int pt)
                                      { if (init_sdl()) return -1; return sdl_SDL_UpdateTexture(t, r, p, pt); }
int    SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* s, const SDL_Rect* d)
                                      { if (init_sdl()) return -1; return sdl_SDL_RenderCopy(r, t, s, d); }
void   SDL_RenderPresent(SDL_Renderer* r){ if (sdl_ready) sdl_SDL_RenderPresent(r); }
Uint32 SDL_GetTicks(void)             { if (!sdl_ready) return 0; return sdl_SDL_GetTicks(); }
void   SDL_Delay(Uint32 ms)           { if (sdl_ready) sdl_SDL_Delay(ms); }
int    SDL_PollEvent(SDL_Event* e)    { if (init_sdl()) return 0; return sdl_SDL_PollEvent(e); }
