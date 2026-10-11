#include "native_activity.h"
_Static_assert(sizeof(ANativeActivity) == 80, "NDK NativeActivity LP64 layout");
_Static_assert(sizeof(ANativeActivityCallbacks) == 128, "NDK callback layout");
static int decimal(const char *text, int limit) {
    if (!*text)
        return -1;
    int value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > (limit - (*text - '0')) / 10)
            return -1;
        value = value * 10 + (*text - '0');
    }
    return value;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    int probe = argc >= 3 && !strcmp(argv[1], "--probe");
    // library, entrypoint, internal files, external files, assets, SDK, frames
    if (!probe && argc != 8) {
        printf("native launcher: invalid package launch arguments\n");
        return 2;
    }
    int sdk = probe ? 35 : decimal(argv[6], 35);
    int frames = probe ? 0 : decimal(argv[7], 36000);
    if (sdk != 35 || frames < 0)
        return 2;
    const char *path = argv[probe ? 2 : 1];
    size_t length = strlen(path), slash = length;
    while (slash && path[slash - 1] != '/')
        --slash;
    if (path[0] != '/' || slash <= 1 || length >= 4096)
        return 2;
    char directory[4096];
    memcpy(directory, path, slash - 1);
    directory[slash - 1] = 0;
    // An absolute dlopen path does not make sibling DT_NEEDED libraries
    // searchable. Configure the real guest linker before loading any app DSO.
    android_update_LD_LIBRARY_PATH(directory);
    // Probe prerequisites are explicit, ordered RTLD_LOCAL loads. Keep their
    // handles alive through process exit, just like the target library.
    if (probe) {
        for (int i = 3; i < argc; ++i) {
            if (!dlopen(argv[i], RTLD_NOW)) {
                printf("native probe: PRELOAD_FAIL %s: %s\n", argv[i], dlerror());
                fflush(stdout);
                return 126;
            }
            printf("native probe: PRELOAD_OK %s\n", argv[i]);
            if (fflush(stdout))
                return 126;
        }
    }
    void *library = dlopen(path, RTLD_NOW); // Resolve every dependency.
    if (!library) {
        printf("%s: %s\n", probe ? "native probe: LOAD_FAIL" : "native launcher", dlerror());
        fflush(stdout);
        return 126;
    }
    if (probe) {
        // dlopen runs constructors, but JNI_OnLoad and Activity entrypoints
        // are deliberately not called without a real JavaVM/framework.
        printf("native probe: LOAD_OK %s JNI_OnLoad=%p\n", path, dlsym(library, "JNI_OnLoad"));
        // Exit handlers can abort before stdio cleanup. Preserve the completed
        // loading phase without treating it as a successful process shutdown.
        if (fflush(stdout))
            return 126;
        return 0;
    }
    void (*create)(ANativeActivity *, void *, size_t) = dlsym(library, argv[2]);
    if (!create) {
        printf("native launcher: missing entrypoint %s\n", argv[2]);
        return 126;
    }
    ANativeActivityCallbacks callbacks = {0};
    ANativeActivity activity = {0};
    activity.callbacks = &callbacks;
    activity.internalDataPath = argv[3];
    activity.externalDataPath = argv[4];
    activity.assetManager = bifrost_asset_manager_create(argv[5]);
    activity.obbPath = "";
    activity.sdkVersion = sdk;
    if (!activity.assetManager)
        return 2;
    // No fake JavaVM/JNIEnv: only native Activities that do not require JNI
    // are supported by this launcher. Managed Activities use the ART launcher.
    create(&activity, 0, 0);
    if (callbacks.onStart)
        callbacks.onStart(&activity);
    if (callbacks.onResume)
        callbacks.onResume(&activity);
    // Private launcher ABI: no Java Surface exists in the native-only path.
    // Keep ANativeWindow_fromSurface's public null-object rejection intact.
    ANativeWindow *window = (ANativeWindow *)(uintptr_t)syscall(0x100e, 7L);
    if (!window) {
        printf("native launcher: Android window backend unavailable\n");
        if (callbacks.onPause)
            callbacks.onPause(&activity);
        if (callbacks.onStop)
            callbacks.onStop(&activity);
        if (callbacks.onDestroy)
            callbacks.onDestroy(&activity);
        bifrost_asset_manager_destroy(activity.assetManager);
        return 126;
    }
    if (callbacks.onNativeWindowCreated)
        callbacks.onNativeWindowCreated(&activity, window);
    AInputQueue *queue = (AInputQueue *)(uintptr_t)syscall(0x100e, 1L);
    if (callbacks.onInputQueueCreated)
        callbacks.onInputQueueCreated(&activity, queue);
    ARect rect = {0, 0, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window)};
    if (callbacks.onContentRectChanged)
        callbacks.onContentRectChanged(&activity, &rect);
    if (callbacks.onWindowFocusChanged)
        callbacks.onWindowFocusChanged(&activity, 1);
    for (int frame = 0; !frames || frame < frames;) {
        if (syscall(0x100e, 0L))
            break;
        if (callbacks.onNativeWindowRedrawNeeded)
            callbacks.onNativeWindowRedrawNeeded(&activity, window);
        usleep(16667);
        if (frames)
            ++frame;
    }
    if (callbacks.onWindowFocusChanged)
        callbacks.onWindowFocusChanged(&activity, 0);
    if (callbacks.onPause)
        callbacks.onPause(&activity);
    if (callbacks.onInputQueueDestroyed)
        callbacks.onInputQueueDestroyed(&activity, queue);
    if (callbacks.onNativeWindowDestroyed)
        callbacks.onNativeWindowDestroyed(&activity, window);
    if (callbacks.onStop)
        callbacks.onStop(&activity);
    if (callbacks.onDestroy)
        callbacks.onDestroy(&activity);
    ANativeWindow_release(window);
    bifrost_asset_manager_destroy(activity.assetManager);
    // Do not unload: app threads may still be executing this DSO. Bionic exit
    // terminates the whole process after the lifecycle callbacks return.
    printf("native launcher: lifecycle complete\n");
    return 0;
}
