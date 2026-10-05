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
    // library, entrypoint, internal files, external files, assets, SDK, frames
    if (argc != 8) {
        printf("native launcher: invalid package launch arguments\n");
        return 2;
    }
    int sdk = decimal(argv[6], 35), frames = decimal(argv[7], 36000);
    if (sdk != 35 || frames < 0)
        return 2;
    void *library = dlopen(argv[1], 2);
    if (!library) {
        printf("native launcher: %s\n", dlerror());
        return 126;
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
    // are supported by this launcher. Managed Activities are rejected by host.
    create(&activity, 0, 0);
    if (callbacks.onStart)
        callbacks.onStart(&activity);
    if (callbacks.onResume)
        callbacks.onResume(&activity);
    ANativeWindow *window = ANativeWindow_fromSurface(0, 0);
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
