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
void ANativeWindow_release(void *);

// ActivityProbe's JNI bridge to the managed-window ops of Bifrost's private
// Android syscall 0x100E. Op 2 opens the host window and prepares this
// thread's adapter looper; op 3 pumps host events and returns twenty words.

static long bifrost_window(long op, long a, long b) {
    register long x0 __asm__("x0") = op;
    register long x1 __asm__("x1") = a;
    register long x2 __asm__("x2") = b;
    register long x8 __asm__("x8") = 0x100E;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

JNIEXPORT jint JNICALL Java_ActivityProbe_openWindow(JNIEnv *env, jclass type, jint width,
                                                     jint height) {
    (void)env;
    (void)type;
    return (jint)bifrost_window(2, width, height);
}

JNIEXPORT jint JNICALL Java_ActivityProbe_pollWindow(JNIEnv *env, jclass type, jlongArray out) {
    (void)type;
    uint64_t data[20];
    if ((*env)->GetArrayLength(env, out) != 20)
        return -22; // EINVAL
    long result = bifrost_window(3, (long)data, 0);
    if (result == 0)
        (*env)->SetLongArrayRegion(env, out, 0, 20, (const jlong *)data);
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
    void *window = ANativeWindow_fromSurface(env, surface);
    if (!window) return -19; // ENODEV
    long result = bifrost_window(5, (long)window, opaque != JNI_FALSE);
    if (result) { ANativeWindow_release(window); return result; }
    // The Activity owns this reference until its window closes. EGL and the
    // app take their own references to the same JNI Surface identity.
    return (jlong)(uintptr_t)window;
}

JNIEXPORT void JNICALL Java_ActivityProbe_releaseSurface(JNIEnv *env, jclass type, jlong window) {
    (void)env;
    (void)type;
    ANativeWindow_release((void *)(uintptr_t)window);
}

// The diagnostic entry point and the managed launcher share the bridge.
JNIEXPORT jint JNICALL Java_ManagedLauncher_openWindow(JNIEnv *e, jclass c, jint w, jint h) {
    return Java_ActivityProbe_openWindow(e, c, w, h);
}
JNIEXPORT jint JNICALL Java_ManagedLauncher_pollWindow(JNIEnv *e, jclass c, jlongArray a) {
    return Java_ActivityProbe_pollWindow(e, c, a);
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
    CHECK(eglTerminate(display));
#undef CHECK
#undef FN
    dlclose(lib);
    return 0;
}
