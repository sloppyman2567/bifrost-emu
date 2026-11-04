/* test_gles_native_fence.c — EGL_ANDROID_native_fence_sync crosses the guest/host fd boundary.
 *
 * WebView's video path calls eglDupNativeFenceFDANDROID once per frame without
 * checking the extension string, and merges the result with SYNC_IOC_MERGE. The
 * driver's fence is a host sync_file, so the guest must get its own descriptor,
 * sync_file ioctls must carry translated fds and pointers, and importing a fence
 * must hand the driver a host fd and consume the guest one.
 *
 * Build:
 *   make cross SRC=Tests/Apps/test_gles_native_fence.c OUT=Tests/Apps/test_gles_native_fence.elf
 * Run (needs host EGL + GLES 2, GPU access; no window):
 *   ./bifrost-emu Tests/Apps/test_gles_native_fence.elf
 * Exits 77 (skip) without host EGL/GLES or a driver with native fences (llvmpipe).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define EGL_SYNC_FLUSH_COMMANDS_BIT 0x0001
#define EGL_CONDITION_SATISFIED 0x30F6
#define EGL_BAD_PARAMETER 0x300C
#define EGL_FOREVER UINT64_C(0xFFFFFFFFFFFFFFFF)

struct sync_merge_data { char name[32]; int32_t fd2, fence; uint32_t flags, pad; };
struct sync_fence_info { char obj_name[32], driver_name[32]; int32_t status; uint32_t flags; uint64_t timestamp_ns; };
struct sync_file_info { char name[32]; int32_t status; uint32_t flags, num_fences, pad; uint64_t sync_fence_info; };
#define SYNC_IOC_MERGE _IOWR('>', 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR('>', 4, struct sync_file_info)

static uint64_t bifrost_dlopen(const char *path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t handle, const char *name) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define FN(T, name) static __typeof__(T) name;
#define LOAD(h, name) name = (__typeof__(name))(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("SKIP: no %s\n", #name); return 77; }
#define CHECK(condition) do { \
    if (!(condition)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); return 1; } } while (0)

FN(void *(*)(uintptr_t), eglGetDisplay)
FN(unsigned (*)(void *, int *, int *), eglInitialize)
FN(unsigned (*)(unsigned), eglBindAPI)
FN(unsigned (*)(void *, const int *, void **, int, int *), eglChooseConfig)
FN(void *(*)(void *, void *, const int *), eglCreatePbufferSurface)
FN(void *(*)(void *, void *, void *, const int *), eglCreateContext)
FN(unsigned (*)(void *, void *, void *, void *), eglMakeCurrent)
FN(const char *(*)(void *, int), eglQueryString)
FN(int (*)(void), eglGetError)
FN(unsigned (*)(void *), eglTerminate)
FN(void *(*)(const char *), eglGetProcAddress)
FN(void (*)(void), glFlush)

static void *(*eglCreateSyncKHR)(void *, unsigned, const int *);
static void *(*eglCreateSync)(void *, unsigned, const intptr_t *);
static unsigned (*eglDestroySyncKHR)(void *, void *);
static int (*eglClientWaitSyncKHR)(void *, void *, int, uint64_t);
static int (*eglDupNativeFenceFDANDROID)(void *, void *);

static void *display;

/* A new fence over the commands so far, exported to a guest descriptor. */
static int export_fence(void) {
    const int attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, -1, EGL_NONE};
    void *sync = eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (!sync) return -1;
    glFlush();
    int fd = eglDupNativeFenceFDANDROID(display, sync);
    eglDestroySyncKHR(display, sync);
    return fd;
}

/* Importing consumes the guest descriptor and yields a sync on that fence. */
static int imported_fence_works(int fd, int use_attrib) {
    const int attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    const intptr_t wide[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    void *sync = use_attrib ? eglCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, wide)
                            : eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (!sync) { printf("FAIL: import returned no sync (%#x)\n", eglGetError()); return 0; }
    errno = 0;
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) { printf("FAIL: import left the guest fd open\n"); return 0; }
    int status = eglClientWaitSyncKHR(display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
    eglDestroySyncKHR(display, sync);
    if (status != EGL_CONDITION_SATISFIED) { printf("FAIL: imported fence wait %#x\n", status); return 0; }
    return 1;
}

int main(void) {
    uint64_t gles = bifrost_dlopen("libGLESv2.so.2", 1);
    if (!gles) gles = bifrost_dlopen("libGLESv2.so", 1);
    uint64_t egl = bifrost_dlopen("libEGL.so.1", 1);
    if (!egl) egl = bifrost_dlopen("libEGL.so", 1);
    if (!gles || !egl) { printf("SKIP: no libGLESv2/libEGL\n"); return 77; }
    LOAD(egl, eglGetDisplay) LOAD(egl, eglInitialize) LOAD(egl, eglBindAPI) LOAD(egl, eglChooseConfig)
    LOAD(egl, eglCreatePbufferSurface) LOAD(egl, eglCreateContext) LOAD(egl, eglMakeCurrent)
    LOAD(egl, eglQueryString) LOAD(egl, eglGetError) LOAD(egl, eglTerminate) LOAD(egl, eglGetProcAddress)
    LOAD(gles, glFlush)

    display = eglGetDisplay(0);
    if (!display || !eglInitialize(display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("SKIP: no EGL display\n");
        return 77;
    }
    const char *extensions = eglQueryString(display, 0x3055);
    if (!extensions || !strstr(extensions, "EGL_ANDROID_native_fence_sync")) {
        printf("SKIP: driver has no EGL_ANDROID_native_fence_sync\n");
        return 77;
    }
    const int config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    const int surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const int context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    void *config = NULL;
    int count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || !count) { printf("SKIP: no config\n"); return 77; }
    void *surface = eglCreatePbufferSurface(display, config, surface_attrs);
    void *context = eglCreateContext(display, config, NULL, context_attrs);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("SKIP: no pbuffer context\n");
        return 77;
    }
    *(void **)&eglCreateSyncKHR = eglGetProcAddress("eglCreateSyncKHR");
    *(void **)&eglCreateSync = eglGetProcAddress("eglCreateSync");
    *(void **)&eglDestroySyncKHR = eglGetProcAddress("eglDestroySyncKHR");
    *(void **)&eglClientWaitSyncKHR = eglGetProcAddress("eglClientWaitSyncKHR");
    *(void **)&eglDupNativeFenceFDANDROID = eglGetProcAddress("eglDupNativeFenceFDANDROID");
    CHECK(eglCreateSyncKHR && eglCreateSync && eglDestroySyncKHR && eglClientWaitSyncKHR && eglDupNativeFenceFDANDROID);

    /* Export: a guest descriptor that signals, and a real fd for a bad sync. */
    int first = export_fence();
    CHECK(first >= 0);
    CHECK(fcntl(first, F_GETFD) >= 0);
    struct pollfd wait_for = {.fd = first, .events = POLLIN};
    CHECK(poll(&wait_for, 1, 5000) == 1 && (wait_for.revents & POLLIN));
    CHECK(eglDupNativeFenceFDANDROID(display, (void *)(uintptr_t)0x1234) == -1);
    CHECK(eglGetError() == EGL_BAD_PARAMETER);

    /* SYNC_IOC_FILE_INFO: count first, then the array in guest memory (libsync's order). */
    struct sync_file_info info;
    memset(&info, 0, sizeof(info));
    CHECK(ioctl(first, SYNC_IOC_FILE_INFO, &info) == 0);
    CHECK(info.num_fences >= 1 && info.num_fences <= 8);
    struct sync_fence_info fences[8];
    memset(fences, 0, sizeof(fences));
    info.sync_fence_info = (uint64_t)(uintptr_t)fences;
    CHECK(ioctl(first, SYNC_IOC_FILE_INFO, &info) == 0);
    CHECK(info.sync_fence_info == (uint64_t)(uintptr_t)fences);  /* still the guest's pointer */
    CHECK(fences[0].driver_name[0] != 0);
    if (info.num_fences > 1) {
        const uint32_t actual = info.num_fences;
        info.num_fences = actual - 1;
        errno = 0;
        CHECK(ioctl(first, SYNC_IOC_FILE_INFO, &info) == -1 && errno == EINVAL);
    }

    /* SYNC_IOC_MERGE: the second fd and the result are guest descriptors. */
    int second = export_fence();
    CHECK(second >= 0);
    struct sync_merge_data merge;
    memset(&merge, 0, sizeof(merge));
    strcpy(merge.name, "merged");
    merge.fd2 = second;
    CHECK(ioctl(first, SYNC_IOC_MERGE, &merge) == 0);
    CHECK(merge.fd2 == second && merge.fence >= 0 && merge.fence != first && merge.fence != second);
    CHECK(fcntl(merge.fence, F_GETFD) == FD_CLOEXEC);
    wait_for.fd = merge.fence;
    wait_for.revents = 0;
    CHECK(poll(&wait_for, 1, 5000) == 1 && (wait_for.revents & POLLIN));
    CHECK(close(merge.fence) == 0);
    merge.fd2 = 9999;
    errno = 0;
    CHECK(ioctl(first, SYNC_IOC_MERGE, &merge) == -1 && errno == ENOENT);
    int pipes[2];
    CHECK(pipe(pipes) == 0);
    merge.fd2 = second;
    errno = 0;
    CHECK(ioctl(pipes[0], SYNC_IOC_MERGE, &merge) == -1 && errno == ENOTTY);
    close(pipes[0]);
    close(pipes[1]);

    /* Import through both attribute widths; the guest number must never reach the driver. */
    CHECK(imported_fence_works(first, 0));
    CHECK(imported_fence_works(second, 1));

    /* One descriptor per frame: nothing may accumulate. */
    for (int i = 0; i < 2000; ++i) {
        int fd = export_fence();
        CHECK(fd >= 0);
        CHECK(close(fd) == 0);
    }

    eglMakeCurrent(display, NULL, NULL, NULL);
    eglTerminate(display);
    printf("ALL PASS\n");
    return 0;
}
