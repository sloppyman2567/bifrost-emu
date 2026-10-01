// SDL2 ABI and Neverball fullscreen recreation regression.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t thunk_dlopen(const char *path) {
    register uint64_t x0 __asm__("x0") = (uintptr_t)path;
    register uint64_t x1 __asm__("x1") = 1;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static void *sym(uint64_t lib, const char *name) {
    register uint64_t x0 __asm__("x0") = lib;
    register uint64_t x1 __asm__("x1") = (uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    if (!x0) { fprintf(stderr,"missing symbol: %s\n", name); exit(2); }
    return (void *)(uintptr_t)x0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } Mode;
typedef union { uint64_t align; unsigned char bytes[56]; } Event;
_Static_assert(sizeof(Mode)==24 && sizeof(Event)==56, "SDL2 LP64 ABI");
int main(void) {
    uint64_t lib=thunk_dlopen("libSDL2.so"); CHECK(lib);
    int (*init)(uint32_t)=sym(lib,"SDL_Init");
    void (*quit)(void)=sym(lib,"SDL_Quit");
    int (*displays)(void)=sym(lib,"SDL_GetNumVideoDisplays");
    int (*desktop)(int,Mode*)=sym(lib,"SDL_GetDesktopDisplayMode");
    int (*current)(int,Mode*)=sym(lib,"SDL_GetCurrentDisplayMode");
    void *(*create)(const char*,int,int,int,int,uint32_t)=sym(lib,"SDL_CreateWindow");
    void (*destroy)(void*)=sym(lib,"SDL_DestroyWindow");
    void (*size)(void*,int*,int*)=sym(lib,"SDL_GetWindowSize");
    void (*resize)(void*,int,int)=sym(lib,"SDL_SetWindowSize");
    int (*fullscreen)(void*,uint32_t)=sym(lib,"SDL_SetWindowFullscreen");
    uint32_t (*id)(void*)=sym(lib,"SDL_GetWindowID");
    int (*poll)(Event*)=sym(lib,"SDL_PollEvent");
    int (*push)(Event*)=sym(lib,"SDL_PushEvent");
    void (*delay)(uint32_t)=sym(lib,"SDL_Delay");
    if(init(0x20)!=0) { puts("SKIP: SDL video unavailable"); return 77; }
    CHECK(displays()>0);
    Mode dm;
    for(int i=0;i<displays();++i) {
        struct { Mode mode; uint64_t guard; } out;
        memset(&out,0xa5,sizeof out); out.guard=UINT64_C(0x123456789abcdef0);
        CHECK(desktop(i,&out.mode)==0);
        CHECK(out.mode.w>1 && out.mode.h>1);
        CHECK(out.guard==UINT64_C(0x123456789abcdef0));
        CHECK(current(i,&out.mode)==0 && out.mode.w>1 && out.mode.h>1);
        CHECK(out.guard==UINT64_C(0x123456789abcdef0));
    }
    CHECK(desktop(-1,&dm)<0); CHECK(current(-1,&dm)<0);
    CHECK(desktop(0,&dm)==0);
    /* Neverball recreates its window, then saves the desktop-mode dimensions. */
    for(int cycle=0;cycle<3;++cycle) {
        void *win=create("Bifrost fullscreen regression",0x2fff0000,0x2fff0000,
                         dm.w,dm.h,0x1009); CHECK(win);
        CHECK(desktop(0,&dm)==0 && dm.w>1 && dm.h>1);
        destroy(win);
        win=create("Bifrost fullscreen regression",0x2fff0000,0x2fff0000,
                   dm.w,dm.h,8); CHECK(win);
        int w=0,h=0; size(win,&w,&h); CHECK(w==dm.w && h==dm.h);
        CHECK(fullscreen(win,0x1001)==0);
        CHECK(fullscreen(win,0)==0);
        size(win,&w,&h); CHECK(w>1 && h>1);
        Event event; while(poll(&event)) {}
        resize(win,640+cycle,480+cycle);
        int seen=0;
        for(int n=0;n<100 && !seen;++n) {
            while(poll(&event)) {
                uint32_t type,window; int ew,eh;
                memcpy(&type,event.bytes,4); memcpy(&window,event.bytes+8,4);
                memcpy(&ew,event.bytes+16,4); memcpy(&eh,event.bytes+20,4);
                if(type==0x200 && window==id(win) && event.bytes[12]==6 &&
                   ew==640+cycle && eh==480+cycle) seen=1;
            }
            if(!seen) delay(10);
        }
        CHECK(seen);
        size(win,&w,&h); CHECK(w==640+cycle && h==480+cycle);
        /* Verify event payload crossing independently of compositor timing. */
        memset(&event,0,sizeof event); uint32_t type=0x200,window=id(win);
        memcpy(event.bytes,&type,4); memcpy(event.bytes+8,&window,4);
        event.bytes[12]=6; memcpy(event.bytes+16,&w,4); memcpy(event.bytes+20,&h,4);
        CHECK(push(&event)==1); seen=0;
        while(poll(&event)) if(event.bytes[12]==6) seen=1;
        CHECK(seen); destroy(win);
    }
    quit(); puts("ALL PASS: SDL2 display modes, fullscreen restore, resize events");
    return 0;
}
