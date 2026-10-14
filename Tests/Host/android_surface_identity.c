#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Link the actual guest surface adapter; JNI handles are controlled test doubles.
void *bifrost_surface_prepare(JNIEnv *, jobject);
void *ANativeWindow_fromSurface(JNIEnv *, jobject);
struct reference { int key, collected; };
static struct reference *first_weak;
static int deleted, fail_weak;
static int key(jobject object) {
    if (!object) return 0;
    if ((uintptr_t)object < 4096) return (uintptr_t)object < 300 ? 1 : (int)((uintptr_t)object / 100);
    struct reference *ref = (struct reference *)object;
    return ref->collected ? 0 : ref->key;
}
static jboolean same(JNIEnv *env, jobject a, jobject b) {
    (void)env;
    return key(a) == key(b);
}
static jweak weak(JNIEnv *env, jobject object) {
    (void)env;
    if (fail_weak) return NULL;
    struct reference *ref = calloc(1, sizeof(*ref));
    if (ref) ref->key = key(object);
    if (!first_weak) first_weak = ref;
    return (jweak)ref;
}
static void delete_weak(JNIEnv *env, jweak object) { (void)env; ++deleted; free(object); }
static void *native_surface(JNIEnv *env, jobject object) { (void)env; (void)object; return NULL; }
void *bifrost_platform_symbol(const char *name) {
    return !strcmp(name, "ANativeWindow_fromSurface") ? (void *)native_surface : NULL;
}
void *thunk_ANativeWindow_fromSurface(JNIEnv *env, void *identity) { (void)env; return identity; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "JNI identity failed at %d\n", __LINE__); return 1; } } while (0)
int main(void) {
    struct JNINativeInterface functions;
    memset(&functions, 0, sizeof(functions));
    functions.IsSameObject = same;
    functions.NewWeakGlobalRef = weak;
    functions.DeleteWeakGlobalRef = delete_weak;
    JNIEnv env = &functions;
    CHECK(!ANativeWindow_fromSurface(&env, NULL));
    CHECK(!ANativeWindow_fromSurface(&env, (jobject)100));
    CHECK(!bifrost_surface_prepare(NULL, (jobject)100));
    CHECK(!bifrost_surface_prepare(&env, NULL));
    void *first = bifrost_surface_prepare(&env, (jobject)100);
    CHECK(first && bifrost_surface_prepare(&env, (jobject)200) == first);
    void *second = bifrost_surface_prepare(&env, (jobject)300);
    CHECK(second && second != first);
    first_weak->collected = 1;
    void *replacement = bifrost_surface_prepare(&env, (jobject)100);
    CHECK(replacement && replacement != first && replacement != second);
    CHECK(deleted == 1);
    fail_weak = 1;
    CHECK(!bifrost_surface_prepare(&env, (jobject)500));
    puts("android_surface_identity: checks passed");
    return 0;
}
