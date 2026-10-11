// test_android_looper.c — per-thread ALooper, AChoreographer frame
// callbacks and the no-sensor ASensorManager (libandroid.so thunks).
//
// Expected values follow AOSP android15-release: libutils Looper.cpp
// (thread-local loopers, addFd rules, callback return 0 unregisters),
// libs/gui Choreographer.cpp (per-thread instance, NULL without a looper,
// one-shot callbacks dispatched by the owning thread's poll with the vsync
// timestamp) and native/android/sensor.cpp with an empty sensor list.
// Headless; prints "ALL PASS" on success.
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct ALooper ALooper;
typedef struct AChoreographer AChoreographer;
typedef struct ASensorManager ASensorManager;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ASensor ASensor;
typedef int (*ALooper_callbackFunc)(int, int, void*);

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

static int checks = 0;
static void chk(int ok, const char* what) {
    checks++;
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) exit(1);
}

static ALooper* (*ALooper_forThread)(void);
static ALooper* (*ALooper_prepare)(int);
static int (*ALooper_pollOnce)(int, int*, int*, void**);
static int (*ALooper_pollAll)(int, int*, int*, void**);
static int (*ALooper_addFd)(ALooper*, int, int, int, ALooper_callbackFunc, void*);
static void (*ALooper_wake)(ALooper*);
static AChoreographer* (*AChoreographer_getInstance)(void);
static void (*AChoreographer_postFrameCallback)(AChoreographer*, void (*)(long, void*), void*);
static void (*AChoreographer_postFrameCallback64)(AChoreographer*, void (*)(int64_t, void*), void*);
static void (*AChoreographer_postFrameCallbackDelayed64)(AChoreographer*, void (*)(int64_t, void*),
                                                          void*, uint32_t);
static ASensorManager* (*ASensorManager_getInstance)(void);
static int (*ASensorManager_getSensorList)(ASensorManager*, const ASensor* const**);
static const ASensor* (*ASensorManager_getDefaultSensor)(ASensorManager*, int);
static ASensorEventQueue* (*ASensorManager_createEventQueue)(ASensorManager*, ALooper*, int,
                                                             ALooper_callbackFunc, void*);
static int (*ASensorManager_destroyEventQueue)(ASensorManager*, ASensorEventQueue*);
static int (*ASensorEventQueue_enableSensor)(ASensorEventQueue*, const ASensor*);
static int (*ASensorEventQueue_setEventRate)(ASensorEventQueue*, const ASensor*, int32_t);
static int (*ASensorEventQueue_hasEvents)(ASensorEventQueue*);
static long (*ASensorEventQueue_getEvents)(ASensorEventQueue*, void*, size_t);
static const char* (*ASensor_getName)(const ASensor*);
static int (*ASensor_getType)(const ASensor*);
static float (*ASensor_getResolution)(const ASensor*);
static int (*ASensor_getMinDelay)(const ASensor*);

#define PERIOD_NS 16666667LL

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static long my_tid(void) { return syscall(SYS_gettid); }

struct frame_rec {
    int count;
    int64_t ts[4];
    int64_t seen_at[4];
    long tid;
    int repost;            // repost from inside the callback this many times
    AChoreographer* choreo;
    ALooper* wake;         // woken after the last repost
};
static void frame64(int64_t t, void* data) {
    struct frame_rec* r = data;
    if (r->count < 4) {
        r->ts[r->count] = t;
        r->seen_at[r->count] = now_ns();
    }
    r->count++;
    r->tid = my_tid();
    if (r->repost > 0) {
        r->repost--;
        AChoreographer_postFrameCallback64(r->choreo, frame64, r);
    } else if (r->wake) {
        ALooper_wake(r->wake);
    }
}
static void frame32(long t, void* data) { frame64((int64_t)t, data); }

static int fd_calls;
static int fd_once(int fd, int events, void* data) {
    char c;
    (void)events;
    (void)data;
    read(fd, &c, 1);
    fd_calls++;
    return 0;  // unregister
}

static AChoreographer* main_choreo;
static ALooper* main_looper;
static struct frame_rec cross;
static struct frame_rec main_pending;
static void* other_thread(void* arg) {
    int* result = arg;
    int ofd, oev;
    void* odata;
    result[0] = ALooper_forThread() == 0;
    result[1] = AChoreographer_getInstance() == 0;
    ALooper* mine = ALooper_prepare(0);
    result[2] = mine != 0 && mine != main_looper && ALooper_forThread() == mine;
    result[3] = AChoreographer_getInstance() != 0 &&
                AChoreographer_getInstance() != main_choreo;
    // Main's pending frame callback must not run on this thread's looper.
    result[4] = ALooper_pollOnce(60, &ofd, &oev, &odata) == -3 &&
                main_pending.count == 0;
    // Posting to another thread's choreographer queues on its looper.
    AChoreographer_postFrameCallback64(main_choreo, frame64, &cross);
    return 0;
}

#define LOAD(h, name) do { *(void**)&name = (void*)(uintptr_t)bifrost_dlsym(h, #name); \
    if (!name) { printf("FAIL: dlsym " #name "\n"); return 1; } } while (0)

int main(void) {
    printf("test_android_looper: start\n");
    uint64_t h = bifrost_dlopen("libandroid.so", 1);
    chk(h != 0, "dlopen libandroid.so");
    LOAD(h, ALooper_forThread);
    LOAD(h, ALooper_prepare);
    LOAD(h, ALooper_pollOnce);
    LOAD(h, ALooper_pollAll);
    LOAD(h, ALooper_addFd);
    LOAD(h, ALooper_wake);
    LOAD(h, AChoreographer_getInstance);
    LOAD(h, AChoreographer_postFrameCallback);
    LOAD(h, AChoreographer_postFrameCallback64);
    LOAD(h, AChoreographer_postFrameCallbackDelayed64);
    LOAD(h, ASensorManager_getInstance);
    LOAD(h, ASensorManager_getSensorList);
    LOAD(h, ASensorManager_getDefaultSensor);
    LOAD(h, ASensorManager_createEventQueue);
    LOAD(h, ASensorManager_destroyEventQueue);
    LOAD(h, ASensorEventQueue_enableSensor);
    LOAD(h, ASensorEventQueue_setEventRate);
    LOAD(h, ASensorEventQueue_hasEvents);
    LOAD(h, ASensorEventQueue_getEvents);
    LOAD(h, ASensor_getName);
    LOAD(h, ASensor_getType);
    LOAD(h, ASensor_getResolution);
    LOAD(h, ASensor_getMinDelay);

    int ofd, oev;
    void* odata;

    // ── Per-thread looper ────────────────────────────────────────────
    chk(ALooper_forThread() == 0, "forThread before prepare -> NULL");
    chk(AChoreographer_getInstance() == 0, "choreographer without looper -> NULL");
    chk(ALooper_pollOnce(0, &ofd, &oev, &odata) == -4, "pollOnce without looper -> ERROR");
    ALooper* looper = ALooper_prepare(0);
    chk(looper != 0 && ALooper_forThread() == looper, "prepare -> forThread");
    chk(ALooper_prepare(1) == looper, "second prepare returns the same looper");

    // ── addFd rules ──────────────────────────────────────────────────
    int pip[2];
    chk(pipe(pip) == 0, "pipe");
    chk(ALooper_addFd(looper, pip[0], 5, 1, 0, 0) == -1,
        "NULL callback without ALLOW_NON_CALLBACKS -> -1");
    chk(ALooper_addFd(looper, pip[0], 5, 1, fd_once, 0) == 1, "addFd with callback");
    write(pip[1], "a", 1);
    chk(ALooper_pollOnce(100, &ofd, &oev, &odata) == -2 && fd_calls == 1,
        "fd callback -> POLL_CALLBACK");
    write(pip[1], "b", 1);
    chk(ALooper_pollOnce(0, &ofd, &oev, &odata) == -3 && fd_calls == 1,
        "callback returning 0 unregistered the fd");
    close(pip[0]);
    close(pip[1]);

    // ── Choreographer: one-shot frame callbacks ──────────────────────
    main_choreo = AChoreographer_getInstance();
    chk(main_choreo != 0 && AChoreographer_getInstance() == main_choreo,
        "getInstance per thread");
    struct frame_rec one = {0};
    int64_t posted = now_ns();
    AChoreographer_postFrameCallback64(main_choreo, frame64, &one);
    chk(ALooper_pollOnce(200, &ofd, &oev, &odata) == -2 && one.count == 1,
        "frame callback -> POLL_CALLBACK");
    chk(one.ts[0] > posted && one.ts[0] <= one.seen_at[0] && one.ts[0] % PERIOD_NS == 0,
        "frame time is the vsync after the post");
    chk(one.tid == my_tid(), "frame callback runs on the looper thread");
    chk(ALooper_pollOnce(40, &ofd, &oev, &odata) == -3 && one.count == 1,
        "frame callback is one-shot");

    // A callback that reposts itself runs once per vsync; pollAll keeps
    // waiting through callbacks until the wake.
    struct frame_rec chain = {0};
    chain.repost = 2;
    chain.choreo = main_choreo;
    chain.wake = looper;
    AChoreographer_postFrameCallback(main_choreo, frame32, &chain);
    chk(ALooper_pollAll(1000, &ofd, &oev, &odata) == -1 && chain.count == 3,
        "reposted 32-bit callback x3 then wake");
    chk(chain.ts[1] - chain.ts[0] >= PERIOD_NS && chain.ts[2] - chain.ts[1] >= PERIOD_NS,
        "reposts wait for the next vsync");

    struct frame_rec delayed = {0};
    posted = now_ns();
    AChoreographer_postFrameCallbackDelayed64(main_choreo, frame64, &delayed, 80);
    chk(ALooper_pollOnce(30, &ofd, &oev, &odata) == -3 && delayed.count == 0,
        "delayed callback not due yet");
    chk(ALooper_pollOnce(300, &ofd, &oev, &odata) == -2 && delayed.count == 1 &&
        delayed.ts[0] - posted >= 80000000LL, "delayed callback after 80 ms");

    // ── Thread isolation ─────────────────────────────────────────────
    AChoreographer_postFrameCallback64(main_choreo, frame64, &main_pending);
    main_looper = looper;
    int result[5] = {0};
    pthread_t t;
    chk(pthread_create(&t, 0, other_thread, result) == 0, "pthread_create");
    pthread_join(t, 0);
    chk(result[0] && result[1], "new thread starts without looper/choreographer");
    chk(result[2], "new thread gets its own looper");
    chk(result[3], "new thread gets its own choreographer");
    chk(result[4], "other thread's poll leaves main's callback pending");
    // The cross-thread post may still be waiting for its vsync.
    for (int i = 0; i < 4 && (main_pending.count == 0 || cross.count == 0); i++)
        ALooper_pollOnce(200, &ofd, &oev, &odata);
    chk(main_pending.count == 1 && cross.count == 1 && main_pending.tid == my_tid() &&
        cross.tid == my_tid(), "main and cross-thread callbacks run on main");

    // ── Sensors: a device with none ──────────────────────────────────
    ASensorManager* mgr = ASensorManager_getInstance();
    chk(mgr != 0, "ASensorManager_getInstance");
    const ASensor* const* list = 0;
    chk(ASensorManager_getSensorList(mgr, &list) == 0 && list != 0, "empty sensor list");
    chk(ASensorManager_getSensorList(0, &list) == -22, "getSensorList NULL manager -> BAD_VALUE");
    chk(ASensorManager_getDefaultSensor(mgr, 1) == 0, "no default accelerometer");
    chk(ASensorManager_createEventQueue(mgr, 0, 1, 0, 0) == 0, "queue needs a looper");
    ASensorEventQueue* q = ASensorManager_createEventQueue(mgr, looper, 3, 0, 0);
    chk(q != 0, "createEventQueue");
    char events[104 * 4];
    chk(ASensorEventQueue_hasEvents(q) == -1, "hasEvents on empty queue -> -1");
    chk(ASensorEventQueue_getEvents(q, events, 4) == 0, "getEvents -> 0");
    chk(ASensorEventQueue_enableSensor(q, 0) == -22, "enableSensor(NULL) -> BAD_VALUE");
    chk(ASensorEventQueue_setEventRate(q, 0, 20000) == -22, "setEventRate(NULL) -> BAD_VALUE");
    chk(ALooper_pollOnce(0, &ofd, &oev, &odata) == -3, "sensor queue never wakes the looper");
    chk(ASensorManager_destroyEventQueue(mgr, q) == 0, "destroyEventQueue");
    chk(ASensor_getName(0) == 0 && ASensor_getType(0) == -1 &&
        isnan(ASensor_getResolution(0)) && ASensor_getMinDelay(0) == INT32_MIN,
        "NULL-sensor getters return *_INVALID");

    printf("test_android_looper: ALL PASS (%d checks)\n", checks);
    return 0;
}
