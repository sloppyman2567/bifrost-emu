#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

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
