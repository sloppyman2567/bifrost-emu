// The native half of ManagedNativeActivity: a NativeActivity subclass with Java around it, as Defold and other engines
// ship their games, which runs on the managed path. Its window surface and input queue come from the framework
// (ViewRootImpl's taken surface and InputQueue), so every callback prints a marker the test checks in order. The
// window is drawn a known colour with EGL and read back. Keys A and BACK and touches are handled; every other key and
// joystick motion is left unhandled, so the views (the Java Activity) must receive it, a gamepad button's fallback
// follows and unhandled joystick motion comes back as ViewRootImpl's synthetic d-pad keys.
#include <android/native_activity.h>
#include <stdio.h>
extern void *eglGetDisplay(void *);
extern unsigned int eglInitialize(void *, int *, int *);
extern unsigned int eglChooseConfig(void *, const int *, void **, int, int *);
extern unsigned int eglBindAPI(unsigned int);
extern unsigned int eglGetError(void);
extern void *eglCreateWindowSurface(void *, void *, void *, const int *);
extern void *eglCreateContext(void *, void *, void *, const int *);
extern unsigned int eglMakeCurrent(void *, void *, void *, void *);
extern unsigned int eglSwapBuffers(void *, void *);
extern unsigned int eglDestroySurface(void *, void *);
extern unsigned int eglDestroyContext(void *, void *);
extern unsigned int eglTerminate(void *);
extern void glViewport(int, int, int, int);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned int);
extern void glReadPixels(int, int, int, int, unsigned int, unsigned int, void *);
extern void glFinish(void);

static void *display, *surface, *context;
static int frames;

static void say(const char *line) {
    printf("%s\n", line);
    fflush(stdout);
}
static void started(ANativeActivity *a) { (void)a; say("NATIVE_START"); }
static void resumed(ANativeActivity *a) { (void)a; say("NATIVE_RESUME"); }
static void paused(ANativeActivity *a) { (void)a; say("NATIVE_PAUSE"); }
static void stopped(ANativeActivity *a) { (void)a; say("NATIVE_STOP"); }
static void destroyed(ANativeActivity *a) { (void)a; say("NATIVE_DESTROY"); }
static void focused(ANativeActivity *a, int focus) { (void)a; printf("NATIVE_FOCUS %d\n", focus); fflush(stdout); }

static void window_created(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    printf("NATIVE_WINDOW_CREATED %dx%d\n", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    fflush(stdout);
    display = eglGetDisplay(0);
    int attributes[] = {0x3033, 4, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038};  // window, ES2, RGB888
    void *config = 0;
    int count = 0;
    if (!display || !eglInitialize(display, 0, 0) || !eglChooseConfig(display, attributes, &config, 1, &count)
            || count != 1 || !eglBindAPI(0x30a0)) {
        printf("NATIVE_EGL_FAILED config 0x%x\n", eglGetError());
        return;
    }
    int version[] = {0x3098, 2, 0x3038};
    surface = eglCreateWindowSurface(display, config, window, 0);
    context = eglCreateContext(display, config, 0, version);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("NATIVE_EGL_FAILED surface 0x%x\n", eglGetError());
        surface = 0;
    }
}

// A frame of the colour (64, 128, 192), read back before it is presented.
static void redraw(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    if (!surface) return;
    glViewport(0, 0, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000);
    unsigned char pixel[4] = {0};
    glReadPixels(1, 1, 1, 1, 0x1908, 0x1401, pixel);
    printf("NATIVE_REDRAW %d pixel=%d,%d,%d swap=%u\n", ++frames, pixel[0], pixel[1], pixel[2],
           eglSwapBuffers(display, surface));
    fflush(stdout);
}

static void window_destroyed(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    (void)window;
    if (display) {
        eglMakeCurrent(display, 0, 0, 0);
        if (surface) eglDestroySurface(display, surface);
        if (context) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    surface = context = display = 0;
    say("NATIVE_WINDOW_DESTROYED");
}

static int input(int fd, int events, void *data) {
    (void)fd;
    (void)events;
    AInputQueue *queue = data;
    AInputEvent *event = 0;
    while (AInputQueue_getEvent(queue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(queue, event)) continue;
        int handled = 1;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
            int code = AKeyEvent_getKeyCode(event);
            printf("NATIVE_KEY %d %d%s\n", code, AKeyEvent_getAction(event),
                   AKeyEvent_getFlags(event) & AKEY_EVENT_FLAG_FALLBACK ? " fallback" : "");
            handled = code == AKEYCODE_A || code == AKEYCODE_BACK;
        } else if (AInputEvent_getSource(event) & AINPUT_SOURCE_CLASS_JOYSTICK) {
            printf("NATIVE_JOYSTICK %.0f\n", AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0));
            handled = 0;
        } else {
            printf("NATIVE_TOUCH %d %.0f,%.0f\n", AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK,
                   AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));
        }
        fflush(stdout);
        AInputQueue_finishEvent(queue, event, handled);
    }
    return 1;
}

static void queue_created(ANativeActivity *a, AInputQueue *queue) {
    (void)a;
    AInputQueue_attachLooper(queue, ALooper_forThread(), 1, input, queue);
    say("NATIVE_INPUT_QUEUE_CREATED");
}

static void queue_destroyed(ANativeActivity *a, AInputQueue *queue) {
    (void)a;
    AInputQueue_detachLooper(queue);
    say("NATIVE_INPUT_QUEUE_DESTROYED");
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t size) {
    (void)saved;
    (void)size;
    // The managed path: Java exists, unlike the standalone native-only launcher.
    printf("NATIVE_ON_CREATE java=%d\n", a->vm != 0 && a->env != 0 && a->clazz != 0);
    fflush(stdout);
    a->callbacks->onStart = started;
    a->callbacks->onResume = resumed;
    a->callbacks->onPause = paused;
    a->callbacks->onStop = stopped;
    a->callbacks->onDestroy = destroyed;
    a->callbacks->onWindowFocusChanged = focused;
    a->callbacks->onNativeWindowCreated = window_created;
    a->callbacks->onNativeWindowRedrawNeeded = redraw;
    a->callbacks->onNativeWindowDestroyed = window_destroyed;
    a->callbacks->onInputQueueCreated = queue_created;
    a->callbacks->onInputQueueDestroyed = queue_destroyed;
}
