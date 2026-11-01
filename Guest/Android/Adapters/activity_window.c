#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <android/looper.h>
#include <android/choreographer.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    // The managed adapter can call in-process Java services from native
    // workers. The standalone NativeActivity adapter has no such boundary.
    void (*set_vm)(JavaVM *) = dlsym(RTLD_DEFAULT, "bifrost_managed_set_java_vm");
    if (set_vm) set_vm(vm);
    return JNI_VERSION_1_6;
}

struct native_frame_probe {
    JavaVM *vm;
    pthread_t owner;
    AChoreographer *choreographer;
    int result, frames;
    int64_t previous;
};
static void native_frame_ready(int64_t time, void *data) {
    struct native_frame_probe *probe = data;
    if (!pthread_equal(pthread_self(), probe->owner) || time <= probe->previous) {
        probe->result = -4;
        return;
    }
    probe->previous = time;
    if (++probe->frames < 3)
        AChoreographer_postFrameCallback64(probe->choreographer, native_frame_ready, probe);
}
static void *native_frame_worker(void *data) {
    struct native_frame_probe *probe = data;
    JNIEnv *env;
    probe->owner = pthread_self();
    if ((*probe->vm)->GetEnv(probe->vm, (void **)&env, JNI_VERSION_1_6) != JNI_EDETACHED) {
        probe->result = -1;
        return NULL;
    }
    if (!ALooper_prepare(0) || !(probe->choreographer = AChoreographer_getInstance())) {
        probe->result = -2;
        return NULL;
    }
    if ((*probe->vm)->GetEnv(probe->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        probe->result = -3;
        return NULL;
    }
    AChoreographer_postFrameCallback64(probe->choreographer, native_frame_ready, probe);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        ALooper_pollOnce(100, NULL, NULL, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (!probe->result && probe->frames < 3 && now.tv_sec - start.tv_sec < 10);
    if (!probe->result) probe->result = probe->frames == 3 ? 1 : -5;
    return NULL;
}
JNIEXPORT jint JNICALL Java_ActivityProbe_00024NativeLooperProbe_nativeFrames(JNIEnv *env,
        jclass type) {
    (void)type;
    // Exercise thread exit and a second independent native owner as well as
    // the service call. ART aborts on exit if an attached pthread is not detached.
    for (int i = 0; i < 2; ++i) {
        struct native_frame_probe probe = {0};
        if ((*env)->GetJavaVM(env, &probe.vm) != JNI_OK) return -6;
        pthread_t worker;
        if (pthread_create(&worker, NULL, native_frame_worker, &probe)) return -7;
        if (pthread_join(worker, NULL)) return -8;
        if (probe.result != 1) return probe.result;
    }
    return 1;
}

// An unattached pthread transacting on a Java Binder in this process, as native code reaches the launcher's Java-hosted
// services (the NDK's AIBinder ends in the same JavaBBinder::onTransact). libbifrost-binder-attach.so must attach it, or
// that aborts the process.
struct native_java_binder_probe {
    JavaVM *vm;
    void *binder;
    int result;
    int32_t (*prepare)(void *, void **);
    int32_t (*transact)(void *, uint32_t, void **, void **, uint32_t);
    void (*delete_parcel)(void *);
};
static void *native_java_binder_worker(void *data) {
    struct native_java_binder_probe *probe = data;
    JNIEnv *env;
    if ((*probe->vm)->GetEnv(probe->vm, (void **)&env, JNI_VERSION_1_6) != JNI_EDETACHED) { probe->result = -1; return NULL; }
    void *request = NULL, *reply = NULL;
    if (probe->prepare(probe->binder, &request)) { probe->result = -2; return NULL; }
    const int32_t status = probe->transact(probe->binder, 1, &request, &reply, 0);  // FIRST_CALL_TRANSACTION
    if (reply) probe->delete_parcel(reply);
    if (status) { probe->result = -3; return NULL; }
    probe->result = (*probe->vm)->GetEnv(probe->vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK ? 1 : -4;
    return NULL;
}
static void *native_java_binder_create(void *arguments) { return arguments; }
static void native_java_binder_destroy(void *data) { (void)data; }
static int32_t native_java_binder_transact(void *binder, uint32_t code, const void *in, void *out) {
    (void)binder; (void)code; (void)in; (void)out;
    return 0;
}
JNIEXPORT jint JNICALL Java_ActivityProbe_00024NativeLooperProbe_nativeJavaBinder(JNIEnv *env, jclass type, jobject binder) {
    (void)type;
    struct native_java_binder_probe probe = {0};
    if ((*env)->GetJavaVM(env, &probe.vm) != JNI_OK) return -6;
    void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!ndk) return -7;
    void *(*from_java)(JNIEnv *, jobject) = dlsym(ndk, "AIBinder_fromJavaBinder");
    void *(*define)(const char *, void *(*)(void *), void (*)(void *),
                    int32_t (*)(void *, uint32_t, const void *, void *)) = dlsym(ndk, "AIBinder_Class_define");
    bool (*associate)(void *, const void *) = dlsym(ndk, "AIBinder_associateClass");
    probe.prepare = dlsym(ndk, "AIBinder_prepareTransaction");
    probe.transact = dlsym(ndk, "AIBinder_transact");
    probe.delete_parcel = dlsym(ndk, "AParcel_delete");
    if (!from_java || !define || !associate || !probe.prepare || !probe.transact || !probe.delete_parcel) return -8;
    // This thread is attached: associating a class asks the Java object for its descriptor.
    probe.binder = from_java(env, binder);
    const void *binder_class = define("org.bifrost.probe", native_java_binder_create, native_java_binder_destroy,
                                      native_java_binder_transact);
    if (!probe.binder || !binder_class || !associate(probe.binder, binder_class)) return -9;
    // Two threads: ART aborts at thread exit if the first was left attached.
    for (int i = 0; i < 2; ++i) {
        pthread_t worker;
        probe.result = 0;
        if (pthread_create(&worker, NULL, native_java_binder_worker, &probe) || pthread_join(worker, NULL)) return -10;
        if (probe.result != 1) return probe.result;
    }
    return 1;
}

struct native_looper_registration { ALooper *looper; int fd, result; };
static int native_looper_ready(int fd, int events, void *data) {
    struct native_looper_registration *registration = data;
    uint64_t value;
    registration->result = ALooper_forThread() == registration->looper
            && (events & ALOOPER_EVENT_INPUT) && read(fd, &value, sizeof(value)) == sizeof(value)
            && value == 1 ? 1 : -1;
    return 0; // one-shot registration
}
JNIEXPORT jlong JNICALL Java_ActivityProbe_00024NativeLooperProbe_arm(JNIEnv *env, jclass type) {
    (void)env; (void)type;
    struct native_looper_registration *registration = calloc(1, sizeof(*registration));
    if (!registration) return 0;
    registration->looper = ALooper_prepare(0);
    registration->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!registration->looper || registration->fd < 0) goto failed;
    if (ALooper_addFd(registration->looper, registration->fd, 0, ALOOPER_EVENT_INPUT,
            native_looper_ready, registration) != 1) goto failed;
    uint64_t value = 1;
    if (write(registration->fd, &value, sizeof(value)) != sizeof(value)) {
        ALooper_removeFd(registration->looper, registration->fd);
        goto failed;
    }
    ALooper_acquire(registration->looper);
    return (jlong)(uintptr_t)registration;
failed:
    if (registration->fd >= 0) close(registration->fd);
    free(registration);
    return 0;
}
JNIEXPORT jint JNICALL Java_ActivityProbe_00024NativeLooperProbe_finish(JNIEnv *env, jclass type,
        jlong handle) {
    (void)env; (void)type;
    struct native_looper_registration *registration = (void *)(uintptr_t)handle;
    int result = ALooper_forThread() == registration->looper ? registration->result : -2;
    ALooper_removeFd(registration->looper, registration->fd);
    ALooper_release(registration->looper);
    close(registration->fd);
    free(registration);
    return result;
}

void *ANativeWindow_fromSurface(JNIEnv *, jobject);
void *bifrost_surface_prepare(JNIEnv *, jobject) __attribute__((weak));
void bifrost_surface_release(void *) __attribute__((weak));
void ANativeWindow_release(void *);

static void release_surface(void *window) {
    if (bifrost_surface_release) bifrost_surface_release(window);
    else ANativeWindow_release(window);
}

// ActivityProbe's JNI bridge to the managed-window ops of Bifrost's private
// Android syscall 0x100E. Op 2 opens the host window and prepares this
// thread's adapter looper; op 3 pumps host events and returns 55 words (up to
// eight touch pointers, or a text payload: kind in word 4, UTF-8 length in
// word 13, bytes from word 14).

static long bifrost_window(long op, long a, long b) {
    register long x0 __asm__("x0") = op;
    register long x1 __asm__("x1") = a;
    register long x2 __asm__("x2") = b;
    register long x8 __asm__("x8") = 0x100E;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}
JNIEXPORT jint JNICALL Java_ManagedLauncher_processState(JNIEnv *env, jclass type, jint state) {
    (void)env; (void)type;
    return (jint)bifrost_window(14, state, 0);
}

// Starts or stops the host's text input (op 20); `x`, `y`, `width`, `height` anchor
// its composition in window pixels. Text then arrives as op 3 events.
JNIEXPORT jint JNICALL Java_ManagedLauncher_setTextInput(JNIEnv *env, jclass type, jboolean active,
                                                         jint x, jint y, jint width, jint height) {
    (void)env; (void)type;
    int64_t record[5] = {active ? 1 : 0, x, y, width, height};
    return (jint)bifrost_window(20, (long)record, 0);
}

JNIEXPORT jint JNICALL Java_ActivityProbe_openWindow(JNIEnv *env, jclass type, jint width,
                                                     jint height) {
    (void)env;
    (void)type;
    return (jint)bifrost_window(2, width, height);
}

JNIEXPORT jint JNICALL Java_ManagedLauncher_setTaskVisible(JNIEnv *env, jclass type, jboolean visible) {
    (void)env; (void)type;
    return (jint)bifrost_window(22, visible != JNI_FALSE, 0);
}

// The pointer shape over the window (op 24): a BIFROST_WINDOW_CURSOR_* value.
JNIEXPORT jint JNICALL Java_ManagedLauncher_setCursor(JNIEnv *env, jclass type, jint cursor) {
    (void)env; (void)type;
    return (jint)bifrost_window(24, cursor, 0);
}

JNIEXPORT jint JNICALL Java_ActivityProbe_pollWindow(JNIEnv *env, jclass type, jlongArray out) {
    (void)type;
    uint64_t data[55];
    if ((*env)->GetArrayLength(env, out) != 55)
        return -22; // EINVAL
    long result = bifrost_window(3, (long)data, 0);
    if (result == 0)
        (*env)->SetLongArrayRegion(env, out, 0, 55, (const jlong *)data);
    return (jint)result;
}

JNIEXPORT jint JNICALL Java_ActivityProbe_audioConfig(JNIEnv *env, jclass type, jlongArray out) {
    (void)type;
    uint64_t data[3];
    if ((*env)->GetArrayLength(env, out) != 3) return -22;
    long result = bifrost_window(4, (long)data, 0);
    if (result == 0)
        (*env)->SetLongArrayRegion(env, out, 0, 3, (const jlong *)data);
    return (jint)result;
}

JNIEXPORT jlong JNICALL Java_ActivityProbe_prepareSurface(JNIEnv *env, jclass type,
                                                         jobject surface, jboolean opaque) {
    (void)type;
    void *window = bifrost_surface_prepare ? bifrost_surface_prepare(env, surface) : ANativeWindow_fromSurface(env, surface);
    if (!window) return -19; // ENODEV
    long result = bifrost_window(5, (long)window, opaque != JNI_FALSE);
    if (result) { release_surface(window); return result; }
    // The Activity owns this reference until its window closes. EGL surfaces
    // take their own host references to the same JNI Surface identity.
    return (jlong)(uintptr_t)window;
}

JNIEXPORT void JNICALL Java_ActivityProbe_releaseSurface(JNIEnv *env, jclass type, jlong window) {
    (void)env;
    (void)type;
    release_surface((void *)(uintptr_t)window);
}

// Framework popup windows (dialogs, popup menus): ops 9/10 copy straight-alpha
// ARGB rows drawn by FrameworkWindows into a host layer above every surface.
JNIEXPORT jint JNICALL Java_FrameworkWindows_showPopup(JNIEnv *env, jclass type, jint id, jint x,
        jint y, jint width, jint height, jintArray pixels) {
    (void)type;
    if (width <= 0 || height <= 0 ||
        (*env)->GetArrayLength(env, pixels) < (jsize)((int64_t)width * height))
        return -22; // EINVAL
    jint *argb = (*env)->GetPrimitiveArrayCritical(env, pixels, NULL);
    if (!argb) return -12; // ENOMEM
    int64_t record[6] = {id, x, y, width, height, (int64_t)(uintptr_t)argb};
    long result = bifrost_window(9, (long)record, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, pixels, argb, JNI_ABORT);
    return (jint)result;
}

JNIEXPORT jint JNICALL Java_FrameworkWindows_removePopup(JNIEnv *env, jclass type, jint id) {
    (void)env;
    (void)type;
    return (jint)bifrost_window(10, id, 0);
}

// The diagnostic entry point and the managed launcher share the bridge.
JNIEXPORT jint JNICALL Java_ManagedLauncher_openWindow(JNIEnv *e, jclass c, jint w, jint h) {
    return Java_ActivityProbe_openWindow(e, c, w, h);
}
JNIEXPORT jint JNICALL Java_ManagedLauncher_pollWindow(JNIEnv *e, jclass c, jlongArray a) {
    return Java_ActivityProbe_pollWindow(e, c, a);
}
// The open window's display: pixel width, pixel height and density dpi.
JNIEXPORT jint JNICALL Java_ManagedLauncher_windowMetrics(JNIEnv *env, jclass type, jlongArray out) {
    (void)type;
    uint64_t data[3];
    if ((*env)->GetArrayLength(env, out) != 3) return -22; // EINVAL
    long result = bifrost_window(11, (long)data, 0);
    if (result == 0)
        (*env)->SetLongArrayRegion(env, out, 0, 3, (const jlong *)data);
    return (jint)result;
}
// The display holding the open window: pixel width and height (max bounds).
JNIEXPORT jint JNICALL Java_ManagedLauncher_displaySize(JNIEnv *env, jclass type, jlongArray out) {
    (void)type;
    uint64_t data[2];
    if ((*env)->GetArrayLength(env, out) != 2) return -22; // EINVAL
    long result = bifrost_window(12, (long)data, 0);
    if (result == 0)
        (*env)->SetLongArrayRegion(env, out, 0, 2, (const jlong *)data);
    return (jint)result;
}
// A non-resizable app keeps this pixel size; the host window is scaled to fit (op 21).
JNIEXPORT jint JNICALL Java_ManagedLauncher_fixWindow(JNIEnv *env, jclass type, jint width, jint height) {
    (void)env; (void)type;
    return (jint)bifrost_window(21, width, height);
}
JNIEXPORT jint JNICALL Java_ManagedLauncher_orientWindow(JNIEnv *env, jclass type, jint axis) {
    (void)env; (void)type;
    return (jint)bifrost_window(23, axis, 0);
}
JNIEXPORT jint JNICALL Java_ManagedLauncher_audioConfig(JNIEnv *e, jclass c, jlongArray a) {
    return Java_ActivityProbe_audioConfig(e, c, a);
}
// Started (unpaused) host output streams; AAudio starts/stops map onto these.
JNIEXPORT jint JNICALL Java_ManagedLauncher_activeAudioOutputs(JNIEnv *e, jclass c) {
    (void)e;
    (void)c;
    return (jint)bifrost_window(8, 0, 0);
}
JNIEXPORT jlong JNICALL Java_ManagedLauncher_prepareSurface(JNIEnv *e, jclass c, jobject s, jboolean o) {
    return Java_ActivityProbe_prepareSurface(e, c, s, o);
}
JNIEXPORT void JNICALL Java_ManagedLauncher_releaseSurface(JNIEnv *e, jclass c, jlong w) {
    Java_ActivityProbe_releaseSurface(e, c, w);
}

JNIEXPORT jint JNICALL Java_FrameworkSurfaces_present(JNIEnv *env, jclass type, jlong window,
        jint x, jint y, jint width, jint height, jintArray pixels) {
    (void)type;
    if (!pixels || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
            (*env)->GetArrayLength(env, pixels) != (int64_t)width * height) return -22;
    jint *argb = (*env)->GetPrimitiveArrayCritical(env, pixels, NULL);
    if (!argb) return -12;
    int64_t record[] = {window, x, y, width, height, (intptr_t)argb};
    long result = bifrost_window(16, (long)record, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, pixels, argb, JNI_ABORT);
    return result;
}
// A video frame: the image's three planes (direct buffers) go to the host, which converts the visible region of the
// layer to ARGB and presents it (op 25). One call per frame, no pixel array crosses the guest.
JNIEXPORT jint JNICALL Java_FrameworkSurfaces_presentYuv(JNIEnv *env, jclass type, jlong window, jint x, jint y,
        jint width, jint height, jint layerWidth, jint layerHeight, jint left, jint top,
        jint cropLeft, jint cropTop, jint cropWidth, jint cropHeight, jobject yPlane, jobject uPlane, jobject vPlane,
        jint yRow, jint uRow, jint vRow, jint yPixel, jint uPixel, jint vPixel, jint matrix, jint fullRange) {
    (void)type;
    jobject planes[3] = {yPlane, uPlane, vPlane};
    int64_t addresses[3], sizes[3];
    for (int i = 0; i < 3; ++i) {
        if (!planes[i]) return -22;
        char *base = (*env)->GetDirectBufferAddress(env, planes[i]);
        jlong capacity = (*env)->GetDirectBufferCapacity(env, planes[i]);
        if (!base || capacity <= 0) return -22;
        addresses[i] = (intptr_t)base;
        sizes[i] = capacity;
    }
    int64_t record[27] = {window, x, y, width, height, layerWidth, layerHeight, left, top,
            cropLeft, cropTop, cropWidth, cropHeight, addresses[0], addresses[1], addresses[2],
            sizes[0], sizes[1], sizes[2], yRow, uRow, vRow, yPixel, uPixel, vPixel, matrix, fullRange};
    return (jint)bifrost_window(25, (long)record, 0);
}
JNIEXPORT jint JNICALL Java_FrameworkSurfaces_geometry(JNIEnv *env, jclass type, jlong window,
        jint x, jint y, jint width, jint height) {
    (void)env; (void)type;
    int64_t record[] = {window, x, y, width, height};
    return bifrost_window(18, (long)record, 0);
}

// Window order: host surface `window` directly below `sibling` (op 19).
JNIEXPORT jint JNICALL Java_FrameworkSurfaces_placeBelow(JNIEnv *env, jclass type, jlong window, jlong sibling) {
    (void)env; (void)type;
    return bifrost_window(19, (long)window, (long)sibling);
}

JNIEXPORT jint JNICALL Java_ManagedLauncher_setWindowIdentity(JNIEnv *env, jclass type,
        jbyteArray title, jintArray argb, jint width, jint height) {
    (void)type;
    if (!title || width < 0 || height < 0 || width > 256 || height > 256 ||
            (argb && (!width || !height || (*env)->GetArrayLength(env, argb) != width * height)))
        return -22;
    jsize length = (*env)->GetArrayLength(env, title);
    if (length >= 1024) return -22;
    char *text = calloc((size_t)length + 1, 1);
    if (!text) return -12;
    (*env)->GetByteArrayRegion(env, title, 0, length, (jbyte *)text);
    if ((*env)->ExceptionCheck(env)) { free(text); return -12; }
    uint8_t *rgba = NULL;
    if (argb) {
        rgba = malloc((size_t)width * height * 4);
        if (!rgba) { free(text); return -12; }
        jint *pixels = (*env)->GetIntArrayElements(env, argb, NULL);
        if (!pixels) { free(rgba); free(text); return -12; }
        for (int i = 0; i < width * height; ++i) {
            uint32_t p = (uint32_t)pixels[i];
            rgba[i * 4] = p >> 16; rgba[i * 4 + 1] = p >> 8;
            rgba[i * 4 + 2] = p; rgba[i * 4 + 3] = p >> 24;
        }
        (*env)->ReleaseIntArrayElements(env, argb, pixels, JNI_ABORT);
    }
    uint64_t record[] = {(uintptr_t)text, (uintptr_t)rgba, width, height};
    long result = bifrost_window(6, (long)record, 0);
    free(rgba);
    free(text);
    return result;
}

static void cache_set(const void *key, intptr_t key_size, const void *value, intptr_t value_size) {
    (void)key; (void)key_size; (void)value; (void)value_size;
}
static intptr_t cache_get(const void *key, intptr_t key_size, void *value, intptr_t value_size) {
    (void)key; (void)key_size; (void)value; (void)value_size; return 0;
}
JNIEXPORT jint JNICALL Java_ActivityProbe_00024EglProbe_check(JNIEnv *env, jclass type) {
    (void)env; (void)type;
    if (bifrost_window(2, 320, 240)) return __LINE__;
    void *lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return __LINE__;
#define FN(result, name, args) result (*name) args = (result (*) args)dlsym(lib, #name); if (!name) return __LINE__
#define CHECK(test) if (!(test)) return __LINE__
    FN(void *, eglGetDisplay, (void *));
    FN(unsigned, eglInitialize, (void *, int *, int *));
    FN(unsigned, eglTerminate, (void *));
    FN(int, eglGetError, (void));
    FN(const char *, eglQueryString, (void *, int));
    FN(void *, eglGetProcAddress, (const char *));
    FN(void, eglSetBlobCacheFuncsANDROID, (void *, void *, void *));
    FN(void *, eglGetPlatformDisplay, (unsigned, void *, const intptr_t *));
    FN(uint64_t, eglGetSystemTimeNV, (void));
    FN(uint64_t, eglGetSystemTimeFrequencyNV, (void));
    void *display = eglGetDisplay(NULL);
    CHECK(display && eglInitialize(display, NULL, NULL));
    (void)eglGetError();
    const char *extensions = eglQueryString(display, 0x3055);
    CHECK(extensions && strstr(extensions, "EGL_ANDROID_blob_cache"));
    CHECK(!strstr(extensions, "EGL_ANDROID_native_fence_sync") && !strstr(extensions, "EGL_KHR_stream"));
    CHECK(eglGetProcAddress("eglSetBlobCacheFuncsANDROID") == (void *)eglSetBlobCacheFuncsANDROID);
    eglSetBlobCacheFuncsANDROID(display, NULL, (void *)cache_get);
    CHECK(eglGetError() == 0x300C && eglGetError() == 0x3000);
    eglSetBlobCacheFuncsANDROID(display, (void *)cache_set, (void *)cache_get);
    CHECK(eglGetError() == 0x3000);
    eglSetBlobCacheFuncsANDROID(display, (void *)cache_set, (void *)cache_get);
    CHECK(eglGetError() == 0x300C);
    CHECK(eglGetPlatformDisplay(0x3141, NULL, NULL) == display);
    const intptr_t invalid[] = {123, 0, 0x3038};
    CHECK(!eglGetPlatformDisplay(0x3141, NULL, invalid) && eglGetError() == 0x3004);
    CHECK(eglGetSystemTimeFrequencyNV() == 1000000000 && eglGetSystemTimeNV() > 0);
    CHECK(eglTerminate(display) && eglInitialize(display, NULL, NULL));
    eglSetBlobCacheFuncsANDROID(display, (void *)cache_set, (void *)cache_get);
    CHECK(eglGetError() == 0x3000);
    // Android ref-counts initialization: another user's terminate leaves the
    // display initialized; the last one terminates it (EGL_NOT_INITIALIZED).
    CHECK(eglInitialize(display, NULL, NULL) && eglTerminate(display));
    CHECK(eglQueryString(display, 0x3054) && eglGetError() == 0x3000);  // EGL_VERSION
    CHECK(eglTerminate(display));
    CHECK(!eglQueryString(display, 0x3054) && eglGetError() == 0x3001);
    CHECK(eglTerminate(display));
#undef CHECK
#undef FN
    dlclose(lib);
    return 0;
}
JNIEXPORT jlong JNICALL Java_ActivityProbe_00024EglProbe_prepare(JNIEnv *env, jclass type, jobject surface) {
    return Java_ActivityProbe_prepareSurface(env, type, surface, JNI_TRUE);
}
JNIEXPORT void JNICALL Java_ActivityProbe_00024EglProbe_release(JNIEnv *env, jclass type, jlong window) {
    Java_ActivityProbe_releaseSurface(env, type, window);
}
// HWUI's setSurface (android_graphics_HardwareRenderer.cpp): the window
// comes from libandroid's ANativeWindow_fromSurface and goes back through
// libnativewindow's ANativeWindow_release.
JNIEXPORT jint JNICALL Java_ActivityProbe_00024EglProbe_hwuiSurface(JNIEnv *env, jclass type, jobject surface) {
    (void)type;
    void *android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    void *nativewindow = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    void *(*from)(JNIEnv *, jobject) = android ? (void *(*)(JNIEnv *, jobject))dlsym(android, "ANativeWindow_fromSurface") : NULL;
    void (*release)(void *) = nativewindow ? (void (*)(void *))dlsym(nativewindow, "ANativeWindow_release") : NULL;
    void *window = from && release ? from(env, surface) : NULL;
    if (window) release(window);
    if (android) dlclose(android);
    if (nativewindow) dlclose(nativewindow);
    return window ? 0 : __LINE__;
}
// Whether a prepared window still has its host backing (op 17 query).
JNIEXPORT jboolean JNICALL Java_ActivityProbe_00024EglProbe_attached(JNIEnv *env, jclass type, jlong window) {
    (void)env; (void)type;
    return bifrost_window(17, (long)window, 0) > 0;
}
