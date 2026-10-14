// Preloaded only by the realm's host Codec2 daemon. The real API-35 AIDL
// wrapper, component interfaces, buffers and callbacks remain in Android.
// Signatures: frameworks/native/libs/binder/ndk/include_platform/android/
// binder_manager.h at android-15.0.0_r1; binder_status_t is int32_t.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

bool AServiceManager_isDeclared(const char *name) {
    // The stock software daemon tries to configure its allocator from the
    // declared default store. This daemon IS that store; do not wait for self.
    if (!strcmp(name, "android.hardware.media.c2.IComponentStore/default")) return false;
    bool (*next)(const char *) = dlsym(RTLD_NEXT, "AServiceManager_isDeclared");
    return next && next(name);
}

int32_t AServiceManager_addService(void *binder, const char *name) {
    int32_t (*next)(void *, const char *) = dlsym(RTLD_NEXT, "AServiceManager_addService");
    if (!next) return -38; // STATUS_INVALID_OPERATION / ENOSYS: no fake registration
    if (!strcmp(name, "android.hardware.media.c2.IComponentStore/software"))
        name = "android.hardware.media.c2.IComponentStore/default";
    return next(binder, name);
}
