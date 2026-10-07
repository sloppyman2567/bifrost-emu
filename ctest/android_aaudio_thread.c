// AAudio data callbacks through the managed libaaudio adapter run on its own
// Bionic thread (guest/android/aaudio.c), as real libaaudio's do. Freestanding
// declarations, like android_bionic_bootstrap.c; constants follow Android 15
// <aaudio/AAudio.h>.
typedef unsigned long pthread_t;
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int (*data_callback)(AAudioStream*, void*, void*, int);
extern int printf(const char*, ...);
extern int gettid(void);
extern int* __errno(void);
extern int usleep(unsigned);
extern int AAudio_createStreamBuilder(AAudioStreamBuilder**);
extern void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, data_callback, void*);
extern int AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream**);
extern int AAudioStreamBuilder_delete(AAudioStreamBuilder*);
extern int AAudioStream_requestStart(AAudioStream*);
extern int AAudioStream_waitForStateChange(AAudioStream*, int, int*, long);
extern int AAudioStream_close(AAudioStream*);
enum { FORMAT_FLOAT = 2, STATE_STARTED = 4, STATE_STOPPED = 10, FRAMES = 256 };

static volatile int calls, stop_after, last_frames, callback_tid, errno_kept, user_ok;
static int user_tag;

static int callback(AAudioStream* stream, void* user, void* audio, int frames) {
    (void)stream;
    float* out = audio;
    for (int i = 0; i < frames * 2; ++i) out[i] = (i & 32) ? 0.25f : -0.25f;
    *__errno() = 4321; // Bionic TLS of the calling thread
    errno_kept = *__errno() == 4321;
    user_ok = user == &user_tag;
    last_frames = frames;
    callback_tid = gettid();
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELEASE);
    return stop_after && calls >= stop_after; // AAUDIO_CALLBACK_RESULT_STOP
}

static int count(void) { return __atomic_load_n(&calls, __ATOMIC_ACQUIRE); }
static int wait_calls(int target) {
    for (int i = 0; i < 400 && count() < target; ++i) usleep(5000);
    return count() >= target;
}

static int failures;
static void check(int ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); ++failures; }
}

static AAudioStream* open_stream(data_callback cb) {
    AAudioStreamBuilder* builder = 0;
    AAudioStream* stream = 0;
    if (AAudio_createStreamBuilder(&builder) != 0) return 0;
    AAudioStreamBuilder_setFormat(builder, FORMAT_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, FRAMES);
    if (cb) AAudioStreamBuilder_setDataCallback(builder, cb, &user_tag);
    int result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    return result == 0 ? stream : 0;
}

int main(void) {
    AAudioStream* stream = open_stream(callback);
    check(stream != 0, "callback stream opens");
    if (!stream) return 1;
    usleep(100000);
    check(count() == 0, "no callback before requestStart");
    check(AAudioStream_requestStart(stream) == 0, "requestStart");
    check(wait_calls(5), "callbacks run after start");
    check(callback_tid != gettid(), "callback runs on its own thread");
    check(errno_kept, "callback thread has Bionic TLS");
    check(user_ok && last_frames == FRAMES, "callback receives user data and frame count");

    stop_after = count() + 3; // STOP ends callbacks and stops the stream
    int state = 0;
    for (int i = 0; i < 100 && state != STATE_STOPPED; ++i)
        AAudioStream_waitForStateChange(stream, STATE_STARTED, &state, 20000000L);
    check(state == STATE_STOPPED, "AAUDIO_CALLBACK_RESULT_STOP stops the stream");
    int stopped = count();
    usleep(100000);
    check(count() == stopped, "no callback after STOP");

    stop_after = 0;
    check(AAudioStream_requestStart(stream) == 0, "restart after STOP");
    check(wait_calls(stopped + 3), "callbacks resume after restart");
    check(AAudioStream_close(stream) == 0, "close joins the callback thread");
    int closed = count();
    usleep(100000);
    check(count() == closed, "no callback after close");

    AAudioStream* plain = open_stream(0); // write()-mode stream: no thread
    check(plain != 0 && AAudioStream_close(plain) == 0, "write-mode stream opens and closes");

    if (failures) return 1;
    printf("android_aaudio_thread: ALL PASS\n");
    return 0;
}
