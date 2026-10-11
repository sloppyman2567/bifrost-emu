#include "../../Guest/Android/native_activity.h"
#include <android/log.h>
#include <zlib.h>
#include <media/NdkMediaFormat.h>
#define main service_manager_checks
#include "../Unit/android_service_manager.c"
#undef main
// A real Bionic-linked NativeActivity loaded from a signed split APK. Expected
// pixels, TLS, asset bytes and lifecycle order are independent result oracles.
extern void *eglGetDisplay(void *);
extern unsigned int eglInitialize(void *, int *, int *);
extern unsigned int eglChooseConfig(void *, const int *, void **, int, int *);
extern unsigned int eglBindAPI(unsigned int);
extern unsigned int eglGetError(void);
extern void *eglCreateWindowSurface(void *, void *, void *, const int *);
extern void *eglCreateContext(void *, void *, void *, const int *);
extern unsigned int eglMakeCurrent(void *, void *, void *, void *);
extern unsigned int eglSwapBuffers(void *, void *);
extern unsigned int eglDestroySurface(void *, void *);
extern unsigned int eglDestroyContext(void *, void *);
extern unsigned int eglTerminate(void *);
extern void glViewport(int, int, int, int);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned int);
extern void glReadPixels(int, int, int, int, unsigned int, unsigned int, void *);
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);
static __thread int tls = 17;
static int initialized, phase, presented;
static void *display;
static void *surface;
static void *context;
extern int fixture_dependency_value(void);
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("APK fixture failed line %d\n", __LINE__);                                      \
            fflush(0);                                                                             \
            _exit(1);                                                                              \
        }                                                                                          \
    } while (0)
static int log_variadic(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = __android_log_vprint(4, "apk-fixture", format, args);
    va_end(args);
    return result;
}
static void media_format_checks(void) {
    // Real AOSP implementation, loaded in the platform namespace. Exercise
    // ownership/type behavior repeatedly without contacting media services.
    for (int iteration = 0; iteration < 256; ++iteration) {
        AMediaFormat *format = AMediaFormat_new(), *copy = AMediaFormat_new();
        REQUIRE(format && copy);
        char mime[] = "video/avc";
        unsigned char bytes[] = {0, 0, 1, 103};
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
        AMediaFormat_setInt64(format, AMEDIAFORMAT_KEY_DURATION, 1234567890123LL);
        AMediaFormat_setFloat(format, "float", 1.25f);
        AMediaFormat_setDouble(format, "double", 2.5);
        AMediaFormat_setSize(format, "size", 4096);
        AMediaFormat_setRect(format, "crop", 1, 2, 3, 4);
        AMediaFormat_setBuffer(format, "csd-0", bytes, sizeof(bytes));
        mime[0] = 'x'; bytes[3] = 0;
        const char *text = 0;
        void *buffer = 0;
        size_t size = 0;
        int32_t width = 0, left, top, right, bottom;
        int64_t duration = 0;
        float small = 0;
        double large = 0;
        REQUIRE(AMediaFormat_getString(format, "mime", &text) && !strcmp(text, "video/avc"));
        REQUIRE(AMediaFormat_getInt32(format, "width", &width) && width == 1920);
        REQUIRE(AMediaFormat_getInt64(format, "durationUs", &duration) && duration == 1234567890123LL);
        REQUIRE(!AMediaFormat_getInt64(format, "width", &duration) && duration == 1234567890123LL);
        REQUIRE(AMediaFormat_getFloat(format, "float", &small) && small == 1.25f);
        REQUIRE(AMediaFormat_getDouble(format, "double", &large) && large == 2.5);
        REQUIRE(AMediaFormat_getSize(format, "size", &size) && size == 4096);
        REQUIRE(AMediaFormat_getRect(format, "crop", &left, &top, &right, &bottom) &&
                left == 1 && top == 2 && right == 3 && bottom == 4);
        REQUIRE(AMediaFormat_getBuffer(format, "csd-0", &buffer, &size) && size == 4 &&
                ((unsigned char *)buffer)[3] == 103);
        REQUIRE(AMediaFormat_copy(copy, format) == AMEDIA_OK);
        AMediaFormat_setString(format, "mime", "changed");
        AMediaFormat_clear(format);
        REQUIRE(!AMediaFormat_getInt32(format, "width", &width));
        REQUIRE(AMediaFormat_getString(copy, "mime", &text) && !strcmp(text, "video/avc"));
        REQUIRE(AMediaFormat_getBuffer(copy, "csd-0", &buffer, &size) && size == 4 &&
                ((unsigned char *)buffer)[3] == 103);
        text = AMediaFormat_toString(copy);
        REQUIRE(text && strstr(text, "1920") && strstr(text, "video/avc"));
        REQUIRE(AMediaFormat_delete(format) == AMEDIA_OK && AMediaFormat_delete(copy) == AMEDIA_OK);
    }
    printf("apk_media_fixture: REAL FORMAT PASS\n");
}
__attribute__((constructor)) static void constructor(void) {
    media_format_checks();
    REQUIRE(fixture_dependency_value() == 41);
    REQUIRE(log_variadic("abi=%d %.2f %s %lld %d %d %d %d %d %d %d %d", 42, 1.25,
                         "guest", 1234567890123LL, 1, 2, 3, 4, 5, 6, 7, 8) == 1);
    REQUIRE(__android_log_buf_print(0, 4, "apk-fixture", "buffer=%.2f %s", 2.5, "ok") == 1);
    // Standard CRC oracle and a stream generated outside the guest implementation.
    REQUIRE(crc32(0, (const unsigned char *)"123456789", 9) == 0xcbf43926UL);
    unsigned char compressed[128], decoded[32];
    unsigned long size = sizeof(compressed), restored = sizeof(decoded);
    REQUIRE(compress(compressed, &size, (const unsigned char *)"guest zlib roundtrip", 21) == Z_OK);
    REQUIRE(uncompress(decoded, &restored, compressed, size) == Z_OK && restored == 21 &&
            !memcmp(decoded, "guest zlib roundtrip", 21));
    static const unsigned char stream[] = {120, 156, 75, 47, 77, 45, 46, 81, 168, 202, 201,
                                          76, 82, 40, 202, 47, 205, 75, 41, 41, 202, 44, 0,
                                          0, 82, 211, 8, 1};
    restored = sizeof(decoded);
    REQUIRE(uncompress(decoded, &restored, stream, sizeof(stream)) == Z_OK && restored == 20 &&
            !memcmp(decoded, "guest zlib roundtrip", 20));
    initialized = 41;
}
static void *worker(void *p) {
    (void)p;
    REQUIRE(tls == 17);
    tls = 29;
    return (void *)(uintptr_t)tls;
}
static void started(ANativeActivity *a) {
    (void)a;
    REQUIRE(phase == 0);
    phase = 1;
}
static void resumed(ANativeActivity *a) {
    (void)a;
    REQUIRE(phase == 1);
    phase = 2;
}
static void window_created(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    REQUIRE(phase == 2);
    phase = 3;
    display = eglGetDisplay(0);
    REQUIRE(display && eglInitialize(display, 0, 0));
    int attributes[] = {0x3033, 4, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038};
    void *config = 0;
    int count = 0;
    REQUIRE(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    REQUIRE(eglBindAPI(0x30a2));
    surface = eglCreateWindowSurface(display, config, window, 0);
    if (!surface) {
        printf("APK surface: window=%p EGL error=0x%x\n", (void *)window, eglGetError());
    }
    REQUIRE(surface);
    context = eglCreateContext(display, config, 0, 0);
    REQUIRE(context && eglMakeCurrent(display, surface, surface, context));
}
static void redraw(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    REQUIRE(phase == 3);
    glViewport(0, 0, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000);
    unsigned char pixel[4] = {0};
    glReadPixels(1, 1, 1, 1, 0x1908, 0x1401, pixel);
    REQUIRE(pixel[0] >= 62 && pixel[0] <= 66 && pixel[1] >= 126 && pixel[1] <= 130 &&
            pixel[2] >= 189 && pixel[2] <= 193);
    REQUIRE(eglSwapBuffers(display, surface));
    ++presented;
}
static void paused(ANativeActivity *a) {
    (void)a;
    REQUIRE(phase == 3 && presented >= 1);
    phase = 4;
}
static void window_destroyed(ANativeActivity *a, ANativeWindow *w) {
    (void)a;
    (void)w;
    REQUIRE(phase == 4);
    REQUIRE(eglMakeCurrent(display, 0, 0, 0));
    REQUIRE(eglDestroySurface(display, surface) && eglDestroyContext(display, context) &&
            eglTerminate(display));
    phase = 5;
}
static void stopped(ANativeActivity *a) {
    (void)a;
    REQUIRE(phase == 5);
    phase = 6;
}
static void destroyed(ANativeActivity *a) {
    (void)a;
    REQUIRE(phase == 6);
    printf("apk_bionic_fixture: ALL PASS\n");
}
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a,
                                                                     void *saved, size_t size) {
    REQUIRE(initialized == 41 && tls == 17 && !saved && !size);
    REQUIRE(a && !a->vm && !a->env && !a->clazz && a->assetManager && a->sdkVersion == 35);
    REQUIRE(strncmp(a->internalDataPath, "/data/user/0/org.bifrost.fixture/",
                    strlen("/data/user/0/org.bifrost.fixture/")) == 0);
    tls = 31;
    unsigned long thread;
    void *result = 0;
    REQUIRE(!pthread_create(&thread, 0, worker, 0) && !pthread_join(thread, &result) &&
            result == (void *)29 && tls == 31);
    AAsset *asset = AAssetManager_open(a->assetManager, "split.txt", 2);
    REQUIRE(asset);
    REQUIRE(AAsset_getLength(asset) == 16 &&
            !memcmp(AAsset_getBuffer(asset), "APK split asset\n", 16));
    char first[4] = {0};
    REQUIRE(AAsset_read(asset, first, 3) == 3 && !strcmp(first, "APK"));
    AAsset_close(asset);
    REQUIRE(!AAssetManager_open(a->assetManager, "../base.apk", 0));
    char file[256];
    snprintf(file, sizeof(file), "%s/save", a->internalDataPath);
    int fd = open(file, 2 | 64, 0600);
    REQUIRE(fd >= 0);
    unsigned char value = 0;
    long n = read(fd, &value, 1);
    REQUIRE(n == 0 || n == 1);
    ++value;
    REQUIRE(lseek(fd, 0, 0) == 0 && write(fd, &value, 1) == 1 && !close(fd));
    REQUIRE(!service_manager_checks());
    a->callbacks->onStart = started;
    a->callbacks->onResume = resumed;
    a->callbacks->onPause = paused;
    a->callbacks->onStop = stopped;
    a->callbacks->onDestroy = destroyed;
    a->callbacks->onNativeWindowCreated = window_created;
    a->callbacks->onNativeWindowRedrawNeeded = redraw;
    a->callbacks->onNativeWindowDestroyed = window_destroyed;
}
