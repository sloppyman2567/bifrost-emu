// Neverball format conversion and Doom 3 read-only icon pixels. No display required.
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
typedef struct { int ncolors; uint32_t *colors; uint32_t version; int refcount; } Palette;
typedef struct Format { uint32_t format; Palette *palette; uint8_t bpp, bytes, pad[2];
    uint32_t rmask,gmask,bmask,amask; uint8_t loss[4],shift[4]; int refcount; struct Format *next; } Format;
typedef struct { uint32_t flags; Format *format; int w,h,pitch; void *pixels; void *userdata;
    int locked; void *list; int clip[4]; void *map; int refcount; } Surface;
_Static_assert(sizeof(Surface)==96 && sizeof(Format)==56, "SDL LP64 layout");

static const uint32_t icon_pixels[4] = {0xff112233, 0xff445566, 0xff778899, 0xffaabbcc};

int main(void) {
    uint64_t lib=thunk_dlopen("libSDL2.so"); CHECK(lib);
    Surface *(*create)(uint32_t,int,int,int,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceWithFormat");
    Surface *(*from)(void*,int,int,int,int,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceWithFormatFrom");
    Surface *(*convert)(Surface*,Format*,uint32_t)=sym(lib,"SDL_ConvertSurface");
    void (*free_surface)(Surface*)=sym(lib,"SDL_FreeSurface");
    Surface *(*from_masks)(void*,int,int,int,int,uint32_t,uint32_t,uint32_t,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceFrom");
    Surface *(*duplicate)(Surface*)=sym(lib,"SDL_DuplicateSurface");
    int (*alpha_mod)(Surface*,uint8_t)=sym(lib,"SDL_SetSurfaceAlphaMod");
    int (*lock)(Surface*)=sym(lib,"SDL_LockSurface");
    void (*unlock)(Surface*)=sym(lib,"SDL_UnlockSurface");
    int (*blit)(Surface*,void*,Surface*,void*)=sym(lib,"SDL_UpperBlit");
    int (*fill)(Surface*,void*,uint32_t)=sym(lib,"SDL_FillRect");
    /* Creating and reading an external surface must never write .rodata.
       Exercise both constructors, metadata/lock calls and blit source use. */
    for (int form=0;form<2;form++) {
        Surface *icon=form ? from((void*)icon_pixels,2,2,32,8,0x16362004)
                           : from_masks((void*)icon_pixels,2,2,32,8,0xff0000,0xff00,0xff,0xff000000);
        CHECK(icon && icon->pixels==(void*)icon_pixels);
        CHECK(alpha_mod(icon,255)==0);
        CHECK(lock(icon)==0); unlock(icon);
        Surface *copy=duplicate(icon); CHECK(copy);
        CHECK(memcmp(copy->pixels,icon_pixels,sizeof(icon_pixels))==0);
        Surface *dst=create(0,2,2,32,0x16362004); CHECK(dst);
        CHECK(blit(icon,NULL,dst,NULL)==0);
        CHECK(memcmp(dst->pixels,icon_pixels,sizeof(icon_pixels))==0);
        CHECK(fill(dst,NULL,0xffabcdef)==0);
        for (int i=0;i<4;i++) CHECK(((uint32_t*)dst->pixels)[i]==0xffabcdef);
        free_surface(dst); free_surface(copy); free_surface(icon);
    }
    /* Writable caller buffers still receive changes made by SDL. */
    uint32_t writable[4]={0};
    Surface *external=from(writable,2,2,32,8,0x16362004); CHECK(external);
    CHECK(fill(external,NULL,0xff123456)==0);
    for (int i=0;i<4;i++) CHECK(writable[i]==0xff123456);
    Surface *blit_source=from((void*)icon_pixels,2,2,32,8,0x16362004); CHECK(blit_source);
    CHECK(blit(blit_source,NULL,external,NULL)==0);
    CHECK(memcmp(writable,icon_pixels,sizeof(writable))==0);
    free_surface(blit_source); free_surface(external);
    /* SDL_ttf pads rows; Neverball converts with a stack-local format. */
    uint32_t pixels[24];
    for (int i=0;i<24;i++) pixels[i]=0xff112233;
    Surface *source=from(pixels,3,3,32,32,0x16362004); CHECK(source);
    Format copied=*source->format;
    copied.rmask=0xff; copied.bmask=0xff0000;
    Surface *packed=convert(source,&copied,0);
    CHECK(packed && packed->w==3 && packed->h==3 && packed->pitch==12);
    for (int i=0;i<9;i++) CHECK(((uint32_t*)packed->pixels)[i]==0xff332211);
    free_surface(packed); free_surface(source);
    /* Copied indexed formats contain two levels of guest pointers. */
    Surface *indexed=create(0,2,2,8,0x13000801); CHECK(indexed);
    CHECK(indexed->format->palette && indexed->format->palette->ncolors==256);
    Palette palette=*indexed->format->palette;
    uint32_t colors[256];
    for (int i=0;i<256;i++) colors[i]=0xff000000u|(uint32_t)i;
    palette.colors=colors;
    copied=*indexed->format; copied.palette=&palette;
    Surface *converted=convert(indexed,&copied,0); CHECK(converted);
    CHECK(converted->format->palette && converted->format->palette->colors[37]==colors[37]);
    free_surface(converted);
    palette.ncolors=257; CHECK(!convert(indexed,&copied,0));
    CHECK(!convert(indexed,NULL,0));
    free_surface(indexed);
    puts("ALL PASS: copied SDL surface formats and palettes");
    return 0;
}
