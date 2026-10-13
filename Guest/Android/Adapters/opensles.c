#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// Managed libOpenSLES: the OpenSL ES 1.0.1 playback subset apps use (engine,
// output mix and PCM buffer-queue audio players). The image's real library
// plays through AudioTrack and AudioFlinger, which Bifrost does not provide.
// Like AOSP frameworks/wilhelm (android-15.0.0_r1) over an AudioTrack pull
// callback, a player renders its queue from the data callback of a managed
// AAudio stream; the libaaudio adapter (aaudio.c) runs that callback on its own
// Bionic thread, so buffer-queue and play callbacks keep Bionic TLS. Result
// codes, validation and Android's shared engine follow wilhelm. Decoding
// sources (URI, fd, Android buffer queue) and recording are unsupported.

// Android 15 <aaudio/AAudio.h>, which the pinned headers do not ship.
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t (*AAudioStream_dataCallback)(AAudioStream *stream, void *user,
                                             void *audio, int32_t frames);
int32_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *builder, int32_t format);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *builder, int32_t channels);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *builder, int32_t rate);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *builder, int32_t usage);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *builder, int32_t mode);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *builder,
                                         AAudioStream_dataCallback callback, void *user);
int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder, AAudioStream **stream);
int32_t AAudioStreamBuilder_delete(AAudioStreamBuilder *builder);
int32_t AAudioStream_requestStart(AAudioStream *stream);
int32_t AAudioStream_requestPause(AAudioStream *stream);
int32_t AAudioStream_requestStop(AAudioStream *stream);
int32_t AAudioStream_close(AAudioStream *stream);
int64_t AAudioStream_getFramesRead(AAudioStream *stream);
int64_t AAudioStream_getFramesWritten(AAudioStream *stream);
enum { AAUDIO_FORMAT_PCM_FLOAT = 2, AAUDIO_USAGE_MEDIA = 1, AAUDIO_PERFORMANCE_MODE_NONE = 10,
       AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };

// ── Interface IDs: OpenSLES_IID.cpp GUIDs for every libOpenSLES.map.txt export ──
#define IID(name, a, b, c, d, n0, n1, n2, n3, n4, n5)                                  \
    static const struct SLInterfaceID_ guid_##name = {a, b, c, d, {n0, n1, n2, n3, n4, n5}}; \
    const SLInterfaceID SL_IID_##name = &guid_##name;
#define IID_STD(name, a, b, c, d) IID(name, a, b, c, d, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID_STD(3DCOMMIT, 0x3564ad80, 0xdd0f, 0x11db, 0x9e19)
IID_STD(3DDOPPLER, 0xb45c9a80, 0xddd2, 0x11db, 0xb028)
IID_STD(3DGROUPING, 0xebe844e0, 0xddd2, 0x11db, 0xb510)
IID_STD(3DLOCATION, 0x2b878020, 0xddd3, 0x11db, 0x8a01)
IID_STD(3DMACROSCOPIC, 0x5089aec0, 0xddd3, 0x11db, 0x9ad3)
IID_STD(3DSOURCE, 0x70bc7b00, 0xddd3, 0x11db, 0xa873)
IID_STD(AUDIODECODERCAPABILITIES, 0x3fe5a3a0, 0xfcc6, 0x11db, 0x94ac)
IID(AUDIOENCODER, 0xd7d5af7a, 0x351c, 0x41a6, 0x94ec, 0x1a, 0xc9, 0x5c, 0x71, 0x82, 0x2c)
IID_STD(AUDIOENCODERCAPABILITIES, 0x0f52a340, 0xfcd1, 0x11db, 0xa993)
IID_STD(AUDIOIODEVICECAPABILITIES, 0xb2564dc0, 0xddd3, 0x11db, 0xbd62)
IID_STD(BASSBOOST, 0x0634f220, 0xddd4, 0x11db, 0xa0fc)
IID_STD(BUFFERQUEUE, 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99)
IID_STD(DEVICEVOLUME, 0xe1634760, 0xf3e2, 0x11db, 0x9ca9)
IID_STD(DYNAMICINTERFACEMANAGEMENT, 0x63936540, 0xf775, 0x11db, 0x9cc4)
IID_STD(DYNAMICSOURCE, 0xc55cc100, 0x038b, 0x11dc, 0xbb45)
IID_STD(EFFECTSEND, 0x56e7d200, 0xddd4, 0x11db, 0xaefb)
IID_STD(ENGINE, 0x8d97c260, 0xddd4, 0x11db, 0x958f)
IID_STD(ENGINECAPABILITIES, 0x8320d0a0, 0xddd5, 0x11db, 0xa1b1)
IID(ENVIRONMENTALREVERB, 0xc2e5d5f0, 0x94bd, 0x4763, 0x9cac, 0x4e, 0x23, 0x4d, 0x06, 0x83, 0x9e)
IID_STD(EQUALIZER, 0x0bed4300, 0xddd6, 0x11db, 0x8f34)
IID_STD(LED, 0x2cc1cd80, 0xddd6, 0x11db, 0x807e)
IID_STD(METADATAEXTRACTION, 0xaa5b1f80, 0xddd6, 0x11db, 0xac8e)
IID_STD(METADATATRAVERSAL, 0xc43662c0, 0xddd6, 0x11db, 0xa7ab)
IID_STD(MIDIMESSAGE, 0xddf4a820, 0xddd6, 0x11db, 0xb174)
IID_STD(MIDIMUTESOLO, 0x039eaf80, 0xddd7, 0x11db, 0x9a02)
IID_STD(MIDITEMPO, 0x1f347400, 0xddd7, 0x11db, 0xa7ce)
IID_STD(MIDITIME, 0x3da51de0, 0xddd7, 0x11db, 0xaf70)
IID_STD(MUTESOLO, 0x5a28ebe0, 0xddd7, 0x11db, 0x8220)
IID(NULL, 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10)
IID_STD(OBJECT, 0x79216360, 0xddd7, 0x11db, 0xac16)
IID_STD(OUTPUTMIX, 0x97750f60, 0xddd7, 0x11db, 0x92b1)
IID_STD(PITCH, 0xc7e8ee00, 0xddd7, 0x11db, 0xa42c)
IID_STD(PLAY, 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49)
IID_STD(PLAYBACKRATE, 0x2e3b2a40, 0xddda, 0x11db, 0xa349)
IID_STD(PREFETCHSTATUS, 0x2a41ee80, 0xddd8, 0x11db, 0xa41f)
IID_STD(PRESETREVERB, 0x47382d60, 0xddd8, 0x11db, 0xbf3a)
IID_STD(RATEPITCH, 0x61b62e60, 0xddda, 0x11db, 0x9eb8)
IID_STD(RECORD, 0xc5657aa0, 0xdddb, 0x11db, 0x82f7)
IID_STD(SEEK, 0xd43135a0, 0xdddc, 0x11db, 0xb458)
IID_STD(THREADSYNC, 0xf6ac6b40, 0xdddc, 0x11db, 0xa62e)
IID_STD(VIBRA, 0x169a8d60, 0xdddd, 0x11db, 0x923d)
IID_STD(VIRTUALIZER, 0x37cc2c00, 0xdddd, 0x11db, 0x8577)
IID_STD(VISUALIZATION, 0xe46b26a0, 0xdddd, 0x11db, 0x8afd)
IID_STD(VOLUME, 0x09e8ede0, 0xddde, 0x11db, 0xb4f6)
IID_STD(ANDROIDEFFECT, 0xae12da60, 0x99ac, 0x11df, 0xb456)
IID_STD(ANDROIDEFFECTCAPABILITIES, 0x6a4f6d60, 0xb5e6, 0x11df, 0xbb3b)
IID_STD(ANDROIDEFFECTSEND, 0x7be462c0, 0xbc43, 0x11df, 0x8670)
IID_STD(ANDROIDCONFIGURATION, 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c)
IID_STD(ANDROIDSIMPLEBUFFERQUEUE, 0x198e4940, 0xc5d7, 0x11df, 0xa2a6)
IID_STD(ANDROIDBUFFERQUEUESOURCE, 0x7fc1a460, 0xeec1, 0x11df, 0xa306)
IID_STD(ANDROIDACOUSTICECHOCANCELLATION, 0x7b491460, 0x8d4d, 0x11e0, 0xbd61)
IID_STD(ANDROIDAUTOMATICGAINCONTROL, 0x0a8abfe0, 0x654c, 0x11e0, 0xba26)
IID_STD(ANDROIDNOISESUPPRESSION, 0x58b4b260, 0x8e06, 0x11e0, 0xaa8e)
#undef IID_STD
#undef IID

// ── Objects ─────────────────────────────────────────────────────────────────
// An SL interface handle points at a vtable pointer; each interface embeds
// that pointer first, followed by its owning object.
struct object;
struct itf {
    const void *vtable;
    struct object *owner;
};
#define OWNER(self) (((struct itf *)(uintptr_t)(self))->owner)

enum kind { KIND_ENGINE, KIND_OUTPUT_MIX, KIND_PLAYER };
// Explicit player interfaces: exposed only when requested at creation.
enum { EXPOSE_QUEUE = 1, EXPOSE_VOLUME = 2, EXPOSE_CONFIGURATION = 4, EXPOSE_EFFECT_SEND = 8 };
enum { MAX_CHANNELS = 8 }; // FCC_8, and the host AAudio stream limit

struct buffer {
    const uint8_t *data;
    SLuint32 size;
};

struct object {
    struct itf object, main, play, queue, volume, config, effect_send;
    enum kind kind;
    pthread_mutex_t lock;
    SLuint32 state; // SL_OBJECT_STATE_*
    unsigned exposed;
    slObjectCallback callback;
    const void *context;
    SLint32 priority;
    SLboolean preemptable;
    // PCM source
    SLuint32 channels, rate, container_bits, representation, frame_bytes;
    // Buffer queue (ring of app-owned buffers)
    struct buffer *buffers;
    SLuint32 capacity, front, count, play_index, consumed;
    slBufferQueueCallback queue_callback;
    void *queue_context;
    // Play
    SLuint32 play_state, event_mask;
    slPlayCallback play_callback;
    void *play_context;
    SLmillisecond marker, update_period;
    uint64_t played, next_update, reported; // frames since the last stop
    bool end_reported;
    // Volume and effect send
    SLmillibel level, direct_level;
    SLboolean mute, stereo;
    SLpermille stereo_position;
    // Android configuration
    SLint32 stream_type;
    SLuint32 performance;
    AAudioStream *stream;
    pthread_t render_thread;
    bool rendering, destroy_pending;
};

static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static struct object *engine; // Android shares one engine per process
static unsigned engine_refs;

static bool same_iid(SLInterfaceID a, const struct SLInterfaceID_ *b) {
    return a && !memcmp(a, b, sizeof(*b));
}

// ── Interface tables ────────────────────────────────────────────────────────
struct interface_row {
    const struct SLInterfaceID_ *iid;
    size_t offset;
    unsigned expose;  // 0: implicit
    bool prerealize;  // available before Realize
};
static const struct interface_row engine_rows[] = {
    {&guid_OBJECT, offsetof(struct object, object), 0, true},
    {&guid_ENGINE, offsetof(struct object, main), 0, false},
};
static const struct interface_row mix_rows[] = {
    {&guid_OBJECT, offsetof(struct object, object), 0, true},
    {&guid_OUTPUTMIX, offsetof(struct object, main), 0, false},
};
static const struct interface_row player_rows[] = {
    {&guid_OBJECT, offsetof(struct object, object), 0, true},
    {&guid_PLAY, offsetof(struct object, play), 0, false},
    // Android serves both buffer-queue IIDs from one interface.
    {&guid_BUFFERQUEUE, offsetof(struct object, queue), EXPOSE_QUEUE, false},
    {&guid_ANDROIDSIMPLEBUFFERQUEUE, offsetof(struct object, queue), EXPOSE_QUEUE, false},
    {&guid_VOLUME, offsetof(struct object, volume), EXPOSE_VOLUME, false},
    {&guid_EFFECTSEND, offsetof(struct object, effect_send), EXPOSE_EFFECT_SEND, false},
    {&guid_ANDROIDCONFIGURATION, offsetof(struct object, config), EXPOSE_CONFIGURATION, true},
};

static const struct interface_row *rows_for(enum kind kind, size_t *count) {
    switch (kind) {
    case KIND_ENGINE: *count = sizeof(engine_rows) / sizeof(engine_rows[0]); return engine_rows;
    case KIND_OUTPUT_MIX: *count = sizeof(mix_rows) / sizeof(mix_rows[0]); return mix_rows;
    default: *count = sizeof(player_rows) / sizeof(player_rows[0]); return player_rows;
    }
}

static const struct interface_row *find_row(enum kind kind, SLInterfaceID iid) {
    size_t count;
    const struct interface_row *rows = rows_for(kind, &count);
    for (size_t i = 0; i < count; ++i)
        if (same_iid(iid, rows[i].iid))
            return &rows[i];
    return NULL;
}

// wilhelm checkInterfaces: unknown optional IDs are ignored, required ones fail.
static SLresult check_interfaces(enum kind kind, SLuint32 count, const SLInterfaceID *ids,
                                 const SLboolean *required, unsigned *exposed) {
    *exposed = 0;
    if (count && (!ids || !required))
        return SL_RESULT_PARAMETER_INVALID;
    for (SLuint32 i = 0; i < count; ++i) {
        if (!ids[i])
            return SL_RESULT_PARAMETER_INVALID;
        const struct interface_row *row = find_row(kind, ids[i]);
        if (row)
            *exposed |= row->expose;
        else if (required[i])
            return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_vtable;
static const struct SLEngineItf_ engine_vtable;
static const struct SLOutputMixItf_ mix_vtable;
static const struct SLPlayItf_ play_vtable;
static const struct SLBufferQueueItf_ queue_vtable;
static const struct SLVolumeItf_ volume_vtable;
static const struct SLEffectSendItf_ effect_send_vtable;
static const struct SLAndroidConfigurationItf_ config_vtable;

static struct object *new_object(enum kind kind, unsigned exposed) {
    struct object *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    struct itf *itfs[] = {&o->object, &o->main, &o->play, &o->queue, &o->volume, &o->config,
                          &o->effect_send};
    const void *vtables[] = {&object_vtable, NULL, &play_vtable, &queue_vtable, &volume_vtable,
                             &config_vtable, &effect_send_vtable};
    vtables[1] = kind == KIND_ENGINE ? (const void *)&engine_vtable : (const void *)&mix_vtable;
    for (size_t i = 0; i < sizeof(itfs) / sizeof(itfs[0]); ++i) {
        itfs[i]->vtable = vtables[i];
        itfs[i]->owner = o;
    }
    pthread_mutex_init(&o->lock, NULL);
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->exposed = exposed;
    o->play_state = SL_PLAYSTATE_STOPPED;
    o->marker = SL_TIME_UNKNOWN;
    o->update_period = 1000; // per spec
    o->stream_type = SL_ANDROID_STREAM_MEDIA;
    o->performance = SL_ANDROID_PERFORMANCE_LATENCY; // wilhelm's default
    return o;
}

static void free_object(struct object *o) {
    pthread_mutex_destroy(&o->lock);
    free(o->buffers);
    free(o);
}

// ── Rendering (AAudio data callback, on the adapter's Bionic thread) ─────────
static float millibel_gain(SLmillibel level) {
    return powf(10.0f, (float)level / 2000.0f);
}

// android_player_volumeUpdate: level and direct level, then stereo position
// (mono pans with sin/cos, other layouts balance). gains[2] is the unpanned
// gain applied to channels beyond the front pair.
static void player_gains(const struct object *o, float gains[3]) {
    float base = o->mute ? 0.0f : millibel_gain(o->level) * millibel_gain(o->direct_level);
    gains[0] = gains[1] = gains[2] = base;
    if (!o->stereo)
        return;
    if (o->channels == 1) {
        double theta = (1000 + o->stereo_position) * M_PI_4 / 1000.0;
        gains[0] *= (float)cos(theta);
        gains[1] *= (float)sin(theta);
    } else if (o->stereo_position > 0) {
        gains[0] *= (1000 - o->stereo_position) / 1000.0f;
    } else {
        gains[1] *= (1000 + o->stereo_position) / 1000.0f;
    }
}

static float read_sample(const struct object *o, const uint8_t *p) {
    switch (o->container_bits) {
    case 8:
        return ((int)p[0] - 128) / 128.0f;
    case 16: {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        return v / 32768.0f;
    }
    case 24:
        return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) /
               2147483648.0f;
    default:
        if (o->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT) {
            float v;
            memcpy(&v, p, sizeof(v));
            return v;
        } else {
            int32_t v;
            memcpy(&v, p, sizeof(v));
            return v / 2147483648.0f;
        }
    }
}

// Mono sources play on a stereo stream so the stereo position can pan them.
static SLuint32 output_channels(const struct object *o) {
    return o->channels == 1 ? 2 : o->channels;
}

static void convert(const struct object *o, const uint8_t *src, float *dst, SLuint32 frames,
                    const float gains[3]) {
    const SLuint32 bytes = o->container_bits / 8;
    for (SLuint32 f = 0; f < frames; ++f) {
        if (o->channels == 1) {
            float s = read_sample(o, src);
            *dst++ = s * gains[0];
            *dst++ = s * gains[1];
            src += bytes;
            continue;
        }
        for (SLuint32 c = 0; c < o->channels; ++c, src += bytes)
            *dst++ = read_sample(o, src) * gains[c < 2 ? c : 2];
    }
}

static uint64_t ms_to_frames(const struct object *o, SLmillisecond ms) {
    return (uint64_t)ms * o->rate / 1000;
}

// Runs an app callback without the player lock, as wilhelm does. False means
// the callback destroyed the player: the caller unlocks and frees it.
static bool call_queue(struct object *o) {
    slBufferQueueCallback callback = o->queue_callback;
    void *context = o->queue_context;
    pthread_mutex_unlock(&o->lock);
    callback((SLBufferQueueItf)&o->queue.vtable, context);
    pthread_mutex_lock(&o->lock);
    return !o->destroy_pending;
}

static bool call_play(struct object *o, SLuint32 events) {
    static const SLuint32 order[] = {SL_PLAYEVENT_HEADATEND, SL_PLAYEVENT_HEADATMARKER,
                                     SL_PLAYEVENT_HEADATNEWPOS};
    events &= o->event_mask;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        slPlayCallback callback = o->play_callback;
        void *context = o->play_context;
        if (!(events & order[i]) || !callback)
            continue;
        // The spec requires one callback per event.
        pthread_mutex_unlock(&o->lock);
        callback((SLPlayItf)&o->play.vtable, context, order[i]);
        pthread_mutex_lock(&o->lock);
        if (o->destroy_pending)
            return false;
    }
    return true;
}

static SLuint32 advance(struct object *o, SLuint32 frames) {
    const uint64_t before = o->played;
    SLuint32 events = 0;
    o->played += frames;
    if (o->marker != SL_TIME_UNKNOWN) {
        const uint64_t marker = ms_to_frames(o, o->marker);
        if (before <= marker && marker < o->played)
            events |= SL_PLAYEVENT_HEADATMARKER;
    }
    if (o->played >= o->next_update) {
        uint64_t period = ms_to_frames(o, o->update_period);
        if (!period)
            period = 1;
        events |= SL_PLAYEVENT_HEADATNEWPOS;
        o->next_update = (o->played / period + 1) * period;
    }
    return events;
}

static int32_t render(AAudioStream *stream, void *user, void *audio, int32_t frames) {
    (void)stream;
    struct object *o = user;
    float *out = audio;
    SLuint32 done = 0, out_channels = output_channels(o);
    float gains[3];
    pthread_mutex_lock(&o->lock);
    o->rendering = true;
    o->render_thread = pthread_self();
    while (done < (SLuint32)frames && o->play_state == SL_PLAYSTATE_PLAYING) {
        SLuint32 events = 0;
        bool finished = false;
        if (!o->count) {
            // Underflow: wilhelm reports the end of content once per drain.
            if (o->end_reported)
                break;
            o->end_reported = true;
            if (!call_play(o, SL_PLAYEVENT_HEADATEND))
                goto destroyed;
            continue;
        }
        const struct buffer *b = &o->buffers[o->front];
        SLuint32 n = (b->size - o->consumed) / o->frame_bytes;
        if (n > (SLuint32)frames - done)
            n = (SLuint32)frames - done;
        player_gains(o, gains);
        convert(o, b->data + o->consumed, out + (size_t)done * out_channels, n, gains);
        o->consumed += n * o->frame_bytes;
        done += n;
        events = advance(o, n);
        if (b->size - o->consumed < o->frame_bytes) { // a trailing partial frame is dropped
            o->front = (o->front + 1) % o->capacity;
            --o->count;
            ++o->play_index;
            o->consumed = 0;
            finished = true;
        }
        if (events && !call_play(o, events))
            goto destroyed;
        if (finished && o->queue_callback && !call_queue(o))
            goto destroyed;
    }
    memset(out + (size_t)done * out_channels, 0,
           (size_t)((SLuint32)frames - done) * out_channels * sizeof(float));
    o->rendering = false;
    pthread_mutex_unlock(&o->lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
destroyed:
    // Destroy from this callback closed the stream; the thread ends after us.
    pthread_mutex_unlock(&o->lock);
    free_object(o);
    return AAUDIO_CALLBACK_RESULT_STOP;
}

// IAndroidConfiguration stream type → AAudio usage (audio_stream_type_t to
// audio_usage_t as AudioTrack maps them).
static int32_t stream_usage(SLint32 type) {
    switch (type) {
    case SL_ANDROID_STREAM_VOICE: return 2;        // VOICE_COMMUNICATION
    case SL_ANDROID_STREAM_SYSTEM: return 13;      // ASSISTANCE_SONIFICATION
    case SL_ANDROID_STREAM_RING: return 6;         // NOTIFICATION_RINGTONE
    case SL_ANDROID_STREAM_ALARM: return 4;        // ALARM
    case SL_ANDROID_STREAM_NOTIFICATION: return 5; // NOTIFICATION
    default: return AAUDIO_USAGE_MEDIA;
    }
}

static int32_t performance_mode(SLuint32 mode) {
    switch (mode) {
    case SL_ANDROID_PERFORMANCE_POWER_SAVING: return 11;
    case SL_ANDROID_PERFORMANCE_LATENCY:
    case SL_ANDROID_PERFORMANCE_LATENCY_EFFECTS: return 12;
    default: return AAUDIO_PERFORMANCE_MODE_NONE;
    }
}

static SLresult player_open(struct object *o) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != 0)
        return SL_RESULT_RESOURCE_ERROR;
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, (int32_t)output_channels(o));
    AAudioStreamBuilder_setSampleRate(builder, (int32_t)o->rate);
    AAudioStreamBuilder_setUsage(builder, stream_usage(o->stream_type));
    AAudioStreamBuilder_setPerformanceMode(builder, performance_mode(o->performance));
    AAudioStreamBuilder_setDataCallback(builder, render, o);
    int32_t result = AAudioStreamBuilder_openStream(builder, &o->stream);
    AAudioStreamBuilder_delete(builder);
    if (result != 0) {
        o->stream = NULL;
        return SL_RESULT_CONTENT_UNSUPPORTED; // as for an AudioTrack initCheck failure
    }
    return SL_RESULT_SUCCESS;
}

// ── SLObjectItf ─────────────────────────────────────────────────────────────
static SLresult object_realize(SLObjectItf self, SLboolean async) {
    struct object *o = OWNER(self);
    bool shared = false;
    if (o->kind == KIND_ENGINE) {
        pthread_mutex_lock(&engine_lock);
        shared = engine_refs > 1;
        pthread_mutex_unlock(&engine_lock);
    }
    pthread_mutex_lock(&o->lock);
    if (o->state != SL_OBJECT_STATE_UNREALIZED) {
        SLresult result = shared && o->state == SL_OBJECT_STATE_REALIZED
                              ? SL_RESULT_SUCCESS : SL_RESULT_PRECONDITIONS_VIOLATED;
        pthread_mutex_unlock(&o->lock);
        return result;
    }
    SLresult result = o->kind == KIND_PLAYER ? player_open(o) : SL_RESULT_SUCCESS;
    if (result == SL_RESULT_SUCCESS)
        o->state = SL_OBJECT_STATE_REALIZED;
    slObjectCallback callback = o->callback;
    const void *context = o->context;
    SLuint32 state = o->state;
    pthread_mutex_unlock(&o->lock);
    // Realize completes synchronously; an async request still gets its event.
    if (async && callback)
        callback(self, context, SL_OBJECT_EVENT_ASYNC_TERMINATION, result, state, NULL);
    return result;
}

static SLresult object_resume(SLObjectItf self, SLboolean async) {
    (void)self, (void)async;
    return SL_RESULT_PRECONDITIONS_VIOLATED; // objects are never suspended
}

static SLresult object_get_state(SLObjectItf self, SLuint32 *state) {
    if (!state)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *state = o->state;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult object_get_interface(SLObjectItf self, const SLInterfaceID iid, void *out) {
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *(void **)out = NULL;
    if (!iid)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    const struct interface_row *row = find_row(o->kind, iid);
    if (!row)
        return SL_RESULT_FEATURE_UNSUPPORTED;
    pthread_mutex_lock(&o->lock);
    SLresult result = SL_RESULT_SUCCESS;
    if (o->state != SL_OBJECT_STATE_REALIZED && !row->prerealize)
        result = SL_RESULT_PRECONDITIONS_VIOLATED;
    else if (row->expose && !(o->exposed & row->expose))
        result = SL_RESULT_FEATURE_UNSUPPORTED;
    else
        *(void **)out = (char *)o + row->offset;
    pthread_mutex_unlock(&o->lock);
    return result;
}

static SLresult object_register_callback(SLObjectItf self, slObjectCallback callback,
                                         void *context) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->callback = callback;
    o->context = context;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static void object_abort_async(SLObjectItf self) {
    (void)self; // Realize and Resume complete synchronously
}

static void object_destroy(SLObjectItf self) {
    struct object *o = OWNER(self);
    if (o->kind == KIND_ENGINE) {
        pthread_mutex_lock(&engine_lock);
        bool last = o == engine && --engine_refs == 0;
        if (last)
            engine = NULL;
        pthread_mutex_unlock(&engine_lock);
        if (last)
            free_object(o);
        return;
    }
    pthread_mutex_lock(&o->lock);
    AAudioStream *stream = o->stream;
    o->stream = NULL;
    o->play_state = SL_PLAYSTATE_STOPPED;
    const bool from_callback = o->rendering && pthread_equal(o->render_thread, pthread_self());
    o->destroy_pending = from_callback;
    pthread_mutex_unlock(&o->lock);
    // Waits for a running callback and ends its thread; from inside the
    // callback the close is deferred and render() frees the player.
    if (stream)
        AAudioStream_close(stream);
    if (!from_callback)
        free_object(o);
}

static SLresult object_set_priority(SLObjectItf self, SLint32 priority, SLboolean preemptable) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->priority = priority;
    o->preemptable = preemptable != SL_BOOLEAN_FALSE;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult object_get_priority(SLObjectItf self, SLint32 *priority,
                                    SLboolean *preemptable) {
    if (!priority || !preemptable)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *priority = o->priority;
    *preemptable = o->preemptable;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult object_set_loss_of_control(SLObjectItf self, SLint16 count,
                                           SLInterfaceID *ids, SLboolean enabled) {
    (void)self, (void)enabled;
    if (count < 0 || (count && !ids))
        return SL_RESULT_PARAMETER_INVALID;
    return SL_RESULT_SUCCESS; // there is no other client to lose control to
}

static const struct SLObjectItf_ object_vtable = {
    object_realize, object_resume, object_get_state, object_get_interface,
    object_register_callback, object_abort_async, object_destroy, object_set_priority,
    object_get_priority, object_set_loss_of_control,
};

// ── SLEngineItf ─────────────────────────────────────────────────────────────
static SLuint32 channel_count(SLuint32 mask) {
    return (SLuint32)__builtin_popcount(mask & ~SL_ANDROID_SPEAKER_NON_POSITIONAL);
}

// wilhelm checkDataFormat (PCM) plus android_audioPlayer_checkSourceSink.
static SLresult check_pcm(const SLDataFormat_PCM *pcm, SLuint32 representation,
                          bool has_representation) {
    if (!pcm->numChannels)
        return SL_RESULT_PARAMETER_INVALID;
    if (pcm->numChannels > MAX_CHANNELS)
        return SL_RESULT_CONTENT_UNSUPPORTED;
    if (!pcm->samplesPerSec)
        return SL_RESULT_PARAMETER_INVALID;
    if (pcm->samplesPerSec < SL_SAMPLINGRATE_8 || pcm->samplesPerSec > SL_SAMPLINGRATE_192)
        return SL_RESULT_CONTENT_UNSUPPORTED;
    switch (pcm->containerSize) {
    case 8:
        if (has_representation && representation != SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT)
            return SL_RESULT_PARAMETER_INVALID;
        break;
    case 16:
    case 24:
        if (has_representation && representation != SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT)
            return SL_RESULT_PARAMETER_INVALID;
        break;
    case 32:
        if (has_representation && representation != SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT &&
            representation != SL_ANDROID_PCM_REPRESENTATION_FLOAT)
            return SL_RESULT_PARAMETER_INVALID;
        break;
    default:
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (!pcm->bitsPerSample || pcm->containerSize < pcm->bitsPerSample)
        return SL_RESULT_PARAMETER_INVALID;
    if (pcm->channelMask && channel_count(pcm->channelMask) != pcm->numChannels)
        return SL_RESULT_PARAMETER_INVALID;
    if (pcm->endianness != SL_BYTEORDER_LITTLEENDIAN && pcm->endianness != SL_BYTEORDER_BIGENDIAN)
        return SL_RESULT_PARAMETER_INVALID;
    if (pcm->endianness != SL_BYTEORDER_LITTLEENDIAN) // not the native order
        return SL_RESULT_CONTENT_UNSUPPORTED;
    if (pcm->containerSize != pcm->bitsPerSample)
        return SL_RESULT_CONTENT_UNSUPPORTED;
    return SL_RESULT_SUCCESS;
}

static SLresult check_source(const SLDataSource *source, struct object *o) {
    if (!source || !source->pLocator || !source->pFormat)
        return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 locator = *(const SLuint32 *)source->pLocator;
    if (locator != SL_DATALOCATOR_BUFFERQUEUE && locator != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE)
        return SL_RESULT_CONTENT_UNSUPPORTED; // decoding players
    const SLuint32 buffers = ((const SLDataLocator_BufferQueue *)source->pLocator)->numBuffers;
    if (!buffers)
        return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 format = *(const SLuint32 *)source->pFormat;
    const SLDataFormat_PCM *pcm = source->pFormat;
    SLuint32 representation = 0;
    SLresult result;
    if (format == SL_ANDROID_DATAFORMAT_PCM_EX) {
        const SLAndroidDataFormat_PCM_EX *ex = source->pFormat;
        representation = ex->representation;
        result = check_pcm(pcm, representation, true);
    } else if (format == SL_DATAFORMAT_PCM) {
        representation = pcm->containerSize == 8 ? SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT
                                                 : SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
        result = check_pcm(pcm, 0, false);
    } else {
        return SL_RESULT_CONTENT_UNSUPPORTED; // buffer queues carry PCM only
    }
    if (result != SL_RESULT_SUCCESS)
        return result;
    o->buffers = calloc(buffers, sizeof(*o->buffers));
    if (!o->buffers)
        return SL_RESULT_MEMORY_FAILURE;
    o->capacity = buffers;
    o->channels = pcm->numChannels;
    o->rate = pcm->samplesPerSec / 1000;
    o->container_bits = pcm->containerSize;
    o->representation = representation;
    o->frame_bytes = pcm->numChannels * (pcm->containerSize / 8);
    return SL_RESULT_SUCCESS;
}

static SLresult check_sink(const SLDataSink *sink) {
    if (!sink || !sink->pLocator)
        return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 locator = *(const SLuint32 *)sink->pLocator;
    if (locator != SL_DATALOCATOR_OUTPUTMIX)
        return SL_RESULT_CONTENT_UNSUPPORTED; // decode to a buffer queue
    SLObjectItf mix = ((const SLDataLocator_OutputMix *)sink->pLocator)->outputMix;
    if (!mix || *mix != &object_vtable || OWNER(mix)->kind != KIND_OUTPUT_MIX)
        return SL_RESULT_PARAMETER_INVALID;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_create_audio_player(SLEngineItf self, SLObjectItf *out,
                                           SLDataSource *source, SLDataSink *sink,
                                           SLuint32 count, const SLInterfaceID *ids,
                                           const SLboolean *required) {
    (void)self;
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    unsigned exposed;
    SLresult result = check_interfaces(KIND_PLAYER, count, ids, required, &exposed);
    if (result != SL_RESULT_SUCCESS)
        return result;
    struct object *o = new_object(KIND_PLAYER, exposed);
    if (!o)
        return SL_RESULT_MEMORY_FAILURE;
    result = check_source(source, o);
    if (result == SL_RESULT_SUCCESS)
        result = check_sink(sink);
    if (result != SL_RESULT_SUCCESS) {
        free_object(o);
        return result;
    }
    o->next_update = ms_to_frames(o, o->update_period);
    *out = (SLObjectItf)&o->object.vtable;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_create_output_mix(SLEngineItf self, SLObjectItf *out, SLuint32 count,
                                         const SLInterfaceID *ids, const SLboolean *required) {
    (void)self;
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    unsigned exposed;
    SLresult result = check_interfaces(KIND_OUTPUT_MIX, count, ids, required, &exposed);
    if (result != SL_RESULT_SUCCESS)
        return result;
    struct object *o = new_object(KIND_OUTPUT_MIX, exposed);
    if (!o)
        return SL_RESULT_MEMORY_FAILURE;
    *out = (SLObjectItf)&o->object.vtable;
    return SL_RESULT_SUCCESS;
}

// LED, vibra, recorder, MIDI, 3D, metadata and extension objects.
static SLresult unsupported_object(SLObjectItf *out) {
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult engine_create_led(SLEngineItf self, SLObjectItf *out, SLuint32 device,
                                  SLuint32 count, const SLInterfaceID *ids,
                                  const SLboolean *required) {
    (void)self, (void)device, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_vibra(SLEngineItf self, SLObjectItf *out, SLuint32 device,
                                    SLuint32 count, const SLInterfaceID *ids,
                                    const SLboolean *required) {
    (void)self, (void)device, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_recorder(SLEngineItf self, SLObjectItf *out, SLDataSource *source,
                                       SLDataSink *sink, SLuint32 count, const SLInterfaceID *ids,
                                       const SLboolean *required) {
    (void)self, (void)source, (void)sink, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_midi(SLEngineItf self, SLObjectItf *out, SLDataSource *midi,
                                   SLDataSource *bank, SLDataSink *audio, SLDataSink *vibra,
                                   SLDataSink *led, SLuint32 count, const SLInterfaceID *ids,
                                   const SLboolean *required) {
    (void)self, (void)midi, (void)bank, (void)audio, (void)vibra, (void)led, (void)count,
        (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_listener(SLEngineItf self, SLObjectItf *out, SLuint32 count,
                                       const SLInterfaceID *ids, const SLboolean *required) {
    (void)self, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_3d_group(SLEngineItf self, SLObjectItf *out, SLuint32 count,
                                       const SLInterfaceID *ids, const SLboolean *required) {
    (void)self, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_metadata(SLEngineItf self, SLObjectItf *out, SLDataSource *source,
                                       SLuint32 count, const SLInterfaceID *ids,
                                       const SLboolean *required) {
    (void)self, (void)source, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static SLresult engine_create_extension(SLEngineItf self, SLObjectItf *out, void *parameters,
                                        SLuint32 object_id, SLuint32 count,
                                        const SLInterfaceID *ids, const SLboolean *required) {
    (void)self, (void)parameters, (void)object_id, (void)count, (void)ids, (void)required;
    return unsupported_object(out);
}

static const struct interface_row *rows_for_id(SLuint32 object_id, size_t *count) {
    switch (object_id) {
    case SL_OBJECTID_ENGINE: return rows_for(KIND_ENGINE, count);
    case SL_OBJECTID_OUTPUTMIX: return rows_for(KIND_OUTPUT_MIX, count);
    case SL_OBJECTID_AUDIOPLAYER: return rows_for(KIND_PLAYER, count);
    default: return NULL;
    }
}

static SLresult engine_query_num_interfaces(SLEngineItf self, SLuint32 object_id,
                                            SLuint32 *count) {
    (void)self;
    size_t n;
    if (!count)
        return SL_RESULT_PARAMETER_INVALID;
    if (!rows_for_id(object_id, &n))
        return SL_RESULT_FEATURE_UNSUPPORTED;
    *count = (SLuint32)n;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_query_interfaces(SLEngineItf self, SLuint32 object_id, SLuint32 index,
                                        SLInterfaceID *iid) {
    (void)self;
    size_t n;
    if (!iid)
        return SL_RESULT_PARAMETER_INVALID;
    *iid = NULL;
    const struct interface_row *rows = rows_for_id(object_id, &n);
    if (!rows)
        return SL_RESULT_FEATURE_UNSUPPORTED;
    if (index >= n)
        return SL_RESULT_PARAMETER_INVALID;
    *iid = rows[index].iid;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_query_num_extensions(SLEngineItf self, SLuint32 *count) {
    (void)self;
    if (!count)
        return SL_RESULT_PARAMETER_INVALID;
    *count = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_query_extension(SLEngineItf self, SLuint32 index, SLchar *name,
                                       SLint16 *length) {
    (void)self, (void)index, (void)name, (void)length;
    return SL_RESULT_PARAMETER_INVALID; // there are no extensions to index
}

static SLresult engine_is_extension_supported(SLEngineItf self, const SLchar *name,
                                              SLboolean *supported) {
    (void)self;
    if (!name || !supported)
        return SL_RESULT_PARAMETER_INVALID;
    *supported = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vtable = {
    engine_create_led, engine_create_vibra, engine_create_audio_player, engine_create_recorder,
    engine_create_midi, engine_create_listener, engine_create_3d_group, engine_create_output_mix,
    engine_create_metadata, engine_create_extension, engine_query_num_interfaces,
    engine_query_interfaces, engine_query_num_extensions, engine_query_extension,
    engine_is_extension_supported,
};

// ── SLOutputMixItf ──────────────────────────────────────────────────────────
static SLresult mix_get_devices(SLOutputMixItf self, SLint32 *count, SLuint32 *ids) {
    (void)self;
    if (!count)
        return SL_RESULT_PARAMETER_INVALID;
    if (ids) {
        if (*count < 1)
            return SL_RESULT_BUFFER_INSUFFICIENT;
        ids[0] = SL_DEFAULTDEVICEID_AUDIOOUTPUT;
    }
    *count = 1;
    return SL_RESULT_SUCCESS;
}

static SLresult mix_register_device_change(SLOutputMixItf self,
                                           slMixDeviceChangeCallback callback, void *context) {
    (void)self, (void)callback, (void)context;
    return SL_RESULT_SUCCESS; // the default device never changes
}

static SLresult mix_reroute(SLOutputMixItf self, SLint32 count, SLuint32 *ids) {
    (void)self;
    if (count <= 0 || !ids)
        return SL_RESULT_PARAMETER_INVALID;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLOutputMixItf_ mix_vtable = {
    mix_get_devices, mix_register_device_change, mix_reroute,
};

// ── SLPlayItf ───────────────────────────────────────────────────────────────
static SLresult play_set_state(SLPlayItf self, SLuint32 state) {
    if (state != SL_PLAYSTATE_STOPPED && state != SL_PLAYSTATE_PAUSED &&
        state != SL_PLAYSTATE_PLAYING)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    if (state != o->play_state) {
        o->play_state = state;
        if (state == SL_PLAYSTATE_PLAYING)
            o->end_reported = false;
        if (state == SL_PLAYSTATE_STOPPED) {
            // AudioTrack::stop resets the playback head; the queue is kept.
            o->played = o->reported = 0;
            o->next_update = ms_to_frames(o, o->update_period);
        }
        if (o->stream) {
            if (state == SL_PLAYSTATE_PLAYING)
                AAudioStream_requestStart(o->stream);
            else if (state == SL_PLAYSTATE_PAUSED)
                AAudioStream_requestPause(o->stream);
            else
                AAudioStream_requestStop(o->stream);
        }
    }
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(SLPlayItf self, SLuint32 *state) {
    if (!state)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *state = o->play_state;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(SLPlayItf self, SLmillisecond *duration) {
    (void)self;
    if (!duration)
        return SL_RESULT_PARAMETER_INVALID;
    *duration = SL_TIME_UNKNOWN; // a buffer queue has no duration
    return SL_RESULT_SUCCESS;
}

// AudioTrack::getPosition counts presented frames. Rendered frames still in
// the host stream's buffer have not been heard yet.
static SLresult play_get_position(SLPlayItf self, SLmillisecond *position) {
    if (!position)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    uint64_t frames = o->played;
    if (o->stream) {
        int64_t written = AAudioStream_getFramesWritten(o->stream);
        int64_t read = AAudioStream_getFramesRead(o->stream);
        uint64_t pending = written > read && read >= 0 ? (uint64_t)(written - read) : 0;
        frames = frames > pending ? frames - pending : 0;
    }
    if (frames < o->reported)
        frames = o->reported;
    o->reported = frames;
    *position = o->rate ? (SLmillisecond)(frames * 1000 / o->rate) : 0;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_register_callback(SLPlayItf self, slPlayCallback callback, void *context) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->play_callback = callback;
    o->play_context = context;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_set_events(SLPlayItf self, SLuint32 mask) {
    if (mask & ~(SL_PLAYEVENT_HEADATEND | SL_PLAYEVENT_HEADATMARKER | SL_PLAYEVENT_HEADATNEWPOS |
                 SL_PLAYEVENT_HEADMOVING | SL_PLAYEVENT_HEADSTALLED))
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    // Enabling position updates postpones the next one by a full period.
    if (!(o->event_mask & SL_PLAYEVENT_HEADATNEWPOS) && (mask & SL_PLAYEVENT_HEADATNEWPOS))
        o->next_update = o->played + ms_to_frames(o, o->update_period);
    o->event_mask = mask;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_events(SLPlayItf self, SLuint32 *mask) {
    if (!mask)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *mask = o->event_mask;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_set_marker(SLPlayItf self, SLmillisecond marker) {
    if (marker == SL_TIME_UNKNOWN)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->marker = marker;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_clear_marker(SLPlayItf self) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->marker = SL_TIME_UNKNOWN;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_marker(SLPlayItf self, SLmillisecond *marker) {
    if (!marker)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *marker = o->marker;
    pthread_mutex_unlock(&o->lock);
    return *marker == SL_TIME_UNKNOWN ? SL_RESULT_PRECONDITIONS_VIOLATED : SL_RESULT_SUCCESS;
}

static SLresult play_set_update_period(SLPlayItf self, SLmillisecond period) {
    if (!period)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    if (o->update_period != period) {
        o->update_period = period; // a new period postpones the next update
        o->next_update = o->played + ms_to_frames(o, period);
    }
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_update_period(SLPlayItf self, SLmillisecond *period) {
    if (!period)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *period = o->update_period;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vtable = {
    play_set_state, play_get_state, play_get_duration, play_get_position,
    play_register_callback, play_set_events, play_get_events, play_set_marker,
    play_clear_marker, play_get_marker, play_set_update_period, play_get_update_period,
};

// ── SLBufferQueueItf / SLAndroidSimpleBufferQueueItf ────────────────────────
static SLresult queue_enqueue(SLBufferQueueItf self, const void *data, SLuint32 size) {
    if (!data || !size)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    SLresult result = SL_RESULT_BUFFER_INSUFFICIENT;
    if (o->count < o->capacity) {
        o->buffers[(o->front + o->count) % o->capacity] = (struct buffer){data, size};
        ++o->count;
        o->end_reported = false;
        result = SL_RESULT_SUCCESS;
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

static SLresult queue_clear(SLBufferQueueItf self) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->front = o->count = o->play_index = o->consumed = 0;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(SLBufferQueueItf self, SLBufferQueueState *state) {
    if (!state)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    state->count = o->count;
    state->playIndex = o->play_index;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_register_callback(SLBufferQueueItf self, slBufferQueueCallback callback,
                                        void *context) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    SLresult result = SL_RESULT_PRECONDITIONS_VIOLATED;
    if (o->play_state == SL_PLAYSTATE_STOPPED) {
        o->queue_callback = callback;
        o->queue_context = context;
        result = SL_RESULT_SUCCESS;
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

// SLAndroidSimpleBufferQueueItf_ has the same layout and state structure.
static const struct SLBufferQueueItf_ queue_vtable = {
    queue_enqueue, queue_clear, queue_get_state, queue_register_callback,
};

// ── SLVolumeItf ─────────────────────────────────────────────────────────────
static SLresult volume_set_level(SLVolumeItf self, SLmillibel level) {
    if (level > 0) // PLATFORM_MILLIBEL_MAX_VOLUME
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->level = level;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_level(SLVolumeItf self, SLmillibel *level) {
    if (!level)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *level = o->level;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_max_level(SLVolumeItf self, SLmillibel *level) {
    (void)self;
    if (!level)
        return SL_RESULT_PARAMETER_INVALID;
    *level = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set_mute(SLVolumeItf self, SLboolean mute) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->mute = mute != SL_BOOLEAN_FALSE;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_mute(SLVolumeItf self, SLboolean *mute) {
    if (!mute)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *mute = o->mute;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_enable_stereo(SLVolumeItf self, SLboolean enable) {
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->stereo = enable != SL_BOOLEAN_FALSE;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_is_stereo_enabled(SLVolumeItf self, SLboolean *enable) {
    if (!enable)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *enable = o->stereo;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set_stereo_position(SLVolumeItf self, SLpermille position) {
    if (position < -1000 || position > 1000)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->stereo_position = position;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_stereo_position(SLVolumeItf self, SLpermille *position) {
    if (!position)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *position = o->stereo_position;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_vtable = {
    volume_set_level, volume_get_level, volume_get_max_level, volume_set_mute,
    volume_get_mute, volume_enable_stereo, volume_is_stereo_enabled,
    volume_set_stereo_position, volume_get_stereo_position,
};

// ── SLEffectSendItf ─────────────────────────────────────────────────────────
// The output mix has no auxiliary (reverb) effects, so every effect argument is
// invalid, as in wilhelm for an effect the mix does not hold. The direct level
// attenuates the dry signal.
static SLresult send_enable(SLEffectSendItf self, const void *effect, SLboolean enable,
                            SLmillibel level) {
    (void)self, (void)effect, (void)enable, (void)level;
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult send_is_enabled(SLEffectSendItf self, const void *effect, SLboolean *enable) {
    (void)self, (void)effect, (void)enable;
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult send_set_direct_level(SLEffectSendItf self, SLmillibel level) {
    if (level > 0)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    o->direct_level = level;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult send_get_direct_level(SLEffectSendItf self, SLmillibel *level) {
    if (!level)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    *level = o->direct_level;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult send_set_level(SLEffectSendItf self, const void *effect, SLmillibel level) {
    (void)self, (void)effect, (void)level;
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult send_get_level(SLEffectSendItf self, const void *effect, SLmillibel *level) {
    (void)self, (void)effect, (void)level;
    return SL_RESULT_PARAMETER_INVALID;
}

static const struct SLEffectSendItf_ effect_send_vtable = {
    send_enable, send_is_enabled, send_set_direct_level, send_get_direct_level,
    send_set_level, send_get_level,
};

// ── SLAndroidConfigurationItf ───────────────────────────────────────────────
static SLresult config_set(SLAndroidConfigurationItf self, const SLchar *key, const void *value,
                           SLuint32 size) {
    if (!key || !value || !size)
        return SL_RESULT_PARAMETER_INVALID;
    const char *name = (const char *)key;
    bool stream = !strcmp(name, (const char *)SL_ANDROID_KEY_STREAM_TYPE);
    if (!stream && strcmp(name, (const char *)SL_ANDROID_KEY_PERFORMANCE_MODE))
        return SL_RESULT_PARAMETER_INVALID;
    if (size < sizeof(SLuint32))
        return SL_RESULT_BUFFER_INSUFFICIENT;
    SLuint32 v;
    memcpy(&v, value, sizeof(v));
    if (stream ? v > (SLuint32)SL_ANDROID_STREAM_NOTIFICATION
               : v > SL_ANDROID_PERFORMANCE_POWER_SAVING)
        return SL_RESULT_PARAMETER_INVALID;
    struct object *o = OWNER(self);
    pthread_mutex_lock(&o->lock);
    SLresult result = SL_RESULT_PRECONDITIONS_VIOLATED; // the stream opens at Realize
    if (o->state == SL_OBJECT_STATE_UNREALIZED) {
        if (stream)
            o->stream_type = (SLint32)v;
        else
            o->performance = v;
        result = SL_RESULT_SUCCESS;
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

static SLresult config_get(SLAndroidConfigurationItf self, const SLchar *key, SLuint32 *size,
                           void *value) {
    if (!key || !size)
        return SL_RESULT_PARAMETER_INVALID;
    const char *name = (const char *)key;
    bool stream = !strcmp(name, (const char *)SL_ANDROID_KEY_STREAM_TYPE);
    if (!stream && strcmp(name, (const char *)SL_ANDROID_KEY_PERFORMANCE_MODE))
        return SL_RESULT_PARAMETER_INVALID;
    SLresult result = SL_RESULT_SUCCESS;
    if (value && *size < sizeof(SLuint32)) {
        result = SL_RESULT_BUFFER_INSUFFICIENT;
    } else if (value) {
        struct object *o = OWNER(self);
        pthread_mutex_lock(&o->lock);
        SLuint32 v = stream ? (SLuint32)o->stream_type : o->performance;
        pthread_mutex_unlock(&o->lock);
        memcpy(value, &v, sizeof(v));
    }
    *size = sizeof(SLuint32);
    return result;
}

static SLresult config_acquire_proxy(SLAndroidConfigurationItf self, SLuint32 type,
                                     jobject *proxy) {
    (void)self, (void)type;
    if (proxy)
        *proxy = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED; // routing proxies wrap a Java AudioTrack
}

static SLresult config_release_proxy(SLAndroidConfigurationItf self, SLuint32 type) {
    (void)self, (void)type;
    return SL_RESULT_PRECONDITIONS_VIOLATED; // none was acquired
}

static const struct SLAndroidConfigurationItf_ config_vtable = {
    config_set, config_get, config_acquire_proxy, config_release_proxy,
};

// ── Exported entry points ───────────────────────────────────────────────────
SLresult slCreateEngine(SLObjectItf *out, SLuint32 option_count, const SLEngineOption *options,
                        SLuint32 count, const SLInterfaceID *ids, const SLboolean *required) {
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *out = NULL;
    if (option_count && !options)
        return SL_RESULT_PARAMETER_INVALID;
    for (SLuint32 i = 0; i < option_count; ++i)
        if (options[i].feature != SL_ENGINEOPTION_THREADSAFE &&
            options[i].feature != SL_ENGINEOPTION_LOSSOFCONTROL)
            return SL_RESULT_PARAMETER_INVALID;
    unsigned exposed;
    SLresult result = check_interfaces(KIND_ENGINE, count, ids, required, &exposed);
    if (result != SL_RESULT_SUCCESS)
        return result;
    pthread_mutex_lock(&engine_lock);
    if (!engine)
        engine = new_object(KIND_ENGINE, exposed);
    if (engine) {
        ++engine_refs;
        *out = (SLObjectItf)&engine->object.vtable;
    } else {
        result = SL_RESULT_MEMORY_FAILURE;
    }
    pthread_mutex_unlock(&engine_lock);
    return result;
}

SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *count) {
    return engine_query_num_interfaces(NULL, SL_OBJECTID_ENGINE, count);
}

SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *iid) {
    return engine_query_interfaces(NULL, SL_OBJECTID_ENGINE, index, iid);
}
