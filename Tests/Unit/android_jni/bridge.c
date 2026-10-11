#include <jni.h>

static jint round_trip(JNIEnv *env, jclass type, jintArray array, jstring text) {
    if ((*env)->GetArrayLength(env, array) != 3 || (*env)->GetStringLength(env, text) != 5)
        return -1;
    jint values[3];
    (*env)->GetIntArrayRegion(env, array, 0, 3, values);
    if ((*env)->ExceptionCheck(env)) return -1;
    jmethodID callback = (*env)->GetStaticMethodID(env, type, "callback", "(I)I");
    if (!callback) return -1;
    jint result = (*env)->CallStaticIntMethod(env, type, callback, values[0] + values[1] + values[2]);
    if ((*env)->ExceptionCheck(env)) return -1;
    jint replacement = 42;
    (*env)->SetIntArrayRegion(env, array, 1, 1, &replacement);
    return (*env)->ExceptionCheck(env) ? -1 : result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass type = (*env)->FindClass(env, "Bootstrap");
    if (!type) return JNI_ERR;
    JNINativeMethod method = {"roundTrip", "([ILjava/lang/String;)I", (void *)round_trip};
    jint status = (*env)->RegisterNatives(env, type, &method, 1);
    (*env)->DeleteLocalRef(env, type);
    return status == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
