// test_android_activity.c — Android lifecycle layer v2 (2026-08).
//
// Covers the framework plumbing that native_app_glue needs so a
// `bifrost-emu --android libfoo.so` run can boot: AConfiguration stubs,
// ALooper (prepare/acquire/poll timeout + fd readiness via the real host
// pipe), AInputQueue (attach/detach/getEvent empty → finish), event
// getters on a null handle, and the liblog stubs. The full lifecycle
// driver plus synthetic tap input is exercised separately with a .so
// built for --android.
//
// Like test_android_surface.elf this test runs under DISPLAY=:0 and
// prints "ALL PASS" on success; run_tests.sh invokes it in the
// android suite slot.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct ALooper ALooper;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct AConfiguration AConfiguration;

// ── bifrost dl* svc trampoline ───────────────────────────────────────
static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t h, const char* name) {
    register uint64_t x0 __asm__("x0") = h;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x1003;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define LOAD(h,T,name) do { name=(T)(uintptr_t)bifrost_dlsym(h,#name); if(!name){printf("FAIL: dlsym " #name "\n"); return 1;} } while(0)

static int checks=0;
static void chk(int ok,const char* what){
    checks++; printf("  %s %s\n", ok?"PASS":"FAIL", what);
    if(!ok) exit(1);
}

// ── libandroid fn types ──────────────────────────────────────────────
typedef ALooper* (*ALooper_prepare_t)(int);
typedef void (*ALooper_acquire_t)(ALooper*);
typedef void (*ALooper_release_t)(ALooper*);
typedef int (*ALooper_pollOnce_t)(int,int*,int*,void**);
typedef int (*ALooper_pollAll_t)(int,int*,int*,void**);
typedef int (*ALooper_addFd_t)(ALooper*,int,int,int,int(*)(int,int,void*),void*);
typedef int (*ALooper_removeFd_t)(ALooper*,int);
typedef void (*ALooper_wake_t)(ALooper*);
typedef void (*AInputQueue_attachLooper_t)(AInputQueue*,ALooper*,int,int(*)(int,int,void*),void*);
typedef void (*AInputQueue_detachLooper_t)(AInputQueue*);
typedef int (*AInputQueue_getEvent_t)(AInputQueue*,AInputEvent**);
typedef int (*AInputQueue_preDispatchEvent_t)(AInputQueue*,AInputEvent*);
typedef void (*AInputQueue_finishEvent_t)(AInputQueue*,AInputEvent*,int);
typedef int (*AInputEvent_getType_t)(AInputEvent*);
typedef int (*AInputEvent_getDeviceId_t)(AInputEvent*);
typedef int (*AInputEvent_getSource_t)(AInputEvent*);
typedef int (*AMotionEvent_getAction_t)(AInputEvent*);
typedef int (*AMotionEvent_getPointerCount_t)(AInputEvent*);
typedef int64_t (*AMotionEvent_getDownTime_t)(AInputEvent*);
typedef int64_t (*AMotionEvent_getEventTime_t)(AInputEvent*);
typedef int (*AMotionEvent_getPointerId_t)(AInputEvent*,size_t);
typedef float (*AMotionEvent_getX_t)(AInputEvent*,size_t);
typedef float (*AMotionEvent_getY_t)(AInputEvent*,size_t);
typedef float (*AMotionEvent_getPressure_t)(AInputEvent*,size_t);
typedef int (*AKeyEvent_getAction_t)(AInputEvent*);
typedef int (*AKeyEvent_getKeyCode_t)(AInputEvent*);
typedef int (*AKeyEvent_getRepeatCount_t)(AInputEvent*);
typedef AConfiguration* (*AConfiguration_new_t)(void);
typedef void (*AConfiguration_delete_t)(AConfiguration*);
typedef void (*AConfiguration_fromAssetManager_t)(AConfiguration*,void*);
typedef int (*AConfiguration_getMcc_t)(AConfiguration*);
typedef int (*AConfiguration_getDensity_t)(AConfiguration*);
typedef int (*AConfiguration_getSdkVersion_t)(AConfiguration*);
typedef void (*AConfiguration_getLanguage_t)(AConfiguration*,char*);
typedef void (*AConfiguration_getCountry_t)(AConfiguration*,char*);
typedef int (*AConfiguration_getOrientation_t)(AConfiguration*);
typedef int (*AConfiguration_getTouchscreen_t)(AConfiguration*);
typedef int (*__android_log_write_t)(int,const char*,const char*);
typedef int (*__android_log_print_t)(int,const char*,const char*,...);

static ALooper_prepare_t ALooper_prepare;
static ALooper_acquire_t ALooper_acquire;
static ALooper_release_t ALooper_release;
static ALooper_pollOnce_t ALooper_pollOnce;
static ALooper_pollAll_t ALooper_pollAll;
static ALooper_addFd_t ALooper_addFd;
static ALooper_removeFd_t ALooper_removeFd;
static ALooper_wake_t ALooper_wake;
static AInputQueue_attachLooper_t AInputQueue_attachLooper;
static AInputQueue_detachLooper_t AInputQueue_detachLooper;
static AInputQueue_getEvent_t AInputQueue_getEvent;
static AInputQueue_preDispatchEvent_t AInputQueue_preDispatchEvent;
static AInputQueue_finishEvent_t AInputQueue_finishEvent;
static AInputEvent_getType_t AInputEvent_getType;
static AInputEvent_getDeviceId_t AInputEvent_getDeviceId;
static AInputEvent_getSource_t AInputEvent_getSource;
static AMotionEvent_getAction_t AMotionEvent_getAction;
static AMotionEvent_getPointerCount_t AMotionEvent_getPointerCount;
static AMotionEvent_getDownTime_t AMotionEvent_getDownTime;
static AMotionEvent_getEventTime_t AMotionEvent_getEventTime;
static AMotionEvent_getPointerId_t AMotionEvent_getPointerId;
static AMotionEvent_getX_t AMotionEvent_getX;
static AMotionEvent_getY_t AMotionEvent_getY;
static AMotionEvent_getPressure_t AMotionEvent_getPressure;
static AKeyEvent_getAction_t AKeyEvent_getAction;
static AKeyEvent_getKeyCode_t AKeyEvent_getKeyCode;
static AKeyEvent_getRepeatCount_t AKeyEvent_getRepeatCount;
static AConfiguration_new_t AConfiguration_new;
static AConfiguration_delete_t AConfiguration_delete;
static AConfiguration_fromAssetManager_t AConfiguration_fromAssetManager;
static AConfiguration_getMcc_t AConfiguration_getMcc;
static AConfiguration_getDensity_t AConfiguration_getDensity;
static AConfiguration_getSdkVersion_t AConfiguration_getSdkVersion;
static AConfiguration_getLanguage_t AConfiguration_getLanguage;
static AConfiguration_getCountry_t AConfiguration_getCountry;
static AConfiguration_getOrientation_t AConfiguration_getOrientation;
static AConfiguration_getTouchscreen_t AConfiguration_getTouchscreen;
static __android_log_write_t __android_log_write;
static __android_log_print_t __android_log_print;

int main(void){
    printf("test_android_activity: start\n");

    uint64_t hand = bifrost_dlopen("libandroid.so",1);
    chk(hand!=0, "dlopen libandroid.so");
    LOAD(hand, ALooper_prepare_t, ALooper_prepare);
    LOAD(hand, ALooper_acquire_t, ALooper_acquire);
    LOAD(hand, ALooper_release_t, ALooper_release);
    LOAD(hand, ALooper_pollOnce_t, ALooper_pollOnce);
    LOAD(hand, ALooper_pollAll_t, ALooper_pollAll);
    LOAD(hand, ALooper_addFd_t, ALooper_addFd);
    LOAD(hand, ALooper_removeFd_t, ALooper_removeFd);
    LOAD(hand, ALooper_wake_t, ALooper_wake);
    LOAD(hand, AInputQueue_attachLooper_t, AInputQueue_attachLooper);
    LOAD(hand, AInputQueue_detachLooper_t, AInputQueue_detachLooper);
    LOAD(hand, AInputQueue_getEvent_t, AInputQueue_getEvent);
    LOAD(hand, AInputQueue_preDispatchEvent_t, AInputQueue_preDispatchEvent);
    LOAD(hand, AInputQueue_finishEvent_t, AInputQueue_finishEvent);
    LOAD(hand, AInputEvent_getType_t, AInputEvent_getType);
    LOAD(hand, AInputEvent_getDeviceId_t, AInputEvent_getDeviceId);
    LOAD(hand, AInputEvent_getSource_t, AInputEvent_getSource);
    LOAD(hand, AMotionEvent_getAction_t, AMotionEvent_getAction);
    LOAD(hand, AMotionEvent_getPointerCount_t, AMotionEvent_getPointerCount);
    LOAD(hand, AMotionEvent_getDownTime_t, AMotionEvent_getDownTime);
    LOAD(hand, AMotionEvent_getEventTime_t, AMotionEvent_getEventTime);
    LOAD(hand, AMotionEvent_getPointerId_t, AMotionEvent_getPointerId);
    LOAD(hand, AMotionEvent_getX_t, AMotionEvent_getX);
    LOAD(hand, AMotionEvent_getY_t, AMotionEvent_getY);
    LOAD(hand, AMotionEvent_getPressure_t, AMotionEvent_getPressure);
    LOAD(hand, AKeyEvent_getAction_t, AKeyEvent_getAction);
    LOAD(hand, AKeyEvent_getKeyCode_t, AKeyEvent_getKeyCode);
    LOAD(hand, AKeyEvent_getRepeatCount_t, AKeyEvent_getRepeatCount);
    LOAD(hand, AConfiguration_new_t, AConfiguration_new);
    LOAD(hand, AConfiguration_delete_t, AConfiguration_delete);
    LOAD(hand, AConfiguration_fromAssetManager_t, AConfiguration_fromAssetManager);
    LOAD(hand, AConfiguration_getMcc_t, AConfiguration_getMcc);
    LOAD(hand, AConfiguration_getDensity_t, AConfiguration_getDensity);
    LOAD(hand, AConfiguration_getSdkVersion_t, AConfiguration_getSdkVersion);
    LOAD(hand, AConfiguration_getLanguage_t, AConfiguration_getLanguage);
    LOAD(hand, AConfiguration_getCountry_t, AConfiguration_getCountry);
    LOAD(hand, AConfiguration_getOrientation_t, AConfiguration_getOrientation);
    LOAD(hand, AConfiguration_getTouchscreen_t, AConfiguration_getTouchscreen);

    uint64_t hlog = bifrost_dlopen("liblog.so",1);
    if(!hlog) hlog = hand; // fallback: resolver also covers --android dual soname
    LOAD(hlog, __android_log_write_t, __android_log_write);
    LOAD(hlog, __android_log_print_t, __android_log_print);

    // ── AConfiguration ───────────────────────────────────────────────
    AConfiguration* cfg = AConfiguration_new();
    chk(cfg!=0, "AConfiguration_new");
    AConfiguration_fromAssetManager(cfg, (void*)0x1);
    chk(AConfiguration_getMcc(cfg)==0, "getMcc == 0");
    chk(AConfiguration_getDensity(cfg)==160, "getDensity 160");
    chk(AConfiguration_getSdkVersion(cfg)==34, "getSdkVersion 34");
    chk(AConfiguration_getOrientation(cfg)==1, "getOrientation 1");
    chk(AConfiguration_getTouchscreen(cfg)==3, "getTouchscreen FINGER");
    char lang[4]={0}, country[4]={0};
    AConfiguration_getLanguage(cfg, lang);
    AConfiguration_getCountry(cfg, country);
    chk(lang[0]=='e'&&lang[1]=='n', "getLanguage en");
    chk(country[0]=='U'&&country[1]=='S', "getCountry US");
    AConfiguration_delete(cfg);
    chk(1, "AConfiguration_delete");

    // ── ALooper ──────────────────────────────────────────────────────
    ALooper* looper = ALooper_prepare(0);
    chk(looper!=0, "ALooper_prepare");
    ALooper_acquire(looper);
    ALooper_release(looper);
    // pollOnce with 0 timeout and no sources: must return POLL_TIMEOUT (-3)
    int ofd=-1, oev=0; void* odata=0;
    int r = ALooper_pollOnce(0,&ofd,&oev,&odata);
    chk(r==-3, "pollOnce 0ms -> TIMEOUT -3");
    r = ALooper_pollAll(0,&ofd,&oev,&odata);
    chk(r==-3, "pollAll 0ms -> TIMEOUT -3");
    // add a pipe fd → become readable → poll returns ident
    int pip[2]; chk(pipe(pip)==0, "pipe");
    void* sentinel=(void*)0x1234;
    int add = ALooper_addFd(looper, pip[0], 42, 1, 0, sentinel);
    chk(add==1, "addFd pipe read end");
    r = ALooper_pollOnce(0,&ofd,&oev,&odata);
    chk(r==-3, "poll before write -> TIMEOUT");
    write(pip[1],"x",1);
    r = ALooper_pollOnce(30,&ofd,&oev,&odata);
    chk(r==42 && ofd==pip[0] && odata==sentinel, "poll after write -> ident 42");
    chk(ALooper_removeFd(looper,pip[0])==1, "removeFd");
    close(pip[0]); close(pip[1]);
    ALooper_wake(looper);
    chk(1, "ALooper_wake");

    // ── AInputQueue (queue empty path) ───────────────────────────────
    // The framework queue handle is a fixed shim; attach/detach should not crash
    // and getEvent on an empty queue must return negative.
    // We use the manager's singleton queue handle 0xA90002000001 directly.
    AInputQueue* q = (AInputQueue*)(uintptr_t)0xA90002000001ULL;
    AInputQueue_attachLooper(q, looper, 2, 0, sentinel);
    chk(1, "attachLooper");
    AInputEvent* ev=0;
    int ge = AInputQueue_getEvent(q,(AInputEvent**)&ev);
    chk(ge<0, "getEvent empty -> <0");
    AInputQueue_detachLooper(q);
    chk(1, "detachLooper");

    // ── Event getters on null / invalid handle → safe zero ───────────
    chk(AInputEvent_getType((AInputEvent*)0x1)==0, "getType invalid -> 0");
    chk(AMotionEvent_getPointerCount((AInputEvent*)0x1)==0, "pointerCount invalid -> 0");
    chk(AMotionEvent_getAction((AInputEvent*)0x1)==0, "getAction invalid -> 0");
    chk(AMotionEvent_getX((AInputEvent*)0x1,0)==0.0f, "getX invalid -> 0");
    chk(AKeyEvent_getKeyCode((AInputEvent*)0x1)==0, "getKeyCode invalid -> 0");

    // ── liblog stubs ─────────────────────────────────────────────────
    chk(__android_log_write(4,"bifrost-test","hello")==0, "log_write");
    chk(__android_log_print(4,"bifrost-test","num=%d str=%s",42,"hi")>0, "log_print");

    printf("test_android_activity: ALL PASS (%d checks)\n", checks);
    return 0;
}
