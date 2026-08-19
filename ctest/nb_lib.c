// nb_lib.c — AArch64 test library for the native bridge adapter.
//
// Built with the musl cross toolchain as a position-independent shared
// object (ctest/nb_testlib.so). Every function is written with the JNI
// native-method shape: the first two args are the opaque JNIEnv* and
// jobject/class handles that ART always passes, followed by the real
// arguments described by the function's JNI shorty:
//
//   nb_add  shorty "JJJ"  long f(JNIEnv*, jobject, jlong, jlong)
//   nb_fadd shorty "DDD"  double f(JNIEnv*, jobject, jdouble, jdouble)
//   nb_gets shorty "JJ"   long f(JNIEnv*, jobject, jlong)
//   nb_mix  shorty "DID"  double f(JNIEnv*, jobject, jint, jdouble)
//   nb_fmul shorty "FFF"  float f(JNIEnv*, jobject, jfloat, jfloat)
//
// The trampoline built by the adapter forwards the host call (env, thiz,
// args...) through bifrost_call/bifrost_call_f into x0..x7 / d0..d7.
typedef void* nb_jnienv;
typedef void* nb_jobject;

long nb_add(nb_jnienv env, nb_jobject thiz, long a, long b) {
    (void)env;
    (void)thiz;
    return a + b;
}

double nb_fadd(nb_jnienv env, nb_jobject thiz, double a, double b) {
    (void)env;
    (void)thiz;
    return a + b;
}

long nb_gets(nb_jnienv env, nb_jobject thiz, long a) {
    (void)env;
    (void)thiz;
    return a & 0xff;
}

double nb_mix(nb_jnienv env, nb_jobject thiz, int a, double b) {
    (void)env;
    (void)thiz;
    return (double)a + b;
}

float nb_fmul(nb_jnienv env, nb_jobject thiz, float a, float b) {
    (void)env;
    (void)thiz;
    return a * b;
}