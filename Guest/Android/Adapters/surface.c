#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct surface_identity { jweak object; uint64_t id; struct surface_identity *next; };
static struct surface_identity *surfaces;
static uint64_t next_id;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *thunk_ANativeWindow_fromSurface(JNIEnv *, void *);
void *bifrost_platform_symbol(const char *);
static long window_call(long operation, long a, long b) {
    register long x0 __asm__("x0") = operation, x1 __asm__("x1") = a, x2 __asm__("x2") = b;
    register long x8 __asm__("x8") = 0x100e;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}
static int shim(void *window) {
    uintptr_t value = (uintptr_t)window;
    return value >= 0xa90000000000ULL && value <= 0xa9000000ffffULL;
}
static void *platform_surface(JNIEnv *env, jobject object) {
    void *(*get)(JNIEnv *, jobject) = bifrost_platform_symbol("ANativeWindow_fromSurface");
    return get(env, object);
}
void *bifrost_surface_prepare(JNIEnv *env, jobject object);

void *bifrost_surface_prepare(JNIEnv *env, jobject object) {
    if (!env || !object) return NULL;
    pthread_mutex_lock(&lock);
    struct surface_identity **link = &surfaces;
    while (*link) {
        struct surface_identity *entry = *link;
        if ((*env)->IsSameObject(env, entry->object, NULL)) {
            *link = entry->next;
            (*env)->DeleteWeakGlobalRef(env, entry->object);
            free(entry);
            continue;
        }
        if ((*env)->IsSameObject(env, entry->object, object)) {
            uint64_t id = entry->id;
            pthread_mutex_unlock(&lock);
            void *host = thunk_ANativeWindow_fromSurface(env, (void *)(uintptr_t)id);
            void *native = platform_surface(env, object);
            if (native && host && window_call(15, (long)native, (long)host)) {
                void (*release)(void *) = bifrost_platform_symbol("ANativeWindow_release");
                release(native); return NULL;
            }
            return native ? native : host;
        }
        link = &entry->next;
    }
    struct surface_identity *entry = calloc(1, sizeof(*entry));
    if (!entry) { pthread_mutex_unlock(&lock); return NULL; }
    entry->object = (*env)->NewWeakGlobalRef(env, object);
    if (!entry->object) { free(entry); pthread_mutex_unlock(&lock); return NULL; }
    entry->id = ++next_id;
    entry->next = surfaces;
    surfaces = entry;
    uint64_t id = entry->id;
    pthread_mutex_unlock(&lock);
    (void)id;
    return bifrost_surface_prepare(env, object);
}

// A real Android Surface always yields the image's genuine ANativeWindow.
// Only managed window preparation attaches it to a host EGL/pixel backing.
void *ANativeWindow_fromSurface(JNIEnv *env, jobject object) {
    if (!env || !object) return NULL;
    void *native = platform_surface(env, object);
    if (native) {
        long host = window_call(17, (long)native, 0);
        if (host > 0) window_call(17, (long)native, 1);
        return native;
    }
    return NULL;
}
void ANativeWindow_acquire(void *window) {
    if (shim(window)) { extern void thunk_ANativeWindow_acquire(void *); thunk_ANativeWindow_acquire(window); return; }
    void (*call)(void *) = bifrost_platform_symbol("ANativeWindow_acquire");
    call(window); window_call(17, (long)window, 1);
}
void ANativeWindow_release(void *window) {
    if (shim(window)) { extern void thunk_ANativeWindow_release(void *); thunk_ANativeWindow_release(window); return; }
    window_call(17, (long)window, 2);
    void (*call)(void *) = bifrost_platform_symbol("ANativeWindow_release");
    call(window);
}
void ANativeWindow_free(void *window) { ANativeWindow_release(window); }
#define WINDOW_INT(name) \
int name(void *window) { \
    extern int thunk_##name(void *); \
    if (shim(window)) return thunk_##name(window); \
    int (*call)(void *) = bifrost_platform_symbol(#name); return call(window); \
}
WINDOW_INT(ANativeWindow_getWidth)
WINDOW_INT(ANativeWindow_getHeight)
WINDOW_INT(ANativeWindow_getFormat)
int ANativeWindow_setBuffersGeometry(void *window, int w, int h, int format) {
    extern int thunk_ANativeWindow_setBuffersGeometry(void *, int, int, int);
    if (shim(window)) return thunk_ANativeWindow_setBuffersGeometry(window,w,h,format);
    int (*call)(void *,int,int,int) = bifrost_platform_symbol("ANativeWindow_setBuffersGeometry");
    return call(window,w,h,format);
}
int ANativeWindow_setFrameRate(void *window, float rate, int compatibility) {
    extern int thunk_ANativeWindow_setFrameRate(void *,float,int);
    if (shim(window)) return thunk_ANativeWindow_setFrameRate(window,rate,compatibility);
    int (*call)(void *,float,int) = bifrost_platform_symbol("ANativeWindow_setFrameRate");
    return call(window,rate,compatibility);
}
int ANativeWindow_setFrameRateWithChangeStrategy(void *window, float rate, int compatibility, int strategy) {
    extern int thunk_ANativeWindow_setFrameRateWithChangeStrategy(void *,float,int,int);
    if (shim(window)) return thunk_ANativeWindow_setFrameRateWithChangeStrategy(window,rate,compatibility,strategy);
    int (*call)(void *,float,int,int) = bifrost_platform_symbol("ANativeWindow_setFrameRateWithChangeStrategy");
    return call(window,rate,compatibility,strategy);
}
