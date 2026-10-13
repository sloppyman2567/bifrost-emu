// Managed apps share libutils' thread-local Looper with Java MessageQueue.
// Keep explicit public exports: Bionic's linked public-library namespaces do
// not expose every symbol in an adapter's private dependency to JNI clients.
#include <android/looper.h>
#include <android/choreographer.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_once_t platform_once = PTHREAD_ONCE_INIT;
static void *platform;
static _Atomic(JavaVM *) managed_vm;
static pthread_once_t attachment_once = PTHREAD_ONCE_INIT;
static pthread_key_t attachment_key;

void bifrost_managed_set_java_vm(JavaVM *vm) {
    atomic_store_explicit(&managed_vm, vm, memory_order_release);
}
static void detach_java_thread(void *value) {
    JavaVM *vm = value;
    (*vm)->DetachCurrentThread(vm);
}
static void create_attachment_key(void) {
    if (pthread_key_create(&attachment_key, detach_java_thread)) {
        fprintf(stderr, "Managed choreographer: cannot create JVM attachment key\n");
        abort();
    }
}
static void attach_java_thread(void) {
    JavaVM *vm = atomic_load_explicit(&managed_vm, memory_order_acquire);
    if (!vm) return; // No Java services in a standalone native process.
    JNIEnv *env;
    jint status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_OK) return; // Never detach an attachment owned by the app/ART.
    pthread_once(&attachment_once, create_attachment_key);
    if (status != JNI_EDETACHED ||
            (*vm)->AttachCurrentThreadAsDaemon(vm, &env, NULL) != JNI_OK) {
        fprintf(stderr, "Managed choreographer: cannot attach native JVM thread\n");
        abort();
    }
    if (pthread_setspecific(attachment_key, vm)) {
        (*vm)->DetachCurrentThread(vm);
        fprintf(stderr, "Managed choreographer: cannot track JVM attachment\n");
        abort();
    }
}

static void open_platform(void) {
    platform = dlopen("re_android.so", RTLD_NOW | RTLD_LOCAL);
    if (!platform) {
        fprintf(stderr, "Managed looper: %s\n", dlerror());
        abort();
    }
}
// Also resolves the generated forwarders for the other public LIBANDROID
// exports (build-android-adapters.py), which cache the result lazily.
__attribute__((visibility("hidden"))) void *bifrost_platform_symbol(const char *name) {
    pthread_once(&platform_once, open_platform);
    void *target = dlsym(platform, name);
    if (!target) {
        fprintf(stderr, "Managed libandroid: missing %s\n", name);
        abort();
    }
    return target;
}
#define PLATFORM_WITH_SETUP(result, name, parameters, arguments, setup) \
    result name parameters { \
        setup; \
        typedef result (*Function) parameters; \
        static _Atomic(Function) cached; \
        Function target = atomic_load_explicit(&cached, memory_order_acquire); \
        if (!target) { \
            target = (Function)bifrost_platform_symbol(#name); \
            atomic_store_explicit(&cached, target, memory_order_release); \
        } \
        return target arguments; \
    }
#define PLATFORM(result, name, parameters, arguments) \
    PLATFORM_WITH_SETUP(result, name, parameters, arguments, (void)0)

PLATFORM(ALooper *, ALooper_prepare, (int opts), (opts))
PLATFORM(ALooper *, ALooper_forThread, (void), ())
PLATFORM(void, ALooper_acquire, (ALooper *looper), (looper))
PLATFORM(void, ALooper_release, (ALooper *looper), (looper))
PLATFORM(int, ALooper_pollOnce, (int timeout, int *fd, int *events, void **data),
        (timeout, fd, events, data))
PLATFORM(int, ALooper_pollAll, (int timeout, int *fd, int *events, void **data),
        (timeout, fd, events, data))
PLATFORM(int, ALooper_addFd, (ALooper *looper, int fd, int ident, int events,
        ALooper_callbackFunc callback, void *data), (looper, fd, ident, events, callback, data))
PLATFORM(int, ALooper_removeFd, (ALooper *looper, int fd), (looper, fd))
PLATFORM(void, ALooper_wake, (ALooper *looper), (looper))

// Frame callbacks must use the same looper as ALooper_pollOnce, including
// native-only managed-app threads such as Unity's Swappy thread.
// Unlike SurfaceFlinger on a device, our Java display service is in-process.
// Native workers can therefore enter JavaBBinder directly during construction.
// Attach at this boundary and retain it until pthread exit, including callbacks.
PLATFORM_WITH_SETUP(AChoreographer *, AChoreographer_getInstance, (void), (), attach_java_thread())
PLATFORM(void, AChoreographer_postFrameCallback, (AChoreographer *choreographer,
        AChoreographer_frameCallback callback, void *data), (choreographer, callback, data))
PLATFORM(void, AChoreographer_postFrameCallbackDelayed, (AChoreographer *choreographer,
        AChoreographer_frameCallback callback, void *data, long delay),
        (choreographer, callback, data, delay))
PLATFORM(void, AChoreographer_postFrameCallback64, (AChoreographer *choreographer,
        AChoreographer_frameCallback64 callback, void *data), (choreographer, callback, data))
PLATFORM(void, AChoreographer_postFrameCallbackDelayed64, (AChoreographer *choreographer,
        AChoreographer_frameCallback64 callback, void *data, uint32_t delay),
        (choreographer, callback, data, delay))
#undef PLATFORM
#undef PLATFORM_WITH_SETUP
