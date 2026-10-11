// Experimental guest-owned ART bootstrap. No host JVM or fake JNI tables.
#include "bionic.h"
#include <jni.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (setenv("ANDROID_ROOT", "/system", 1) ||
        setenv("ANDROID_ART_ROOT", "/apex/com.android.art", 1) ||
        setenv("ANDROID_I18N_ROOT", "/apex/com.android.i18n", 1) ||
        setenv("ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata", 1) ||
        setenv("ANDROID_DATA", "/data", 1)) return 2;
    void *library = dlopen("libart.so", RTLD_NOW | RTLD_GLOBAL);
    if (!library) {
        fprintf(stderr, "ART_LOAD_FAIL: %s\n", dlerror());
        return 126;
    }
    jint (*create_vm)(JavaVM **, void **, void *) = dlsym(library, "JNI_CreateJavaVM");
    if (!create_vm) return 126;
    printf("ART_LOAD_OK\n");
    fflush(stdout);
    JavaVMOption options[] = {
        {"-Xuse-stderr-logger", 0}, {"-Xint", 0}, {"-Xusejit:false", 0}, {"-Xcheck:jni", 0},
        {"-Xms16m", 0}, {"-Xmx64m", 0},
        {"-Xnoimage-dex2oat", 0}, {"-Ximage:/nonexistent/boot.art", 0},
        {"-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
         "/apex/com.android.art/javalib/core-libart.jar:"
         "/apex/com.android.art/javalib/okhttp.jar:"
         "/apex/com.android.art/javalib/bouncycastle.jar:"
         "/apex/com.android.art/javalib/apache-xml.jar:"
         "/apex/com.android.i18n/javalib/core-icu4j.jar:"
         "/apex/com.android.conscrypt/javalib/conscrypt.jar", 0},
        {"-Djava.class.path=/data/local/tmp/classes.dex", 0},
        {"-Djava.library.path=/data/local/tmp", 0},
    };
    JavaVMInitArgs args = {JNI_VERSION_1_6, sizeof(options) / sizeof(options[0]), options, JNI_FALSE};
    JavaVM *vm = 0;
    JNIEnv *env = 0;
    if (create_vm(&vm, (void **)&env, &args) != JNI_OK) {
        fprintf(stderr, "ART_VM_FAIL\n");
        return 126;
    }
    printf("ART_VM_OK\n");
    fflush(stdout);
    jclass entry = (*env)->FindClass(env, argv[1]);
    jmethodID main_method = entry ? (*env)->GetStaticMethodID(env, entry, "main", "([Ljava/lang/String;)V") : 0;
    jclass string_class = main_method ? (*env)->FindClass(env, "java/lang/String") : 0;
    jobjectArray arguments = string_class ? (*env)->NewObjectArray(env, 0, string_class, 0) : 0;
    if (arguments) (*env)->CallStaticVoidMethod(env, entry, main_method, arguments);
    int failed = !arguments || (*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionDescribe(env);
    if (arguments) (*env)->DeleteLocalRef(env, arguments);
    if (string_class) (*env)->DeleteLocalRef(env, string_class);
    if (entry) (*env)->DeleteLocalRef(env, entry);
    // ART owns shutdown and attached-thread cleanup. Its DSO remains loaded.
    if ((*vm)->DestroyJavaVM(vm) != JNI_OK) failed = 1;
    return failed ? 1 : 0;
}
