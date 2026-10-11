#ifndef BIFROST_SDL_SHIM_H
#define BIFROST_SDL_SHIM_H

// Minimal SDL2 API surface for the VoxelSpace guest. The real SDL2 is
// resolved at runtime through the emulator's thunked host libSDL2 (via
// internal syscalls 0x1002/0x1003), so the guest binary stays a static
// musl ELF. The struct layouts below mirror host SDL2 (64-bit) exactly
// for the fields the game touches (event.type / event.key.keysym.sym).
#define SDL_MAIN_HANDLED 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;

typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_Keysym {
    Uint32 scancode;
    Uint32 sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8  state;
    Uint8  repeat;
    Uint8  padding2;
    Uint8  padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    Uint8 padding[56];
} SDL_Event;

enum {
    SDL_INIT_TIMER          = 0x00000001u,
    SDL_INIT_AUDIO          = 0x00000010u,
    SDL_INIT_VIDEO          = 0x00000020u,
    SDL_INIT_JOYSTICK       = 0x00000200u,
    SDL_INIT_HAPTIC         = 0x00001000u,
    SDL_INIT_GAMECONTROLLER = 0x00002000u,
    SDL_INIT_EVENTS         = 0x00004000u,
    SDL_INIT_SENSOR         = 0x00008000u,
    SDL_INIT_EVERYTHING     = 0x0000F231u,

    SDL_WINDOWPOS_CENTERED  = 0x2FFF0000u,

    SDL_RENDERER_PRESENTVSYNC = 0x00000004u,
    SDL_PIXELFORMAT_RGBA32    = 0x16762004u,
    SDL_TEXTUREACCESS_STREAMING = 0x00000001u,

    SDL_QUIT   = 0x100,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP   = 0x301,
};

enum {
    SDLK_a = 'a', SDLK_d = 'd', SDLK_j = 'j', SDLK_m = 'm',
    SDLK_n = 'n', SDLK_s = 's', SDLK_u = 'u', SDLK_w = 'w',
};

int         SDL_Init(Uint32 flags);
void        SDL_Quit(void);
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h,
                             Uint32 flags);
void        SDL_DestroyWindow(SDL_Window* window);
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags);
void        SDL_DestroyRenderer(SDL_Renderer* renderer);
SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format,
                               int access, int w, int h);
void        SDL_DestroyTexture(SDL_Texture* texture);
int         SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect,
                              const void* pixels, int pitch);
int         SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture,
                           const SDL_Rect* srcrect, const SDL_Rect* dstrect);
void        SDL_RenderPresent(SDL_Renderer* renderer);
Uint32      SDL_GetTicks(void);
void        SDL_Delay(Uint32 ms);
int         SDL_PollEvent(SDL_Event* event);

#endif
