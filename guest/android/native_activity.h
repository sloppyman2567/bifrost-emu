#ifndef BIFROST_GUEST_NATIVE_ACTIVITY_H
#define BIFROST_GUEST_NATIVE_ACTIVITY_H
#include "bionic.h"
#include <android/native_activity.h>
#include <android/native_window_jni.h>
// Bifrost's own guest-side asset storage, distinct from platform ABI declarations.
extern AAssetManager *bifrost_asset_manager_create(const char *);
extern void bifrost_asset_manager_destroy(AAssetManager *);
#endif
