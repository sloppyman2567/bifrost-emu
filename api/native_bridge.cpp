// api/native_bridge.cpp — Android libnativebridge adapter for libbifrost.
//
// Fills the `NativeBridgeItf` interface table so ART can use libbifrost as
// a native bridge (-XX:NativeBridge): load AArch64 shared objects into the
// emulator and obtain host-callable trampolines that forward JNI native
// method calls into the guest.
//
// Thin adapter scope (per project decision): loadLibrary/isSupported/
// getError/getSignalHandler + borrow-CPU trampolines for scalar shorty
// signatures. No namespace support (createNamespace → NULL), no
// CriticalNative, no borrowed method pointers. Trampolines are built with
// libffi closures so the host callable has the exact JNI native-method
// signature; the closure body splits args into x-regs/d-regs per AAPCS and
// drives the guest through bifrost_call/bifrost_call_f.
//
// The adapter is deliberately global (one emu at a time), matching ART's
// model of one native bridge per process. bifrost_nb_init() wires it up.
#include "native_bridge.h"
#include "bifrost.h"
#include <ffi.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

// ── Global state ────────────────────────────────────────────────────────
static bifrost_emu_t* g_nb_emu = nullptr;
static const struct NativeBridgeRuntimeCallbacks* g_runtime_cbs = nullptr;

// Trampoline tracking: closures must outlive the callbacks that use them,
// and unloadLibrary must free them for the given handle.
struct NbTrampData {
    bifrost_emu_t* emu;
    uint64_t guest_fn;
    char shorty[64];
    uint32_t len;
};
struct NbTramp {
    ffi_closure* closure;
    ffi_cif cif;
    // ffi_prep_cif keeps a pointer to the arg-types array (it does NOT copy
    // it), so the array must outlive the cif — store it in the trampoline
    // instead of a stack-local. Max 2 fixed (env/thiz) + 14 shorty args.
    ffi_type* atypes[16];
    void* code;
    NbTrampData* data;
    uint64_t handle;
};
static std::vector<NbTramp*> g_trampolines;
static std::mutex g_tramp_mu;

// ── Shorty ↔ ffi_type / marshalling ─────────────────────────────────────
// JNI shorty characters (ART): first char = return type, rest = arg types.
//   Z boolean, B byte, C char, S short, I int, J long, F float, D double,
//   L object/array (pointer), V void (return only).
static ffi_type* shorty_ffi_type(char c) {
    switch (c) {
        case 'Z': return &ffi_type_uint8;
        case 'B': return &ffi_type_sint8;
        case 'C': return &ffi_type_uint16;
        case 'S': return &ffi_type_sint16;
        case 'I': return &ffi_type_sint32;
        case 'J': return &ffi_type_sint64;
        case 'F': return &ffi_type_float;
        case 'D': return &ffi_type_double;
        case 'L': return &ffi_type_pointer;
        case 'V': return &ffi_type_void;
        default: return nullptr;
    }
}

// The host native-method signature is ret f(JNIEnv*, jobject, <args>). The
// libffi closure hands us a pointer to each argument value; we split them
// into the guest's x-regs (iargs) and d-regs (fargs) exactly as AAPCS
// classifies them, then drive the borrow-CPU call and write the result.
static void nb_closure_fn(ffi_cif* cif, void* ret, void** args, void* userdata) {
    (void)cif;
    NbTrampData* d = static_cast<NbTrampData*>(userdata);
    int64_t iargs[16] = {0};
    double fargs[8] = {0};
    size_t ni = 0, nf = 0;
    // args[0] = JNIEnv*, args[1] = jobject/class → x0, x1
    iargs[ni++] = static_cast<int64_t>(reinterpret_cast<intptr_t>(*static_cast<void**>(args[0])));
    iargs[ni++] = static_cast<int64_t>(reinterpret_cast<intptr_t>(*static_cast<void**>(args[1])));
    for (uint32_t i = 1; i < d->len; i++) {
        char c = d->shorty[i];
        void* ap = args[i + 1];
        switch (c) {
            case 'F': {
                // Guest expects the float in the LOW 32 bits of the s/d
                // register. call_guest_function memcpy's the 8-byte double
                // into v_lo[], so pack the float bits into the low 32 and
                // zero the high 32.
                float f = *static_cast<float*>(ap);
                uint32_t fb;
                std::memcpy(&fb, &f, 4);
                double packed = 0.0;
                std::memcpy(&packed, &fb, 4);
                fargs[nf++] = packed;
                break;
            }
            case 'D':
                fargs[nf++] = *static_cast<double*>(ap);
                break;
            case 'Z': iargs[ni++] = *static_cast<uint8_t*>(ap); break;
            case 'B': iargs[ni++] = *static_cast<int8_t*>(ap); break;
            case 'C': iargs[ni++] = *static_cast<uint16_t*>(ap); break;
            case 'S': iargs[ni++] = *static_cast<int16_t*>(ap); break;
            case 'I': iargs[ni++] = *static_cast<int32_t*>(ap); break;
            case 'J': iargs[ni++] = *static_cast<int64_t*>(ap); break;
            case 'L':
                iargs[ni++] = static_cast<int64_t>(
                    reinterpret_cast<intptr_t>(*static_cast<void**>(ap)));
                break;
            default:
                iargs[ni++] = 0;
                break;
        }
        if (ni >= 16 || nf >= 8) break;
    }
    char rt = d->shorty[0];
    if (rt == 'F' || rt == 'D') {
        // FP return: guest leaves the result in s0/d0 (low 32 / full 64 of
        // v_lo[0]); bifrost_call_f returns that as a double. A float result
        // occupies the low 32 bits of the returned double's bit pattern.
        double fres = bifrost_call_f(d->emu, d->guest_fn, iargs, ni, fargs, nf);
        if (rt == 'F') {
            uint32_t fb;
            std::memcpy(&fb, &fres, 4);
            float f;
            std::memcpy(&f, &fb, 4);
            std::memcpy(ret, &f, 4);
        } else {
            std::memcpy(ret, &fres, 8);
        }
        return;
    }
    uint64_t x0 = bifrost_call(d->emu, d->guest_fn, iargs, ni, fargs, nf);
    switch (rt) {
        case 'V': break;
        case 'Z': { uint8_t v = static_cast<uint8_t>(x0); std::memcpy(ret, &v, 1); break; }
        case 'B': { int8_t v = static_cast<int8_t>(x0); std::memcpy(ret, &v, 1); break; }
        case 'C': { uint16_t v = static_cast<uint16_t>(x0); std::memcpy(ret, &v, 2); break; }
        case 'S': { int16_t v = static_cast<int16_t>(x0); std::memcpy(ret, &v, 2); break; }
        case 'I': { int32_t v = static_cast<int32_t>(x0); std::memcpy(ret, &v, 4); break; }
        case 'J': { int64_t v = static_cast<int64_t>(x0); std::memcpy(ret, &v, 8); break; }
        case 'L': { void* v = reinterpret_cast<void*>(static_cast<intptr_t>(x0)); std::memcpy(ret, &v, 8); break; }
        default: break;
    }
}

// ── The callbacks ───────────────────────────────────────────────────────
static bool nb_initialize(const struct NativeBridgeRuntimeCallbacks* runtime_cbs,
                          const char* private_dir, const char* instruction_set) {
    (void)private_dir;
    g_runtime_cbs = runtime_cbs;
    // Only support AArch64 guests.
    if (instruction_set == nullptr) return true;
    return std::strcmp(instruction_set, "arm64") == 0 ||
           std::strcmp(instruction_set, "aarch64") == 0;
}

static void* nb_load_library(const char* libpath, int flag) {
    if (!g_nb_emu || !libpath) return nullptr;
    uint64_t h = bifrost_dlopen(g_nb_emu, libpath, flag);
    return h == 0 ? nullptr : reinterpret_cast<void*>(static_cast<uintptr_t>(h));
}

static void* nb_get_trampoline(void* handle, const char* name,
                               const char* shorty, uint32_t len) {
    if (!g_nb_emu || !handle || !name || !shorty || len == 0 || len > 32)
        return nullptr;
    uint64_t guest_fn = bifrost_dlsym(g_nb_emu,
                                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)),
                                      name);
    if (guest_fn == 0) return nullptr;
    ffi_type* ret_t = shorty_ffi_type(shorty[0]);
    if (!ret_t) return nullptr;
    ffi_type* atypes[16] = {&ffi_type_pointer, &ffi_type_pointer};
    int nargs = 2;
    for (uint32_t i = 1; i < len; i++) {
        if (nargs >= 16) return nullptr;
        ffi_type* t = shorty_ffi_type(shorty[i]);
        if (!t) return nullptr;
        atypes[nargs++] = t;
    }
    NbTramp* tramp = new (std::nothrow) NbTramp();
    NbTrampData* data = new (std::nothrow) NbTrampData();
    if (!tramp || !data) {
        delete tramp;
        delete data;
        return nullptr;
    }
    std::memcpy(tramp->atypes, atypes, sizeof(atypes));
    data->emu = g_nb_emu;
    data->guest_fn = guest_fn;
    data->len = len;
    std::memcpy(data->shorty, shorty, len);
    if (ffi_prep_cif(&tramp->cif, FFI_DEFAULT_ABI, nargs, ret_t, tramp->atypes) != FFI_OK) {
        delete tramp;
        delete data;
        return nullptr;
    }
    tramp->closure = static_cast<ffi_closure*>(
        ffi_closure_alloc(sizeof(ffi_closure), &tramp->code));
    if (!tramp->closure) {
        delete tramp;
        delete data;
        return nullptr;
    }
    if (ffi_prep_closure_loc(tramp->closure, &tramp->cif, nb_closure_fn, data,
                             tramp->code) != FFI_OK) {
        ffi_closure_free(tramp->closure);
        delete tramp;
        delete data;
        return nullptr;
    }
    tramp->data = data;
    tramp->handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lk(g_tramp_mu);
    g_trampolines.push_back(tramp);
    return tramp->code;
}

static bool nb_is_supported(const char* libpath) {
    // Valid AArch64 ELF (64-bit, e_machine == 183)?
    if (!libpath) return false;
    FILE* f = std::fopen(libpath, "rb");
    if (!f) return false;
    unsigned char hdr[20] = {0};
    size_t r = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (r < sizeof(hdr)) return false;
    if (!(hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F'))
        return false;
    if (hdr[4] != 2) return false;  // ELFCLASS64
    uint16_t machine = static_cast<uint16_t>(hdr[18] | (hdr[19] << 8));
    return machine == 183;  // EM_AARCH64
}

static const struct NativeBridgeRuntimeValues* nb_get_app_env(const char*) {
    return nullptr;
}

static bool nb_is_compatible_with(uint32_t bridge_version) {
    // Claim version 4 (VENDOR_NAMESPACE, pre-Q) like nb-qemu. libnativebridge
    // asks isCompatibleWith(NAMESPACE_VERSION=3) at load; 3 <= 4 → true. The
    // v7 check (CRITICAL_NATIVE_SUPPORT_VERSION) returns false so ART uses the
    // legacy getTrampoline path, which is what we implement.
    return bridge_version <= 4;
}

static NativeBridgeSignalHandlerFn nb_get_signal_handler(int signal) {
    // The emulator manages its own host signal forwarding (SIGSEGV guest
    // emulation, SIGPROF sampler); ART must not install a bridge handler.
    (void)signal;
    return nullptr;
}

static int nb_unload_library(void* handle) {
    uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    {
        std::lock_guard<std::mutex> lk(g_tramp_mu);
        for (auto it = g_trampolines.begin(); it != g_trampolines.end();) {
            if ((*it)->handle == h) {
                ffi_closure_free((*it)->closure);
                delete (*it)->data;
                delete *it;
                it = g_trampolines.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!g_nb_emu) return -1;
    return bifrost_dlclose(g_nb_emu, h) == 0 ? 0 : -1;
}

static const char* nb_get_error() {
    return g_nb_emu ? bifrost_get_error(g_nb_emu) : "native bridge not initialized";
}

static bool nb_is_path_supported(const char* library_path) {
    // Namespace scenario: ART passes a directory of AArch64 libs. Accept any
    // existing directory (loadLibrary validates individual files).
    if (!library_path) return false;
    FILE* f = std::fopen(library_path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

static bool nb_unused_init_anonymous_namespace(const char*, const char*) {
    return false;
}

static native_bridge_namespace_t* nb_create_namespace(
    const char*, const char*, const char*, uint64_t, const char*,
    native_bridge_namespace_t*) {
    return nullptr;  // no namespace support in the thin adapter
}

static bool nb_link_namespaces(native_bridge_namespace_t*, native_bridge_namespace_t*,
                               const char*) {
    return false;
}

static void* nb_load_library_ext(const char* libpath, int flag, native_bridge_namespace_t* ns) {
    (void)ns;
    return nb_load_library(libpath, flag);
}

static native_bridge_namespace_t* nb_get_vendor_namespace() {
    return nullptr;
}

static native_bridge_namespace_t* nb_get_exported_namespace(const char*) {
    return nullptr;
}

static void nb_pre_zygote_fork() {}

static void* nb_get_trampoline_with_jni_call_type(void* handle, const char* name,
                                                  const char* shorty, uint32_t len,
                                                  JNICallType jni_call_type) {
    if (jni_call_type != kJNICallTypeRegular) return nullptr;
    return nb_get_trampoline(handle, name, shorty, len);
}

static void* nb_get_trampoline_for_function_pointer(const void*, const char*, uint32_t,
                                                    JNICallType) {
    return nullptr;  // unsupported: we cannot map a raw method pointer to a guest fn
}

static bool nb_is_native_bridge_function_pointer(const void*) {
    return false;
}

// ── The interface table ─────────────────────────────────────────────────
// Defined with C linkage (the name is what ART dlsyms). The header's
// extern "C" declaration fixes the linkage; this definition keeps it.
extern "C" {
NativeBridgeCallbacks NativeBridgeItf;
}

int bifrost_nb_init(bifrost_emu_t* emu) {
    if (!emu) return -1;
    g_nb_emu = emu;
    NativeBridgeItf.version = 4;
    NativeBridgeItf.initialize = nb_initialize;
    NativeBridgeItf.loadLibrary = nb_load_library;
    NativeBridgeItf.getTrampoline = nb_get_trampoline;
    NativeBridgeItf.isSupported = nb_is_supported;
    NativeBridgeItf.getAppEnv = nb_get_app_env;
    NativeBridgeItf.isCompatibleWith = nb_is_compatible_with;
    NativeBridgeItf.getSignalHandler = nb_get_signal_handler;
    NativeBridgeItf.unloadLibrary = nb_unload_library;
    NativeBridgeItf.getError = nb_get_error;
    NativeBridgeItf.isPathSupported = nb_is_path_supported;
    NativeBridgeItf.unused_initAnonymousNamespace = nb_unused_init_anonymous_namespace;
    NativeBridgeItf.createNamespace = nb_create_namespace;
    NativeBridgeItf.linkNamespaces = nb_link_namespaces;
    NativeBridgeItf.loadLibraryExt = nb_load_library_ext;
    NativeBridgeItf.getVendorNamespace = nb_get_vendor_namespace;
    NativeBridgeItf.getExportedNamespace = nb_get_exported_namespace;
    NativeBridgeItf.preZygoteFork = nb_pre_zygote_fork;
    NativeBridgeItf.getTrampolineWithJNICallType = nb_get_trampoline_with_jni_call_type;
    NativeBridgeItf.getTrampolineForFunctionPointer = nb_get_trampoline_for_function_pointer;
    NativeBridgeItf.isNativeBridgeFunctionPointer = nb_is_native_bridge_function_pointer;
    return 0;
}

int bifrost_nb_shutdown(void) {
    {
        std::lock_guard<std::mutex> lk(g_tramp_mu);
        for (NbTramp* t : g_trampolines) {
            ffi_closure_free(t->closure);
            delete t->data;
            delete t;
        }
        g_trampolines.clear();
    }
    g_nb_emu = nullptr;
    g_runtime_cbs = nullptr;
    return 0;
}