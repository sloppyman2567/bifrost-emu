// Preloaded into every managed app process.
//
// The launcher publishes Java implementations of native services (media.metrics, SurfaceFlinger, sensorservice, audio)
// inside the app process. On a device those live in other processes and run on attached Binder threads; here libbinder
// finds the object local and calls it on the caller's own thread. JavaBBinder::onTransact aborts ("env null. Attach
// JVM?") when that is a native thread the JVM does not know. So BBinder::transact first attaches such a thread: as a
// daemon, detached when the pthread exits, and never touching an attachment the app or ART owns (JNI tips guidance).
//
// Bionic's breadth-first lookup reaches libbinder before the libandroid adapter, hence a preload of its own.
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

typedef int32_t (*transact_function)(void *, uint32_t, const void *, void *, uint32_t);
#define TRANSACT "_ZN7android7BBinder8transactEjRKNS_6ParcelEPS1_j"

static _Atomic(transact_function) next_transact;
static _Atomic(JavaVM *) java_vm;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t attachment_key;
static int key_ready;

static void detach_thread(void *value) {
    JavaVM *vm = value;
    (*vm)->DetachCurrentThread(vm);
}
static void create_key(void) {
    key_ready = pthread_key_create(&attachment_key, detach_thread) == 0;
}
// Apps do not get an LD_PRELOAD in their environment on a device, and some of them look.
__attribute__((constructor)) static void hide_preload(void) {
    unsetenv("LD_PRELOAD");
}
static JavaVM *running_vm(void) {
    JavaVM *vm = atomic_load_explicit(&java_vm, memory_order_acquire);
    if (vm) return vm;
    jint (*created)(JavaVM **, jsize, jsize *) = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    jsize count = 0;
    if (!created || created(&vm, 1, &count) != JNI_OK || count != 1) return NULL;
    atomic_store_explicit(&java_vm, vm, memory_order_release);
    return vm;
}
static void attach_thread(void) {
    JavaVM *vm = running_vm();
    if (!vm) return;  // The VM is not up yet, so there are no Java services to reach.
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_EDETACHED) return;
    pthread_once(&key_once, create_key);
    // Without a detach at exit ART aborts when the thread ends, so do not attach if that cannot be arranged.
    if (!key_ready || (*vm)->AttachCurrentThreadAsDaemon(vm, &env, NULL) != JNI_OK) return;
    if (pthread_setspecific(attachment_key, vm)) (*vm)->DetachCurrentThread(vm);
}

int32_t _ZN7android7BBinder8transactEjRKNS_6ParcelEPS1_j(void *self, uint32_t code, const void *data, void *reply,
                                                         uint32_t flags) {
    transact_function next = atomic_load_explicit(&next_transact, memory_order_acquire);
    if (!next) {
        next = (transact_function)dlsym(RTLD_NEXT, TRANSACT);
        if (!next) abort();
        atomic_store_explicit(&next_transact, next, memory_order_release);
    }
    attach_thread();
    return next(self, code, data, reply, flags);
}
