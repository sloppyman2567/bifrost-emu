// Asgard's Zygote runtime: app_process for a VM started with -Xzygote that must not open Binder before it forks.
//
// app_process's RuntimeInit mode starts Binder's thread pool before main (AppRuntime::onStarted in
// frameworks/base/cmds/app_process/app_main.cpp), and libbinder aborts a fork child that uses a ProcessState made
// before the fork (ProcessState::childPostFork), so app_process cannot host a Zygote that forks managed children.
// This is the image's own AndroidRuntime (android-15.0.0_r1 AndroidRuntime.h) started as app_process starts it,
// with app_process's two virtuals filled in: onStarted calls CLASS.main without Binder, and onZygoteInit, which a
// child reaches through ZygoteInit.nativeZygoteInit once it has specialized, starts the Binder thread pool as
// AppRuntime::onZygoteInit does. CLASS also gets the native zygoteFork (below).
//
//   bifrost-zygote [VM options...] CLASS [ARGS...]
#include <errno.h>
#include <jni.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include "bionic.h"

extern void runtime_construct(void* self, char* arguments, size_t size) __asm__("_ZN7android14AndroidRuntimeC2EPcm");
extern void runtime_add_option(void* self, const char* option, void* extra)
    __asm__("_ZN7android14AndroidRuntime9addOptionEPKcPv");
extern void runtime_set_argv0(void* self, const char* name, bool set_thread_name)
    __asm__("_ZN7android14AndroidRuntime8setArgv0EPKcb");
extern void runtime_start(void* self, const char* class_name, const void* options, bool zygote)
    __asm__("_ZN7android14AndroidRuntime5startEPKcRKNS_6VectorINS_7String8EEEb");
extern JNIEnv* runtime_jni_env(void) __asm__("_ZN7android14AndroidRuntime9getJNIEnvEv");
extern void ABinderProcess_startThreadPool(void);

// Itanium C++ ABI vtable: offset to top, type info, then AndroidRuntime's virtuals in declaration order.
enum { DESTRUCTOR, DELETING_DESTRUCTOR, ON_VM_CREATED, ON_STARTED, ON_ZYGOTE_INIT, ON_EXIT, VIRTUALS };
static void* vtable[2 + VIRTUALS];
// AndroidRuntime is 72 bytes (vptr, Vector<JavaVMOption>, a flag and the argument block); the rest is slack.
static struct { void** vptr; unsigned char members[248]; } runtime;
// An empty Vector<String8> (vptr, storage, count, flags, item size): RuntimeInit.main gets no options.
static const uint64_t no_options[6];
static const char* entry_class;
static char** entry_arguments;
static int entry_count;

// fork(); in the child, SpecializeCommon's order (group, then user, both `uid`), then its name, death with this
// Zygote and stdout/stderr on `log` (kept when negative). Returns the child's pid, 0 in the child, or -errno.
static jint zygote_fork(JNIEnv* env, jclass type, jint uid, jint log, jstring name) {
    (void)type;
    const char* chars = (*env)->GetStringUTFChars(env, name, NULL);
    if (!chars) return -ENOMEM;
    char nice_name[64];
    snprintf(nice_name, sizeof nice_name, "%s", chars);
    (*env)->ReleaseStringUTFChars(env, name, chars);
    // ForkCommon blocks SIGCHLD across the fork so no handler sees a half-made child.
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &previous);
    const pid_t zygote = getpid();
    const pid_t pid = fork();
    if (pid != 0) {
        const int error = errno;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        return pid < 0 ? -error : pid;
    }
    if (log >= 0 && (dup2(log, 1) < 0 || dup2(log, 2) < 0)) _exit(127);  // the caller closes `log`
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != zygote) _exit(128 + SIGKILL);
    if (setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) {
        fprintf(stderr, "bifrost-zygote: cannot become uid %d: %s\n", uid, strerror(errno));
        _exit(126);
    }
    runtime_set_argv0(&runtime, nice_name, true);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return 0;
}

// AppRuntime::onStarted without ProcessState: CLASS.main(ARGS), loaded by the system class loader (java.class.path).
static void on_started(void* self) {
    (void)self;
    JNIEnv* env = runtime_jni_env();
    jclass loaders = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID system_loader = (*env)->GetStaticMethodID(env, loaders, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID load = (*env)->GetMethodID(env, loaders, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jobject loader = (*env)->CallStaticObjectMethod(env, loaders, system_loader);
    jstring name = loader ? (*env)->NewStringUTF(env, entry_class) : NULL;
    jclass entry = name ? (jclass)(*env)->CallObjectMethod(env, loader, load, name) : NULL;
    if (!entry) return;  // the pending exception ends RuntimeInit.main, as app_process's callMain leaves it
    const JNINativeMethod fork_method = {"zygoteFork", "(IILjava/lang/String;)I", (void*)zygote_fork};
    if ((*env)->RegisterNatives(env, entry, &fork_method, 1)) return;
    jmethodID main_method = (*env)->GetStaticMethodID(env, entry, "main", "([Ljava/lang/String;)V");
    jclass strings = main_method ? (*env)->FindClass(env, "java/lang/String") : NULL;
    jobjectArray arguments = strings ? (*env)->NewObjectArray(env, entry_count, strings, NULL) : NULL;
    if (!arguments) return;
    for (int i = 0; i < entry_count; i++) {
        jstring argument = (*env)->NewStringUTF(env, entry_arguments[i]);
        if (!argument) return;
        (*env)->SetObjectArrayElement(env, arguments, i, argument);
        (*env)->DeleteLocalRef(env, argument);
    }
    (*env)->CallStaticVoidMethod(env, entry, main_method, arguments);
}

// AppRuntime::onZygoteInit: a specialized child's Binder thread pool.
static void on_zygote_init(void* self) {
    (void)self;
    ABinderProcess_startThreadPool();
}

int main(int argc, char** argv) {
    int first = 1;
    while (first < argc && argv[first][0] == '-') first++;
    if (first >= argc) {
        fprintf(stderr, "usage: bifrost-zygote [VM options...] CLASS [ARGS...]\n");
        return 2;
    }
    void** image = dlsym(RTLD_DEFAULT, "_ZTVN7android14AndroidRuntimeE");
    void* pure_virtual = dlsym(RTLD_DEFAULT, "__cxa_pure_virtual");
    if (!image || !pure_virtual || image[2 + ON_STARTED] != pure_virtual) {
        fprintf(stderr, "bifrost-zygote: the image's AndroidRuntime has an unexpected layout\n");
        return 1;
    }
    memcpy(vtable, image, sizeof vtable);
    vtable[2 + ON_STARTED] = (void*)on_started;
    vtable[2 + ON_ZYGOTE_INIT] = (void*)on_zygote_init;
    // setArgv0 overwrites the argument block, so keep copies, as app_process does.
    entry_class = strdup(argv[first]);
    entry_count = argc - first - 1;
    entry_arguments = calloc(entry_count + 1, sizeof *entry_arguments);
    if (!entry_class || !entry_arguments) return 1;
    for (int i = 0; i < entry_count; i++)
        if (!(entry_arguments[i] = strdup(argv[first + 1 + i]))) return 1;
    char* end = argv[argc - 1] + strlen(argv[argc - 1]);
    runtime_construct(&runtime, argv[0], (size_t)(end - argv[0]));
    runtime.vptr = &vtable[2];
    runtime_add_option(&runtime, "-Xzygote", NULL);
    for (int i = 1; i < first; i++) {
        char* option = strdup(argv[i]);
        if (!option) return 1;
        runtime_add_option(&runtime, option, NULL);
    }
    runtime_set_argv0(&runtime, "zygote64", true);
    runtime_start(&runtime, "com.android.internal.os.RuntimeInit", no_options, true);
    return 0;
}
