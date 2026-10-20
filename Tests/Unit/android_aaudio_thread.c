// AAudio data callbacks through the managed libaaudio adapter run on its own
// Bionic thread (Guest/Android/Adapters/aaudio.c), as real libaaudio's do. Freestanding
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
extern void AAudioStreamBuilder_setUsage(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, data_callback, void*);
extern int AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream**);
extern int AAudioStreamBuilder_delete(AAudioStreamBuilder*);
extern int AAudioStream_requestStart(AAudioStream*);
extern int AAudioStream_waitForStateChange(AAudioStream*, int, int*, long);
extern int AAudioStream_close(AAudioStream*);
extern int AAudioStream_getUsage(AAudioStream*);
extern int AAudioStream_getInputPreset(AAudioStream*);
extern int AAudioStream_getTimestamp(AAudioStream*, int, long*, long*);
extern void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder*, unsigned);
extern void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setContentType(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setSpatializationBehavior(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setIsContentSpatialized(AAudioStreamBuilder*, _Bool);
extern void AAudioStreamBuilder_setAllowedCapturePolicy(AAudioStreamBuilder*, int);
extern void AAudioStreamBuilder_setPrivacySensitive(AAudioStreamBuilder*, _Bool);
extern void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder*, const char*);
extern void AAudioStreamBuilder_setAttributionTag(AAudioStreamBuilder*, const char*);
extern unsigned AAudioStream_getChannelMask(AAudioStream*);
extern int AAudioStream_getSamplesPerFrame(AAudioStream*);
extern int AAudioStream_getAllowedCapturePolicy(AAudioStream*);
extern int AAudioStream_getSpatializationBehavior(AAudioStream*);
extern _Bool AAudioStream_isContentSpatialized(AAudioStream*);
extern _Bool AAudioStream_isPrivacySensitive(AAudioStream*);
extern int AAudioStream_requestFlush(AAudioStream*);
extern int AAudioStream_requestStop(AAudioStream*);
extern int AAudioStream_release(AAudioStream*);
extern int AAudioStream_getState(AAudioStream*);
extern const char* AAudio_convertStreamStateToText(int);
extern const char* AAudio_convertResultToText(int);
extern int strcmp(const char*, const char*);
extern void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder*, int);
extern int AAudioStream_getBufferCapacityInFrames(AAudioStream*);
extern int AAudio_getMMapPolicy(void);
extern int AAudio_setMMapPolicy(int);
extern int AAudioStream_read(AAudioStream*, void*, int, long);
extern int AAudioStream_getContentType(AAudioStream*);
extern int AAudioStream_getHardwareChannelCount(AAudioStream*);
extern int AAudioStream_getHardwareFormat(AAudioStream*);
extern int AAudioStream_getHardwareSampleRate(AAudioStream*);
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
enum { FORMAT_FLOAT = 2, STATE_STARTED = 4, STATE_FLUSHED = 8, STATE_STOPPED = 10,
       STATE_CLOSING = 11, FRAMES = 256, INVALID_STATE = -895 };

// Every export of Android 15's libaaudio.map.txt (android-15.0.0_r1); engines such as
// FMOD resolve the table with dlsym when they pick an output.
static const char* const ndk_exports[] = {
    "AAudio_convertResultToText",
    "AAudio_convertStreamStateToText",
    "AAudio_createStreamBuilder",
    "AAudio_getMMapPolicy",
    "AAudio_setMMapPolicy",
    "AAudioStreamBuilder_delete",
    "AAudioStreamBuilder_openStream",
    "AAudioStreamBuilder_setAllowedCapturePolicy",
    "AAudioStreamBuilder_setAttributionTag",
    "AAudioStreamBuilder_setBufferCapacityInFrames",
    "AAudioStreamBuilder_setChannelCount",
    "AAudioStreamBuilder_setChannelMask",
    "AAudioStreamBuilder_setContentType",
    "AAudioStreamBuilder_setDataCallback",
    "AAudioStreamBuilder_setDeviceId",
    "AAudioStreamBuilder_setDirection",
    "AAudioStreamBuilder_setErrorCallback",
    "AAudioStreamBuilder_setFormat",
    "AAudioStreamBuilder_setFramesPerDataCallback",
    "AAudioStreamBuilder_setInputPreset",
    "AAudioStreamBuilder_setIsContentSpatialized",
    "AAudioStreamBuilder_setPackageName",
    "AAudioStreamBuilder_setPerformanceMode",
    "AAudioStreamBuilder_setPrivacySensitive",
    "AAudioStreamBuilder_setSampleRate",
    "AAudioStreamBuilder_setSamplesPerFrame",
    "AAudioStreamBuilder_setSessionId",
    "AAudioStreamBuilder_setSharingMode",
    "AAudioStreamBuilder_setSpatializationBehavior",
    "AAudioStreamBuilder_setUsage",
    "AAudioStream_close",
    "AAudioStream_getAllowedCapturePolicy",
    "AAudioStream_getBufferCapacityInFrames",
    "AAudioStream_getBufferSizeInFrames",
    "AAudioStream_getChannelCount",
    "AAudioStream_getChannelMask",
    "AAudioStream_getContentType",
    "AAudioStream_getDeviceId",
    "AAudioStream_getDirection",
    "AAudioStream_getFormat",
    "AAudioStream_getFramesPerBurst",
    "AAudioStream_getFramesPerDataCallback",
    "AAudioStream_getFramesRead",
    "AAudioStream_getFramesWritten",
    "AAudioStream_getHardwareChannelCount",
    "AAudioStream_getHardwareFormat",
    "AAudioStream_getHardwareSampleRate",
    "AAudioStream_getInputPreset",
    "AAudioStream_getPerformanceMode",
    "AAudioStream_getSampleRate",
    "AAudioStream_getSamplesPerFrame",
    "AAudioStream_getSessionId",
    "AAudioStream_getSharingMode",
    "AAudioStream_getSpatializationBehavior",
    "AAudioStream_getState",
    "AAudioStream_getTimestamp",
    "AAudioStream_getUsage",
    "AAudioStream_getXRunCount",
    "AAudioStream_isContentSpatialized",
    "AAudioStream_isMMapUsed",
    "AAudioStream_isPrivacySensitive",
    "AAudioStream_read",
    "AAudioStream_release",
    "AAudioStream_requestFlush",
    "AAudioStream_requestPause",
    "AAudioStream_requestStart",
    "AAudioStream_requestStop",
    "AAudioStream_setBufferSizeInFrames",
    "AAudioStream_waitForStateChange",
    "AAudioStream_write"
};

static volatile int calls, stop_after, last_frames, callback_tid, errno_kept, user_ok;
static volatile int release_in_callback = 1, flush_in_callback = 1; // 1: not tried yet
static int user_tag;

static int callback(AAudioStream* stream, void* user, void* audio, int frames) {
    if (release_in_callback == 1) { // both are illegal from the data callback
        release_in_callback = AAudioStream_release(stream);
        flush_in_callback = AAudioStream_requestFlush(stream);
    }
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
    AAudioStreamBuilder_setUsage(builder, 14); // AAUDIO_USAGE_GAME
    AAudioStreamBuilder_setInputPreset(builder, 6); // VOICE_RECOGNITION
    if (cb) AAudioStreamBuilder_setDataCallback(builder, cb, &user_tag);
    int result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    return result == 0 ? stream : 0;
}

int main(void) {
    AAudioStream* stream = open_stream(callback);
    check(stream != 0, "callback stream opens");
    if (!stream) return 1;
    check(AAudioStream_getUsage(stream) == 14 && AAudioStream_getInputPreset(stream) == 6,
          "builder usage and input preset survive open");
    long position = 123, timestamp = 456;
    check(AAudioStream_getTimestamp(stream, 1, &position, &timestamp) == -895,
          "timestamp before start is INVALID_STATE");
    check(position == 123 && timestamp == 456, "failed timestamp preserves outputs");
    check(AAudioStream_getTimestamp(stream, 0, &position, &timestamp) == -898,
          "timestamp rejects unsupported clock");
    usleep(100000);
    check(count() == 0, "no callback before requestStart");
    check(AAudioStream_requestStart(stream) == 0, "requestStart");
    check(wait_calls(5), "callbacks run after start");
    check(AAudioStream_getTimestamp(stream, 1, &position, &timestamp) == -889
          && AAudioStream_getTimestamp(stream, 7, &position, &timestamp) == -889,
          "host backend reports presentation timestamps unavailable");
    check(position == 123 && timestamp == 456, "unavailable timestamp preserves outputs");
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
    check(release_in_callback == INVALID_STATE && flush_in_callback == INVALID_STATE,
          "release and flush are illegal from the data callback");

    // Channel masks follow AOSP: a count is a (hidden) index mask; layouts stay.
    AAudioStreamBuilder* builder = 0;
    AAudioStream* masked = 0;
    AAudio_createStreamBuilder(&builder);
    AAudioStreamBuilder_setChannelMask(builder, 0x3f); // AAUDIO_CHANNEL_5POINT1
    check(AAudioStreamBuilder_openStream(builder, &masked) == 0 &&
          AAudioStream_getChannelMask(masked) == 0x3f &&
          AAudioStream_getSamplesPerFrame(masked) == 6, "5.1 layout mask");
    AAudioStream_close(masked);
    AAudioStreamBuilder_setSamplesPerFrame(builder, 1);
    check(AAudioStreamBuilder_openStream(builder, &masked) == 0 &&
          AAudioStream_getChannelMask(masked) == 0 &&
          AAudioStream_getSamplesPerFrame(masked) == 1, "channel count hides its index mask");
    // Defaults from AudioStream::open, then the requested values.
    check(AAudioStream_getAllowedCapturePolicy(masked) == 1 &&
          AAudioStream_getSpatializationBehavior(masked) == 1 &&
          !AAudioStream_isContentSpatialized(masked) && !AAudioStream_isPrivacySensitive(masked),
          "stream attribute defaults");
    AAudioStream_close(masked);
    AAudioStreamBuilder_setChannelMask(builder, 0x5); // not an output layout
    check(AAudioStreamBuilder_openStream(builder, &masked) == -898, "invalid channel mask");
    AAudioStreamBuilder_setSamplesPerFrame(builder, 13); // above FCC_LIMIT
    check(AAudioStreamBuilder_openStream(builder, &masked) == -882, "channel count out of range");
    AAudioStreamBuilder_setChannelMask(builder, 0x3);
    AAudioStreamBuilder_setContentType(builder, 99);
    check(AAudioStreamBuilder_openStream(builder, &masked) == -898, "invalid content type");
    AAudioStreamBuilder_setContentType(builder, 3); // MOVIE
    AAudioStreamBuilder_setSpatializationBehavior(builder, 2);
    AAudioStreamBuilder_setIsContentSpatialized(builder, 1);
    AAudioStreamBuilder_setAllowedCapturePolicy(builder, 3);
    AAudioStreamBuilder_setPrivacySensitive(builder, 1);
    AAudioStreamBuilder_setPackageName(builder, "com.example");
    AAudioStreamBuilder_setAttributionTag(builder, 0);
    check(AAudioStreamBuilder_openStream(builder, &masked) == 0 &&
          AAudioStream_getAllowedCapturePolicy(masked) == 3 &&
          AAudioStream_getSpatializationBehavior(masked) == 2 &&
          AAudioStream_isContentSpatialized(masked) && AAudioStream_isPrivacySensitive(masked),
          "requested stream attributes");
    AAudioStream_close(masked);
    AAudioStreamBuilder_delete(builder);

    check(!strcmp(AAudio_convertStreamStateToText(STATE_FLUSHED), "AAUDIO_STREAM_STATE_FLUSHED") &&
          !strcmp(AAudio_convertResultToText(INVALID_STATE), "AAUDIO_ERROR_INVALID_STATE") &&
          !strcmp(AAudio_convertStreamStateToText(99), "Unrecognized"), "state and result text");

    // Flush only while inactive; release frees the device but keeps the handle.
    AAudioStream* life = open_stream(0);
    check(life && AAudioStream_requestStart(life) == 0 &&
          AAudioStream_requestFlush(life) == INVALID_STATE, "flush rejected while started");
    check(AAudioStream_requestStop(life) == 0 && AAudioStream_requestFlush(life) == 0 &&
          AAudioStream_getState(life) == STATE_FLUSHED, "flush after stop");
    check(AAudioStream_release(life) == 0 && AAudioStream_getState(life) == STATE_CLOSING &&
          AAudioStream_release(life) == 0, "release enters CLOSING once");
    check(AAudioStream_requestStart(life) == INVALID_STATE, "a released stream cannot start");
    check(AAudioStream_close(life) == 0, "close after release");

    // The whole NDK table resolves, then an output opens as FMOD's AAudio output does.
    void* lib = dlopen("libaaudio.so", 2 /* RTLD_NOW */);
    int missing = lib ? 0 : 1;
    for (unsigned i = 0; lib && i < sizeof(ndk_exports) / sizeof(ndk_exports[0]); ++i)
        if (!dlsym(lib, ndk_exports[i])) {
            printf("FAIL: libaaudio.so lacks %s\n", ndk_exports[i]);
            ++missing;
        }
    check(missing == 0, "libaaudio.so exports every Android 15 NDK symbol");

    AAudioStream* hw = open_stream(0);
    check(hw != 0, "output stream opens");
    if (hw) {
        int rate = AAudioStream_getHardwareSampleRate(hw);
        int channels = AAudioStream_getHardwareChannelCount(hw);
        int format = AAudioStream_getHardwareFormat(hw);
        check(rate >= 8000 && rate <= 384000 && channels >= 1 && channels <= 8 &&
              (format == 1 || format == FORMAT_FLOAT), "hardware configuration is the host device");
        check(AAudioStream_getContentType(hw) == 2, "default content type is MUSIC");
        char pcm[64] = {0};
        check(AAudioStream_read(hw, pcm, 8, 0) == -890, "read on an output stream is UNIMPLEMENTED");
        check(AAudioStream_read(hw, 0, 8, 0) == -886, "read without a buffer is NULL");
        check(AAudioStream_read(hw, pcm, -1, 0) == -898, "read of negative frames is ILLEGAL_ARGUMENT");
        check(AAudioStream_read(hw, pcm, 0, 0) == 0, "read of zero frames returns 0");
        check(AAudioStream_close(hw) == 0, "hardware stream closes");
    }
    // FMOD probes a stream, then reopens with twice its capacity (32768 frames, above
    // the default ring); AAudio sizes the buffer to the request rather than failing.
    AAudioStreamBuilder* sized = 0;
    AAudioStream* big = 0;
    AAudio_createStreamBuilder(&sized);
    AAudioStreamBuilder_setBufferCapacityInFrames(sized, 32768);
    check(AAudioStreamBuilder_openStream(sized, &big) == 0 &&
          AAudioStream_getBufferCapacityInFrames(big) >= 32768,
          "buffer capacity request above the default ring is honored");
    if (big) AAudioStream_close(big);
    big = 0;
    AAudioStreamBuilder_setBufferCapacityInFrames(sized, 1 << 20);
    check(AAudioStreamBuilder_openStream(sized, &big) == -889, "absurd capacity is UNAVAILABLE");
    AAudioStreamBuilder_delete(sized);
    AAudioStreamBuilder* typed = 0;
    AAudioStream* movie = 0;
    AAudio_createStreamBuilder(&typed);
    AAudioStreamBuilder_setContentType(typed, 3); // MOVIE
    check(AAudioStreamBuilder_openStream(typed, &movie) == 0 &&
          AAudioStream_getContentType(movie) == 3, "requested content type");
    AAudioStream_close(movie);
    AAudioStreamBuilder_delete(typed);

    // AudioGlobal_setMMapPolicy: UNSPECIFIED until set; only the four values.
    check(AAudio_getMMapPolicy() == 0, "MMAP policy starts UNSPECIFIED");
    check(AAudio_setMMapPolicy(1) == 0 && AAudio_getMMapPolicy() == 1, "MMAP policy NEVER");
    check(AAudio_setMMapPolicy(4) == -898 && AAudio_getMMapPolicy() == 1,
          "invalid MMAP policy is rejected and keeps the old one");
    check(AAudio_setMMapPolicy(0) == 0 && AAudio_getMMapPolicy() == 0, "MMAP policy reset");

    if (failures) return 1;
    printf("android_aaudio_thread: ALL PASS\n");
    return 0;
}
