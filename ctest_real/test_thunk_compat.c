/* Guest-visible SDL structs/FP returns and GL/Vulkan pointer marshalling.
 * scripts/run_thunk_compat.sh supplies small host probes for GL/Vulkan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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

static int sdl_test(void) {
    uint64_t lib=thunk_dlopen("libSDL2.so"); CHECK(lib);
    Surface *(*create)(uint32_t,int,int,int,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceWithFormat");
    Surface *(*from)(void*,int,int,int,int,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceWithFormatFrom");
    Surface *(*from_masks)(void*,int,int,int,int,uint32_t,uint32_t,uint32_t,uint32_t)=sym(lib,"SDL_CreateRGBSurfaceFrom");
    Surface *(*duplicate)(Surface*)=sym(lib,"SDL_DuplicateSurface");
    Surface *(*convert)(Surface*,Format*,uint32_t)=sym(lib,"SDL_ConvertSurface");
    void (*free_surface)(Surface*)=sym(lib,"SDL_FreeSurface");
    int (*fill)(Surface*,const void*,uint32_t)=sym(lib,"SDL_FillRect");
    int (*blit)(Surface*,const void*,Surface*,void*)=sym(lib,"SDL_UpperBlit");
    int (*lock)(Surface*)=sym(lib,"SDL_LockSurface");
    void (*unlock)(Surface*)=sym(lib,"SDL_UnlockSurface");
    uint32_t (*rgba)(Format*,uint8_t,uint8_t,uint8_t,uint8_t)=sym(lib,"SDL_MapRGBA");
    void (*getrgba)(uint32_t,Format*,uint8_t*,uint8_t*,uint8_t*,uint8_t*)=sym(lib,"SDL_GetRGBA");
    int (*set_blend)(Surface*,int)=sym(lib,"SDL_SetSurfaceBlendMode");
    const uint8_t *(*keyboard)(int*)=sym(lib,"SDL_GetKeyboardState");
    void (*pump)(void)=sym(lib,"SDL_PumpEvents");
    float (*sqrtf_fn)(float)=sym(lib,"SDL_sqrtf");
    float (*atan2f_fn)(float,float)=sym(lib,"SDL_atan2f");
    double (*pow_fn)(double,double)=sym(lib,"SDL_pow");
    double (*sqrt_fn)(double)=sym(lib,"SDL_sqrt");
    CHECK(sqrtf_fn(9)==3 && sqrt_fn(16)==4 && pow_fn(2,5)==32);
    CHECK(atan2f_fn(0,1)==0);
    int count=0; const uint8_t *keys=keyboard(&count); CHECK(keys && count==512);
    for(int i=0;i<count;i++) CHECK(keys[i]<=1);
    /* Cached pointers must get refreshed, not just subsequent getter calls. */
    ((uint8_t*)keys)[0]=0xff;
    pump(); CHECK(keys[0]<=1);
    CHECK(keyboard(NULL)==keys);
    for(int iteration=0;iteration<16;iteration++) {
        Surface *s=create(0,8,4,32,0x16362004); CHECK(s);
        CHECK(s->w==8 && s->h==4 && s->pitch>=32 && s->pixels && s->format);
        CHECK(s->format->bpp==32 && s->format->amask==0xff000000 && !s->format->next);
        CHECK(!s->map && !s->list);
        uint32_t color=rgba(s->format,17,34,51,255); CHECK(color==0xff112233);
        uint8_t r,g,b,a; getrgba(color,s->format,&r,&g,&b,&a);
        CHECK(r==17 && g==34 && b==51 && a==255);
        CHECK(fill(s,NULL,color)==0);
        CHECK(((uint32_t*)s->pixels)[0]==color);
        CHECK(lock(s)==0); ((uint32_t*)s->pixels)[0]=0xffabcdef; unlock(s);
        Surface *copy=duplicate(s); CHECK(copy && ((uint32_t*)copy->pixels)[0]==0xffabcdef);
        Surface *converted=convert(s,copy->format,0); CHECK(converted && converted->w==8);
        CHECK(set_blend(s,0)==0 && blit(s,NULL,copy,NULL)==0);
        CHECK(((uint32_t*)copy->pixels)[0]==0xffabcdef);
        s->userdata=(void*)(uintptr_t)0x12345678;
        CHECK(fill(s,NULL,color)==0 && s->userdata==(void*)(uintptr_t)0x12345678);
        free_surface(converted); free_surface(copy); free_surface(s);
    }
    Surface *indexed=create(0,2,2,8,0x13000801); CHECK(indexed && indexed->format->palette);
    CHECK(indexed->format->palette->ncolors==256);
    volatile uint32_t palcolor=indexed->format->palette->colors[255]; (void)palcolor;
    /* Neverball passes a stack copy with swapped masks. SDL_ttf's
     * padded input pitch must become tightly packed on conversion. */
    uint32_t padded[24];
    for (int i=0;i<24;i++) padded[i]=0xff112233;
    Surface *font_like=from(padded,3,3,32,32,0x16362004); CHECK(font_like);
    Format copied=*font_like->format;
    copied.rmask=0xff; copied.bmask=0xff0000;
    Surface *packed=convert(font_like,&copied,0);
    CHECK(packed && packed->w==3 && packed->h==3 && packed->pitch==12);
    for (int i=0;i<9;i++) CHECK(((uint32_t*)packed->pixels)[i]==0xff332211);
    free_surface(packed); free_surface(font_like);
    /* A copied indexed format carries nested guest palette pointers. */
    Palette copied_palette=*indexed->format->palette;
    uint32_t palette_colors[256];
    for (int i=0;i<256;i++) palette_colors[i]=0xff000000u|(uint32_t)i;
    copied_palette.colors=palette_colors;
    copied=*indexed->format; copied.palette=&copied_palette;
    Surface *palette_copy=convert(indexed,&copied,0); CHECK(palette_copy);
    CHECK(palette_copy->format->palette &&
          palette_copy->format->palette->colors[37]==palette_colors[37]);
    free_surface(palette_copy);
    free_surface(indexed);
    /* Both direct-window and sparse guest pixels must outlive the create call. */
    uint32_t low_pixels[16]={0};
    uint32_t *high_pixels=mmap((void*)(uintptr_t)0x5000000000ULL,4096,PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    CHECK(high_pixels!=MAP_FAILED && (uintptr_t)high_pixels>UINT32_MAX);
    for(int high=0;high<2;high++) {
        uint32_t *pixels=high ? high_pixels : low_pixels;
        Surface *s=from(pixels,4,4,32,16,0x16362004);
        CHECK(s && s->pixels==pixels);
        pixels[0]=0xff765432;
        Surface *copy=duplicate(s); CHECK(copy && ((uint32_t*)copy->pixels)[0]==pixels[0]);
        CHECK(fill(s,NULL,0xff010203)==0 && pixels[15]==0xff010203);
        free_surface(copy); free_surface(s);
        pixels[0]=0xff123456; /* FreeSurface must not free caller's pixels. */
        s=from_masks(pixels,4,4,32,16,0xff0000,0xff00,0xff,0xff000000);
        CHECK(s && s->format->amask==0xff000000 && s->pixels==pixels);
        free_surface(s);
    }
    CHECK(munmap(high_pixels,4096)==0);
    int (*init)(uint32_t)=sym(lib,"SDL_Init");
    void (*quit)(void)=sym(lib,"SDL_Quit");
    void *(*window)(const char*,int,int,int,int,uint32_t)=sym(lib,"SDL_CreateWindow");
    Surface *(*window_surface)(void*)=sym(lib,"SDL_GetWindowSurface");
    int (*update)(void*)=sym(lib,"SDL_UpdateWindowSurface");
    void (*size)(void*,int,int)=sym(lib,"SDL_SetWindowSize");
    void (*destroy)(void*)=sym(lib,"SDL_DestroyWindow");
    CHECK(init(0x20)==0);
    void *w=window("surface regression",0,0,16,16,8); CHECK(w);
    Surface *s=window_surface(w); CHECK(s && s->w==16 && s->pixels);
    ((uint32_t*)s->pixels)[0]=0xff998877;
    CHECK(window_surface(w)==s && ((uint32_t*)s->pixels)[0]==0xff998877);
    CHECK(update(w)==0);
    size(w,24,20); s=window_surface(w); CHECK(s && s->w==24 && s->h==20);
    CHECK(update(w)==0); destroy(w); quit();
    CHECK(keyboard(NULL)==keys); /* block lifetime survives subsystem restart */
    puts("thunk SDL compatibility passed"); return 0;
}

static int gl_test(void) {
    uint64_t lib=thunk_dlopen("libGL.so.1"); CHECK(lib);
    void (*ip)(unsigned,int,unsigned,int,const void*)=sym(lib,"glVertexAttribIPointer");
    void (*fp)(unsigned,int,unsigned,unsigned char,int,const void*)=sym(lib,"glVertexAttribPointer");
    void (*bind)(unsigned,unsigned)=sym(lib,"glBindBuffer");
    unsigned (*error)(void)=sym(lib,"glGetError");
    int values[4]={10,20,30,40};
    ip(0,4,0x1404,0,values); CHECK(error()==0);
    fp(1,4,0x1404,0,0,values); CHECK(error()==0);
    bind(0x8892,3); ip(0,4,0x1404,0,(void*)16); CHECK(error()==0);
    puts("thunk GL compatibility passed"); return 0;
}
typedef struct Node {uint32_t type; const void *next; uint32_t count; const uint64_t *devices;} Node;
typedef struct {uint32_t type; const void *next; uint32_t flags,queues; const void *queueinfo;
    uint32_t layers; const void *layernames; uint32_t extensions; const void *extnames,*features;} DeviceInfo;
static int vk_test(void) {
    uint64_t lib=thunk_dlopen("libvulkan.so.1"); CHECK(lib);
    int (*create)(uint64_t,const void*,const void*,uint64_t*)=sym(lib,"vkCreateDevice");
    uint64_t physical[1024]; for(int i=0;i<1024;i++) physical[i]=0x1234+i;
    Node node={1000070001,NULL,1,physical};
    DeviceInfo info={3,&node,0,0,NULL,0,NULL,0,NULL,NULL}; uint64_t device=0;
    CHECK(create(0x1234,&info,NULL,&device)==0 && device==0x12345678);
    /* Synthetic repeated nodes exercise >64KiB of nested staging without
       requiring a GPU. The probe validates every array and chain link. */
    Node nodes[8];
    for(int i=0;i<8;i++) nodes[i]=(Node){1000070001,i==7 ? NULL : &nodes[i+1],1024,physical};
    info.next=nodes; CHECK(create(0x1234,&info,NULL,&device)==0 && device==0x12345678);
    nodes[0].count=0x40000000; CHECK(create(0x1234,&info,NULL,&device)!=0);
    nodes[0].count=1; nodes[0].next=&nodes[0];
    CHECK(create(0x1234,&info,NULL,&device)!=0); // cycle must not reach host
    int (*present)(uint64_t,const void*)=sym(lib,"vkQueuePresentKHR");
    struct {uint32_t type; const void *next; uint32_t count; const uint32_t *masks; uint32_t mode;}
        group={1000060011,NULL,1,NULL,1};
    uint32_t mask=1,index=0; uint64_t swapchain=0x5678; group.masks=&mask;
    struct {uint32_t type; const void *next; uint32_t waits; const void *semaphores;
        uint32_t swaps; const uint64_t *swapchains; const uint32_t *indices; void *results;}
        pi={1000001001,&group,0,NULL,1,&swapchain,&index,NULL};
    CHECK(present(0x1234,&pi)==0);
    group.count=0x40000000; CHECK(present(0x1234,&pi)!=0);
    puts("thunk Vulkan compatibility passed"); return 0;
}
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    if(!strcmp(argv[1],"sdl")) return sdl_test();
    if(!strcmp(argv[1],"gl")) return gl_test();
    if(!strcmp(argv[1],"vk")) return vk_test();
    return 2;
}
