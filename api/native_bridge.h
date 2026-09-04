// native_bridge.h — Android libnativebridge ABI for libbifrost.
//
// This header makes libbifrost usable as an Android "native bridge"
// (-XX:NativeBridge), the mechanism ART uses to load ARM apps on an
// x86_64 Android device. ART dlopens the bridge library, dlsyms the
// symbol `NativeBridgeItf`, and calls the callbacks in the struct below
// to load AArch64 shared objects and obtain trampolines that forward
// JNI native-method calls into the emulated guest.
//
// ABI contract: the struct layout here MUST match AOSP's
// `NativeBridgeCallbacks` in platform/art
// (runtime/native_bridge/native_bridge.h) field-for-field, because ART
// reads the fields by offset. The function-pointer parameter types are
// only meaningful to ART (which calls them); we mirror the layout with
// void* where a JNI type (JNIEnv*, jobject, ...) would otherwise drag in
// jni.h.
//
// Licensing: this is a clean-room ABI mirror (the interface shape, not
// AOSP implementation code). AOSP's libnativebridge is Apache-2.0;
// libbifrost is Unlicense. Describing the same wire ABI for
// interoperability is not a copy of AOSP's implementation.
//
// Version: 1.5.5-alpha.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Opaque emulator handle (from bifrost.h). Forward-declared so this header
// stays usable without pulling in the whole C API.
typedef struct bifrost_emu bifrost_emu_t;

// Type of JNI call, as defined by ART (kJNICallTypeRegular /
// kJNICallTypeCriticalNative). Regular is the normal marshalled call;
// CriticalNative skips the JNIEnv*/jobject preamble. 1.5.5-alpha supports
// Regular only.
typedef enum {
    kJNICallTypeRegular       = 1,
    kJNICallTypeCriticalNative = 2,
} JNICallType;

// A signal handler the runtime would manage for the bridge. We mirror the
// shape (siginfo_t* in real AOSP; void* here keeps this header free of
// <signal.h>) because only the pointer ABI matters.
typedef bool (*NativeBridgeSignalHandlerFn)(int, void*, void*);

// Opaque namespace handle (Android linker namespace). Unused in the
// thin adapter (no namespace support).
struct native_bridge_namespace_t;
typedef struct native_bridge_namespace_t native_bridge_namespace_t;

// Runtime → bridge callbacks (ART provides these to initialize()).
// Mirrors `NativeBridgeRuntimeCallbacks`. We keep it opaque — the thin
// adapter does not call back into the runtime.
struct NativeBridgeRuntimeCallbacks;
struct NativeBridgeRuntimeValues;

// The bridge → runtime interface table. Field order/types MUST match
// AOSP's NativeBridgeCallbacks (offsets are read directly by ART).
typedef struct NativeBridgeCallbacks {
    // Version number of the interface.
    uint32_t version;

    // Initialize the native bridge. Must be MT-safe and idempotent.
    //   runtime_cbs    [IN] pointer to NativeBridgeRuntimeCallbacks
    //   private_dir    [IN] app data dir
    //   instruction_set[IN] instruction set of the app ("arm64"/...)
    // Returns true on success.
    bool (*initialize)(const struct NativeBridgeRuntimeCallbacks* runtime_cbs,
                       const char* private_dir, const char* instruction_set);

    // Load a shared library supported by the bridge. Returns an opaque
    // handle, or NULL on failure.
    void* (*loadLibrary)(const char* libpath, int flag);

    // Get a trampoline for a native method: a host function pointer with
    // the SAME signature as the native method
    // (ret f(JNIEnv*, jobject, ...shorty args...)). The trampoline
    // marshals the call into the emulated guest.
    //   handle [IN] handle from loadLibrary
    //   name   [IN] method name (JNI native name; symbol in the library)
    //   shorty [IN] short descriptor (return type + arg types)
    //   len    [IN] length of shorty
    // Returns the trampoline address, or NULL.
    void* (*getTrampoline)(void* handle, const char* name,
                           const char* shorty, uint32_t len);

    // Check whether a native library is valid and for a supported ABI.
    bool (*isSupported)(const char* libpath);

    // Environment values required by an app running with the bridge.
    // Returns NULL if not supported (no extra env).
    const struct NativeBridgeRuntimeValues* (*getAppEnv)(const char* instruction_set);

    // ── Added in version 2 ────────────────────────────────────────────
    // Whether the bridge is compatible with the given libnativebridge
    // version. If false, libnativebridge stops using the bridge.
    bool (*isCompatibleWith)(uint32_t bridge_version);

    // A signal handler the runtime should manage on the bridge's behalf.
    // The bridge must not install it itself. Return NULL if the bridge
    // doesn't use one (we do — the emulator manages its own host
    // signal forwarding).
    NativeBridgeSignalHandlerFn (*getSignalHandler)(int signal);

    // ── Added in version 3 ────────────────────────────────────────────
    // Decrement the reference count; unload when it reaches 0.
    // Returns 0 on success, nonzero on error.
    int (*unloadLibrary)(void* handle);

    // Last failure message (mirror of dlerror()). NULL if none.
    const char* (*getError)();

    // Check whether library search paths are supported (namespace
    // scenario). Here: whether the directory exists.
    bool (*isPathSupported)(const char* library_path);

    // No longer used.
    bool (*unused_initAnonymousNamespace)(const char*, const char*);

    // Create a linker namespace. NULL = unsupported (thin adapter).
    native_bridge_namespace_t* (*createNamespace)(
        const char* name, const char* ld_library_path,
        const char* default_library_path, uint64_t type,
        const char* permitted_when_isolated_path,
        native_bridge_namespace_t* parent_ns);

    // Link two namespaces sharing some libraries. False = unsupported.
    bool (*linkNamespaces)(native_bridge_namespace_t* from,
                           native_bridge_namespace_t* to,
                           const char* shared_libs_sonames);

    // Load a library within a namespace. Thin adapter: ignore the
    // namespace and fall back to loadLibrary.
    void* (*loadLibraryExt)(const char* libpath, int flag,
                            native_bridge_namespace_t* ns);

    // ── Added in version 4 ────────────────────────────────────────────
    // Vendor namespace (pre-Q). NULL if not set up.
    native_bridge_namespace_t* (*getVendorNamespace)();

    // ── Added in version 5 ────────────────────────────────────────────
    // Exported namespace by name. NULL if not set up.
    native_bridge_namespace_t* (*getExportedNamespace)(const char* name);

    // ── Added in version 6 ────────────────────────────────────────────
    // Clean up the environment before forking from zygote. No-op here.
    void (*preZygoteFork)();

    // ── Added in version 7 ────────────────────────────────────────────
    // Get a trampoline with an explicit JNI call type. Regular routes to
    // getTrampoline; CriticalNative is unsupported (returns NULL).
    void* (*getTrampolineWithJNICallType)(void* handle, const char* name,
                                          const char* shorty, uint32_t len,
                                          JNICallType jni_call_type);

    // Trampoline for a raw method implementation pointer. Unsupported.
    void* (*getTrampolineForFunctionPointer)(const void* method,
                                             const char* shorty,
                                             uint32_t len,
                                             JNICallType jni_call_type);

    // ── Added in version 8 ────────────────────────────────────────────
    // Whether a method pointer lives in bridge executable space.
    // Always false here (no borrowed method pointers).
    bool (*isNativeBridgeFunctionPointer)(const void* method);
} NativeBridgeCallbacks;

// The global interface table ART resolves via dlsym(handle, "NativeBridgeItf").
// Filled in by bifrost_nb_init().
extern NativeBridgeCallbacks NativeBridgeItf;

// Initialize the native bridge adapter for `emu`. Wires NativeBridgeItf
// (version 4, thin adapter) so ART can load AArch64 libraries and call
// their JNI native methods through libbifrost's borrow-CPU guest calls.
// Returns 0 on success, -1 on error (NULL emu).
int bifrost_nb_init(bifrost_emu_t* emu);

// Detach the adapter (releases the emu reference). Any subsequently
// issued callbacks return NULL/false. Returns 0.
int bifrost_nb_shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif