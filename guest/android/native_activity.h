#ifndef BIFROST_GUEST_NATIVE_ACTIVITY_H
#define BIFROST_GUEST_NATIVE_ACTIVITY_H
#include "bionic.h"
// NDK native_activity.h LP64 ABI. JNI is deliberately absent until guest ART.
typedef struct ANativeActivity ANativeActivity;
typedef struct ANativeWindow ANativeWindow;
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AInputQueue AInputQueue;
typedef struct {
    int32_t left, top, right, bottom;
} ARect;
typedef struct {
    void (*onStart)(ANativeActivity *);
    void (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void (*onPause)(ANativeActivity *);
    void (*onStop)(ANativeActivity *);
    void (*onDestroy)(ANativeActivity *);
    void (*onWindowFocusChanged)(ANativeActivity *, int);
    void (*onNativeWindowCreated)(ANativeActivity *, ANativeWindow *);
    void (*onNativeWindowResized)(ANativeActivity *, ANativeWindow *);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *, ANativeWindow *);
    void (*onNativeWindowDestroyed)(ANativeActivity *, ANativeWindow *);
    void (*onInputQueueCreated)(ANativeActivity *, AInputQueue *);
    void (*onInputQueueDestroyed)(ANativeActivity *, AInputQueue *);
    void (*onContentRectChanged)(ANativeActivity *, const ARect *);
    void (*onConfigurationChanged)(ANativeActivity *);
    void (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;
struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm;
    void *env;
    void *clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    AAssetManager *assetManager;
    const char *obbPath;
};
extern ANativeWindow *ANativeWindow_fromSurface(void *, void *);
extern void ANativeWindow_release(ANativeWindow *);
extern int32_t ANativeWindow_getWidth(ANativeWindow *);
extern int32_t ANativeWindow_getHeight(ANativeWindow *);
extern AAssetManager *bifrost_asset_manager_create(const char *);
extern void bifrost_asset_manager_destroy(AAssetManager *);
extern AAsset *AAssetManager_open(AAssetManager *, const char *, int);
extern int AAsset_read(AAsset *, void *, size_t);
extern long AAsset_seek(AAsset *, long, int);
extern long AAsset_getLength(AAsset *);
extern const void *AAsset_getBuffer(AAsset *);
extern void AAsset_close(AAsset *);
#endif
