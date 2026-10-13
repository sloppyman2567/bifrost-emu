// OpenSL ES PCM buffer-queue playback through the managed libOpenSLES adapter
// (Guest/Android/Adapters/opensles.c) over the managed libaaudio adapter. Built
// against the pinned AOSP wilhelm headers; result codes follow wilhelm.
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

enum { FRAMES = 480, BUFFERS = 2 };

static int failures;
static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct player {
    SLObjectItf object;
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf queue;
    int16_t pcm[3][FRAMES * 2];
    atomic_int calls, refill, destroy_on, destroyed, events, callback_tid, errno_kept, caller_ok;
};

static void on_buffer(SLAndroidSimpleBufferQueueItf queue, void *context) {
    struct player *p = context;
    errno = 4321; // Bionic TLS of the calling thread
    atomic_store(&p->errno_kept, errno == 4321);
    atomic_store(&p->caller_ok, queue == p->queue);
    atomic_store(&p->callback_tid, gettid());
    int calls = atomic_fetch_add(&p->calls, 1) + 1;
    if (atomic_load(&p->destroy_on) == calls) {
        (*p->object)->Destroy(p->object); // freed after this callback returns
        atomic_store(&p->destroyed, 1);
        return;
    }
    if (atomic_load(&p->refill))
        (*queue)->Enqueue(queue, p->pcm[calls % 3], sizeof(p->pcm[0]));
}

static void on_play(SLPlayItf play, void *context, SLuint32 event) {
    struct player *p = context;
    if (play == p->play)
        atomic_fetch_or(&p->events, (int)event);
}

static int wait_for(atomic_int *value, int target) {
    for (int i = 0; i < 400 && atomic_load(value) < target; ++i)
        usleep(5000);
    return atomic_load(value) >= target;
}

static int wait_event(struct player *p, int event) {
    for (int i = 0; i < 400 && !(atomic_load(&p->events) & event); ++i)
        usleep(5000);
    return (atomic_load(&p->events) & event) != 0;
}

static SLDataFormat_PCM stereo16(void) {
    return (SLDataFormat_PCM){SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_48, SL_PCMSAMPLEFORMAT_FIXED_16,
                              SL_PCMSAMPLEFORMAT_FIXED_16,
                              SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                              SL_BYTEORDER_LITTLEENDIAN};
}

static SLresult create(SLEngineItf engine, SLObjectItf mix, void *format, SLuint32 locator,
                       SLuint32 count, const SLInterfaceID *ids, SLObjectItf *out) {
    SLDataLocator_AndroidSimpleBufferQueue queue = {locator, BUFFERS};
    SLDataSource source = {&queue, format};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&output, NULL};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    return (*engine)->CreateAudioPlayer(engine, out, &source, &sink, count, ids, required);
}

static int realize_player(struct player *p, SLEngineItf engine, SLObjectItf mix, void *format) {
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    if (create(engine, mix, format, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1, ids, &p->object) ||
        (*p->object)->Realize(p->object, SL_BOOLEAN_FALSE) ||
        (*p->object)->GetInterface(p->object, SL_IID_PLAY, &p->play) ||
        (*p->object)->GetInterface(p->object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &p->queue) ||
        (*p->queue)->RegisterCallback(p->queue, on_buffer, p))
        return 0;
    return 1;
}

int main(void) {
    // ── Engine: Android shares one engine object per process ──
    SLObjectItf engine_object = NULL, second = NULL;
    check(slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS,
          "slCreateEngine");
    check(slCreateEngine(&second, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS &&
          second == engine_object, "second slCreateEngine shares the engine");
    SLEngineItf engine = NULL;
    check((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) ==
          SL_RESULT_PRECONDITIONS_VIOLATED && !engine, "engine interface needs Realize");
    check((*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS &&
          (*second)->Realize(second, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS,
          "shared engine realizes twice");
    check((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) ==
          SL_RESULT_SUCCESS && engine, "engine interface");
    void *unsupported = &engine;
    check((*engine_object)->GetInterface(engine_object, SL_IID_RECORD, &unsupported) ==
          SL_RESULT_FEATURE_UNSUPPORTED && !unsupported, "unsupported interface is NULL");
    if (!engine)
        return 1;

    SLObjectItf mix = NULL;
    const SLInterfaceID reverb[] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean optional[] = {SL_BOOLEAN_FALSE};
    check((*engine)->CreateOutputMix(engine, &mix, 1, reverb, optional) == SL_RESULT_SUCCESS &&
          (*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS,
          "output mix ignores an optional unsupported interface");

    // ── CreateAudioPlayer validation (wilhelm checkDataFormat/checkSourceSink) ──
    SLObjectItf rejected = NULL;
    const SLInterfaceID seek[] = {SL_IID_SEEK};
    SLDataFormat_PCM pcm = stereo16();
    check(create(engine, mix, &pcm, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1, seek, &rejected) ==
          SL_RESULT_FEATURE_UNSUPPORTED && !rejected, "required unsupported interface");
    pcm.endianness = SL_BYTEORDER_BIGENDIAN;
    check(create(engine, mix, &pcm, SL_DATALOCATOR_BUFFERQUEUE, 0, NULL, &rejected) ==
          SL_RESULT_CONTENT_UNSUPPORTED, "non-native byte order");
    pcm = stereo16();
    pcm.numChannels = 0;
    check(create(engine, mix, &pcm, SL_DATALOCATOR_BUFFERQUEUE, 0, NULL, &rejected) ==
          SL_RESULT_PARAMETER_INVALID, "zero channels");
    pcm = stereo16();
    pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_32;
    check(create(engine, mix, &pcm, SL_DATALOCATOR_BUFFERQUEUE, 0, NULL, &rejected) ==
          SL_RESULT_CONTENT_UNSUPPORTED, "container larger than sample");
    pcm = stereo16();
    pcm.channelMask = SL_SPEAKER_FRONT_LEFT;
    check(create(engine, mix, &pcm, SL_DATALOCATOR_BUFFERQUEUE, 0, NULL, &rejected) ==
          SL_RESULT_PARAMETER_INVALID, "channel mask disagrees with count");
    pcm = stereo16();
    SLDataLocator_URI uri = {SL_DATALOCATOR_URI, (SLchar *)"file:///sdcard/a.ogg"};
    SLDataFormat_MIME mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
    SLDataSource uri_source = {&uri, &mime};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&output, NULL};
    check((*engine)->CreateAudioPlayer(engine, &rejected, &uri_source, &sink, 0, NULL, NULL) ==
          SL_RESULT_CONTENT_UNSUPPORTED && !rejected, "decoding players are unsupported");

    // ── Stereo 16-bit player with explicit interfaces ──
    static struct player p;
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME,
                                 SL_IID_ANDROIDCONFIGURATION};
    check(create(engine, mix, &pcm, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 3, ids, &p.object) ==
          SL_RESULT_SUCCESS, "create PCM buffer-queue player");
    if (!p.object)
        return 1;
    SLAndroidConfigurationItf config = NULL;
    check((*p.object)->GetInterface(p.object, SL_IID_PLAY, &p.play) ==
          SL_RESULT_PRECONDITIONS_VIOLATED, "play interface needs Realize");
    check((*p.object)->GetInterface(p.object, SL_IID_ANDROIDCONFIGURATION, &config) ==
          SL_RESULT_SUCCESS, "configuration is available before Realize");
    SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;
    SLuint32 mode = SL_ANDROID_PERFORMANCE_NONE, size = 0;
    check((*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &stream_type,
                                      sizeof(stream_type)) == SL_RESULT_SUCCESS &&
          (*config)->SetConfiguration(config, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode,
                                      sizeof(mode)) == SL_RESULT_SUCCESS,
          "stream type and performance mode before Realize");
    check((*config)->SetConfiguration(config, (const SLchar *)"bogus", &mode, sizeof(mode)) ==
          SL_RESULT_PARAMETER_INVALID, "unknown configuration key");
    check((*config)->GetConfiguration(config, SL_ANDROID_KEY_PERFORMANCE_MODE, &size, NULL) ==
          SL_RESULT_SUCCESS && size == sizeof(SLuint32), "configuration size query");
    check((*p.object)->Realize(p.object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize player");
    check((*p.object)->Realize(p.object, SL_BOOLEAN_FALSE) == SL_RESULT_PRECONDITIONS_VIOLATED,
          "second Realize");
    check((*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &stream_type,
                                      sizeof(stream_type)) == SL_RESULT_PRECONDITIONS_VIOLATED,
          "configuration is fixed after Realize");
    SLBufferQueueItf standard = NULL;
    SLVolumeItf volume = NULL;
    SLEffectSendItf send = NULL;
    check((*p.object)->GetInterface(p.object, SL_IID_PLAY, &p.play) == SL_RESULT_SUCCESS &&
          (*p.object)->GetInterface(p.object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &p.queue) ==
          SL_RESULT_SUCCESS && (*p.object)->GetInterface(p.object, SL_IID_BUFFERQUEUE, &standard) ==
          SL_RESULT_SUCCESS && (void *)standard == (void *)p.queue,
          "both buffer-queue IIDs name one interface");
    check((*p.object)->GetInterface(p.object, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS,
          "requested volume interface");
    check((*p.object)->GetInterface(p.object, SL_IID_EFFECTSEND, &send) ==
          SL_RESULT_FEATURE_UNSUPPORTED, "unrequested explicit interface");
    SLmillibel max = 1;
    check((*volume)->SetVolumeLevel(volume, 1) == SL_RESULT_PARAMETER_INVALID &&
          (*volume)->SetVolumeLevel(volume, -600) == SL_RESULT_SUCCESS &&
          (*volume)->GetMaxVolumeLevel(volume, &max) == SL_RESULT_SUCCESS && max == 0,
          "volume range");
    check((*volume)->SetStereoPosition(volume, 1001) == SL_RESULT_PARAMETER_INVALID &&
          (*volume)->SetStereoPosition(volume, -300) == SL_RESULT_SUCCESS &&
          (*volume)->EnableStereoPosition(volume, SL_BOOLEAN_TRUE) == SL_RESULT_SUCCESS,
          "stereo position");

    for (int i = 0; i < FRAMES * 2; ++i)
        p.pcm[0][i] = p.pcm[1][i] = p.pcm[2][i] = (int16_t)((i & 64) ? 4000 : -4000);
    check((*p.queue)->RegisterCallback(p.queue, on_buffer, &p) == SL_RESULT_SUCCESS,
          "register buffer callback while stopped");
    check((*p.queue)->Enqueue(p.queue, NULL, 4) == SL_RESULT_PARAMETER_INVALID, "NULL buffer");
    check((*p.queue)->Enqueue(p.queue, p.pcm[0], sizeof(p.pcm[0])) == SL_RESULT_SUCCESS &&
          (*p.queue)->Enqueue(p.queue, p.pcm[1], sizeof(p.pcm[1])) == SL_RESULT_SUCCESS,
          "enqueue up to numBuffers");
    check((*p.queue)->Enqueue(p.queue, p.pcm[2], sizeof(p.pcm[2])) ==
          SL_RESULT_BUFFER_INSUFFICIENT, "full queue");
    check((*p.play)->RegisterCallback(p.play, on_play, &p) == SL_RESULT_SUCCESS &&
          (*p.play)->SetCallbackEventsMask(p.play, 0x100) == SL_RESULT_PARAMETER_INVALID &&
          (*p.play)->SetPositionUpdatePeriod(p.play, 100) == SL_RESULT_SUCCESS &&
          (*p.play)->SetCallbackEventsMask(p.play, SL_PLAYEVENT_HEADATEND |
                                           SL_PLAYEVENT_HEADATNEWPOS) == SL_RESULT_SUCCESS,
          "play events");
    usleep(50000);
    check(atomic_load(&p.calls) == 0, "no callback before PLAYING");

    atomic_store(&p.refill, 1);
    check((*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS, "play");
    check((*p.queue)->RegisterCallback(p.queue, on_buffer, &p) ==
          SL_RESULT_PRECONDITIONS_VIOLATED, "callback registration needs STOPPED");
    check(wait_for(&p.calls, 30), "buffer callbacks run while playing");
    check(atomic_load(&p.callback_tid) != gettid(), "callbacks run on their own thread");
    check(atomic_load(&p.errno_kept), "callback thread has Bionic TLS");
    check(atomic_load(&p.caller_ok), "callback receives its queue interface");
    SLAndroidSimpleBufferQueueState state;
    check((*p.queue)->GetState(p.queue, &state) == SL_RESULT_SUCCESS && state.index >= 30 &&
          state.count <= BUFFERS, "queue state advances");
    SLmillisecond position = 0, duration = 0;
    check((*p.play)->GetPosition(p.play, &position) == SL_RESULT_SUCCESS && position > 0 &&
          position <= (SLmillisecond)((state.index + 1) * FRAMES / 48),
          "position follows playback");
    check((*p.play)->GetDuration(p.play, &duration) == SL_RESULT_SUCCESS &&
          duration == SL_TIME_UNKNOWN, "a buffer queue has no duration");
    check(wait_event(&p, SL_PLAYEVENT_HEADATNEWPOS), "HEADATNEWPOS");

    atomic_store(&p.refill, 0); // the queue drains: HEADATEND, as wilhelm on underflow
    check(wait_event(&p, SL_PLAYEVENT_HEADATEND), "HEADATEND after the queue drains");
    check((*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PAUSED) == SL_RESULT_SUCCESS, "pause");
    int paused = atomic_load(&p.calls);
    (*p.queue)->Enqueue(p.queue, p.pcm[0], sizeof(p.pcm[0]));
    usleep(100000);
    check(atomic_load(&p.calls) == paused, "no callback while paused");
    check((*p.play)->SetPlayState(p.play, SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS &&
          (*p.play)->GetPosition(p.play, &position) == SL_RESULT_SUCCESS && position == 0,
          "stop rewinds the position");
    check((*p.queue)->Clear(p.queue) == SL_RESULT_SUCCESS &&
          (*p.queue)->GetState(p.queue, &state) == SL_RESULT_SUCCESS && !state.count &&
          !state.index, "clear empties the queue");

    atomic_store(&p.refill, 1);
    (*p.queue)->Enqueue(p.queue, p.pcm[0], sizeof(p.pcm[0]));
    int resumed = atomic_load(&p.calls);
    check((*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS &&
          wait_for(&p.calls, resumed + 5), "playback restarts after stop");
    (*p.object)->Destroy(p.object);
    int closed = atomic_load(&p.calls);
    usleep(100000);
    check(atomic_load(&p.calls) == closed, "no callback after Destroy");

    // ── Destroy from inside the buffer callback ──
    static struct player q;
    SLDataFormat_PCM pcm2 = stereo16();
    check(realize_player(&q, engine, mix, &pcm2), "second player");
    atomic_store(&q.refill, 1);
    atomic_store(&q.destroy_on, 3);
    (*q.queue)->Enqueue(q.queue, q.pcm[0], sizeof(q.pcm[0]));
    (*q.play)->SetPlayState(q.play, SL_PLAYSTATE_PLAYING);
    check(wait_for(&q.destroyed, 1), "player destroyed from its callback");
    usleep(100000);
    check(atomic_load(&q.calls) == 3, "no callback after self-destroy");

    // ── Mono float PCM_EX player plays on a stereo stream ──
    static struct player m;
    SLAndroidDataFormat_PCM_EX mono = {SL_ANDROID_DATAFORMAT_PCM_EX, 1, SL_SAMPLINGRATE_44_1, 32, 32,
                                       SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN,
                                       SL_ANDROID_PCM_REPRESENTATION_FLOAT};
    check(realize_player(&m, engine, mix, &mono), "mono float player");
    atomic_store(&m.refill, 1);
    (*m.queue)->Enqueue(m.queue, m.pcm[0], sizeof(m.pcm[0]));
    (*m.play)->SetPlayState(m.play, SL_PLAYSTATE_PLAYING);
    check(wait_for(&m.calls, 5), "mono float callbacks");
    (*m.object)->Destroy(m.object);

    (*mix)->Destroy(mix);
    (*second)->Destroy(second);
    SLuint32 count = 0;
    check(slQueryNumSupportedEngineInterfaces(&count) == SL_RESULT_SUCCESS && count >= 2,
          "engine interface query");
    check((*engine)->CreateOutputMix(engine, &mix, 0, NULL, NULL) == SL_RESULT_SUCCESS,
          "engine survives one of two Destroy calls");
    (*mix)->Destroy(mix);
    (*engine_object)->Destroy(engine_object);

    if (failures)
        return 1;
    printf("android_opensles_player: ALL PASS\n");
    return 0;
}
