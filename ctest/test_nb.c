// test_nb.c — Test the Android native bridge adapter (libbifrost.a +
// api/native_bridge.h).
//
// Drives the NativeBridgeItf table exactly as ART's libnativebridge would:
//  1. bifrost_nb_init() wires the callbacks.
//  2. isCompatibleWith() decides whether ART may use the bridge.
//  3. loadLibrary() maps a guest AArch64 .so via bifrost_dlopen.
//  4. getTrampoline(handle, name, shorty, len) builds a host-callable
//     libffi closure with the JNI native-method signature.
//  5. Calling that closure forwards the call into the emulated guest
//     (borrow-CPU bifrost_call/bifrost_call_f) and returns the result.
//
// The guest library is ctest/nb_testlib.so (JNI-shaped functions, see
// ctest/nb_lib.c). Compiles as pure C, links libbifrost.a + libffi.
#include "bifrost.h"
#include "native_bridge.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

int main(int argc, const char* argv[]) {
    printf("=== Native Bridge Test (libbifrost.a + libffi) ===\n\n");

    // ── init ──────────────────────────────────────────────────────────
    CHECK(bifrost_nb_init(NULL) == -1, "bifrost_nb_init(NULL) fails");

    bifrost_emu_t* emu = bifrost_create();
    CHECK(emu != NULL, "bifrost_create()");

    int r = bifrost_load_elf(emu, "ctest/hello.elf", argc, argv);
    CHECK(r == 0, "bifrost_load_elf(\"ctest/hello.elf\")");

    CHECK(bifrost_nb_init(emu) == 0, "bifrost_nb_init(emu)");

    // ── interface table ───────────────────────────────────────────────
    CHECK(NativeBridgeItf.version == 4, "version == 4 (nb-qemu-style claim)");
    CHECK(NativeBridgeItf.initialize != NULL, "initialize wired");
    CHECK(NativeBridgeItf.loadLibrary != NULL, "loadLibrary wired");
    CHECK(NativeBridgeItf.getTrampoline != NULL, "getTrampoline wired");
    CHECK(NativeBridgeItf.isSupported != NULL, "isSupported wired");
    CHECK(NativeBridgeItf.isCompatibleWith != NULL, "isCompatibleWith wired");
    CHECK(NativeBridgeItf.unloadLibrary != NULL, "unloadLibrary wired");
    CHECK(NativeBridgeItf.getError != NULL, "getError wired");

    // ── compatibility ─────────────────────────────────────────────────
    // libnativebridge asks isCompatibleWith(NAMESPACE_VERSION=3) at load.
    CHECK(NativeBridgeItf.isCompatibleWith(3) == true,
          "isCompatibleWith(3) true (NAMESPACE_VERSION)");
    CHECK(NativeBridgeItf.isCompatibleWith(4) == true,
          "isCompatibleWith(4) true (same version)");
    // v7 (CRITICAL_NATIVE_SUPPORT) must be false so ART uses the legacy
    // getTrampoline path (which we implement), not getTrampolineWithJNICallType.
    CHECK(NativeBridgeItf.isCompatibleWith(7) == false,
          "isCompatibleWith(7) false (no CriticalNative)");
    CHECK(NativeBridgeItf.isCompatibleWith(100) == false,
          "isCompatibleWith(100) false (too new)");

    // ── signal / app-env ──────────────────────────────────────────────
    CHECK(NativeBridgeItf.getSignalHandler(SIGSEGV) == NULL,
          "getSignalHandler(SIGSEGV) NULL (emulator manages its own)");
    CHECK(NativeBridgeItf.getSignalHandler(SIGPROF) == NULL,
          "getSignalHandler(SIGPROF) NULL");
    CHECK(NativeBridgeItf.getAppEnv("arm64") == NULL,
          "getAppEnv(\"arm64\") NULL (no extra env)");
    CHECK(NativeBridgeItf.isNativeBridgeFunctionPointer((const void*)0x1) == false,
          "isNativeBridgeFunctionPointer false");

    // ── isSupported (AArch64 ELF check) ───────────────────────────────
    CHECK(NativeBridgeItf.isSupported("ctest/nb_testlib.so") == true,
          "isSupported(nb_testlib.so) true (AArch64 ELF)");
    CHECK(NativeBridgeItf.isSupported("ctest/test_nb.c") == false,
          "isSupported(test_nb.c) false (not an ELF)");
    CHECK(NativeBridgeItf.isSupported("/nonexistent_bifrost_xyz") == false,
          "isSupported(nonexistent) false");
    CHECK(NativeBridgeItf.isSupported(NULL) == false, "isSupported(NULL) false");

    // ── loadLibrary + getTrampoline ───────────────────────────────────
    void* h = NativeBridgeItf.loadLibrary("ctest/nb_testlib.so", 1);
    CHECK(h != NULL, "loadLibrary(nb_testlib.so) -> handle");

    // nb_add: shorty "JJJ" = jlong f(JNIEnv*, jobject, jlong, jlong).
    typedef long (*nb_add_fn)(void*, void*, long, long);
    nb_add_fn add = (nb_add_fn)NativeBridgeItf.getTrampoline(h, "nb_add", "JJJ", 3);
    CHECK(add != NULL, "getTrampoline(nb_add, \"JJJ\")");
    if (add) {
        CHECK(add(NULL, NULL, 20, 22) == 42, "nb_add(20, 22) == 42");
        CHECK(add(NULL, NULL, -7, 5) == -2, "nb_add(-7, 5) == -2");
    }

    // nb_fadd: shorty "DDD" = jdouble f(JNIEnv*, jobject, jdouble, jdouble).
    typedef double (*nb_fadd_fn)(void*, void*, double, double);
    nb_fadd_fn fadd = (nb_fadd_fn)NativeBridgeItf.getTrampoline(h, "nb_fadd", "DDD", 3);
    CHECK(fadd != NULL, "getTrampoline(nb_fadd, \"DDD\")");
    if (fadd) {
        CHECK(fadd(NULL, NULL, 1.5, 2.25) == 3.75, "nb_fadd(1.5, 2.25) == 3.75");
        CHECK(fadd(NULL, NULL, -0.5, 0.25) == -0.25, "nb_fadd(-0.5, 0.25) == -0.25");
    }

    // nb_gets: shorty "JJ" = jlong f(JNIEnv*, jobject, jlong) & 0xff.
    typedef long (*nb_gets_fn)(void*, void*, long);
    nb_gets_fn gets = (nb_gets_fn)NativeBridgeItf.getTrampoline(h, "nb_gets", "JJ", 2);
    CHECK(gets != NULL, "getTrampoline(nb_gets, \"JJ\")");
    if (gets) {
        CHECK(gets(NULL, NULL, 0x1ab) == 0xab, "nb_gets(0x1ab) == 0xab");
        CHECK(gets(NULL, NULL, 0x100) == 0, "nb_gets(0x100) == 0");
    }

    // nb_mix: shorty "DID" = jdouble f(JNIEnv*, jobject, jint, jdouble).
    typedef double (*nb_mix_fn)(void*, void*, int, double);
    nb_mix_fn mix = (nb_mix_fn)NativeBridgeItf.getTrampoline(h, "nb_mix", "DID", 3);
    CHECK(mix != NULL, "getTrampoline(nb_mix, \"DID\")");
    if (mix) {
        CHECK(mix(NULL, NULL, 5, 2.5) == 7.5, "nb_mix(5, 2.5) == 7.5");
        CHECK(mix(NULL, NULL, -3, 1.5) == -1.5, "nb_mix(-3, 1.5) == -1.5");
    }

    // nb_fmul: shorty "FFF" = jfloat f(JNIEnv*, jobject, jfloat, jfloat).
    typedef float (*nb_fmul_fn)(void*, void*, float, float);
    nb_fmul_fn fmul = (nb_fmul_fn)NativeBridgeItf.getTrampoline(h, "nb_fmul", "FFF", 3);
    CHECK(fmul != NULL, "getTrampoline(nb_fmul, \"FFF\")");
    if (fmul) {
        CHECK(fmul(NULL, NULL, 1.5f, 2.0f) == 3.0f, "nb_fmul(1.5f, 2.0f) == 3.0f");
        CHECK(fmul(NULL, NULL, -2.0f, 4.0f) == -8.0f, "nb_fmul(-2.0f, 4.0f) == -8.0f");
    }

    // Unknown symbol -> NULL trampoline.
    CHECK(NativeBridgeItf.getTrampoline(h, "definitely_not_here", "JJJ", 3) == NULL,
          "getTrampoline(unknown symbol) NULL");
    // Bad shorty -> NULL.
    CHECK(NativeBridgeItf.getTrampoline(h, "nb_add", "QQQ", 3) == NULL,
          "getTrampoline(bad shorty 'Q') NULL");
    // JNICallType routing: CriticalNative unsupported, Regular routes.
    CHECK(NativeBridgeItf.getTrampolineWithJNICallType(h, "nb_add", "JJJ", 3,
                                                       kJNICallTypeCriticalNative) == NULL,
          "getTrampolineWithJNICallType(CriticalNative) NULL");
    void* reg_t = NativeBridgeItf.getTrampolineWithJNICallType(h, "nb_add", "JJJ", 3,
                                                               kJNICallTypeRegular);
    CHECK(reg_t != NULL, "getTrampolineWithJNICallType(Regular) works");
    if (reg_t) {
        nb_add_fn add2 = (nb_add_fn)reg_t;
        CHECK(add2(NULL, NULL, 100, 1) == 101, "Regular trampoline nb_add(100, 1) == 101");
    }
    CHECK(NativeBridgeItf.getTrampolineForFunctionPointer((const void*)0x1, "JJJ", 3,
                                                          kJNICallTypeRegular) == NULL,
          "getTrampolineForFunctionPointer NULL (unsupported)");

    // ── loadLibrary failure / getError ────────────────────────────────
    CHECK(NativeBridgeItf.loadLibrary("/nonexistent_bifrost_xyz", 1) == NULL,
          "loadLibrary(nonexistent) NULL");
    CHECK(NativeBridgeItf.getError() != NULL, "getError() non-NULL");

    // ── unloadLibrary ─────────────────────────────────────────────────
    // First unload drops the refcount (loadLibrary bumped it). Second
    // unload hits refcount 0 -> error (matches glibc/dlclose).
    CHECK(NativeBridgeItf.unloadLibrary(h) == 0, "unloadLibrary(handle) == 0");
    CHECK(NativeBridgeItf.unloadLibrary(h) != 0, "unloadLibrary(handle) again != 0 (refcount 0)");

    // ── C API guest dl* surface (used by the adapter) ─────────────────
    uint64_t handle = bifrost_dlopen(emu, "ctest/nb_testlib.so", 1);
    CHECK(handle != 0, "bifrost_dlopen(nb_testlib.so) -> handle");
    uint64_t sym = bifrost_dlsym(emu, handle, "nb_add");
    CHECK(sym != 0, "bifrost_dlsym(handle, \"nb_add\") -> guest addr");
    CHECK(bifrost_dlsym(emu, handle, "definitely_not_here") == 0,
          "bifrost_dlsym(unknown) -> 0");
    CHECK(bifrost_dlsym(emu, handle, NULL) == 0, "bifrost_dlsym(NULL name) -> 0");
    CHECK(bifrost_dlclose(emu, handle) == 0, "bifrost_dlclose(handle) == 0");
    CHECK(bifrost_dlclose(emu, handle) != 0, "bifrost_dlclose(handle) again != 0");
    CHECK(bifrost_dlopen(NULL, "ctest/nb_testlib.so", 1) == 0, "bifrost_dlopen(NULL) -> 0");
    CHECK(bifrost_dlsym(NULL, handle, "nb_add") == 0, "bifrost_dlsym(NULL) -> 0");
    CHECK(bifrost_dlclose(NULL, handle) == -1, "bifrost_dlclose(NULL) == -1");

    // ── shutdown ──────────────────────────────────────────────────────
    CHECK(bifrost_nb_shutdown() == 0, "bifrost_nb_shutdown() == 0");
    CHECK(NativeBridgeItf.loadLibrary("ctest/nb_testlib.so", 1) == NULL,
          "loadLibrary after shutdown NULL");

    bifrost_destroy(emu);
    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}