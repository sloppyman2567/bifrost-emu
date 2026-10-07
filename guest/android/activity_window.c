#include <jni.h>
#include <stdint.h>

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
