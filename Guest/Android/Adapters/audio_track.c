// Server side of Android's AudioTrack shared-memory protocol.
//
// libaudioclient's AudioTrack writes PCM into a control block + ring buffer
// (audio_track_cblk_t, AudioTrackShared.h) and waits on its futex. AudioFlinger
// normally reads it from a mixer thread; here one AAudio data-callback stream per
// track pulls frames from the ring, converts them to float, applies the track's
// volume and hands them to the host audio engine through the managed libaaudio.
//
// The layout and the obtain/release/flush/stop/underrun rules follow
// frameworks/av at android-15.0.0_r1: AudioTrackShared.h, AudioTrackShared.cpp
// (ServerProxy, AudioTrackServerProxy) and services/audioflinger/Tracks.cpp.
// Only streaming tracks exist so far; a shared (static) buffer is refused.
//
// Java side: FrameworkAudioTracks (Guest/Android/Managed/Framework).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// ── audio_track_cblk_t (arm64 layout, 232 bytes) ───────────────────────
#define CBLK_UNDERRUN 0x01
#define CBLK_FORCEREADY 0x02
#define CBLK_FUTEX_WAKE 1

enum track_state { // TrackBase::track_state; the client reads IDLE, ACTIVE and PAUSING
    TS_IDLE = 0, TS_FLUSHED = 1, TS_STOPPED = 2, TS_STOPPING = 3, TS_ACTIVE = 6, TS_PAUSING = 7, TS_PAUSED = 8
};

typedef struct { // SingleStateQueue<AudioPlaybackRate>::Shared
    int32_t ack, sequence;
    float speed, pitch;
    int32_t stretch_mode, fallback_mode;
} playback_rate_shared;

typedef struct { // ExtendedTimestamp
    alignas(8) int64_t position[5];
    int64_t time_ns[5];
    int64_t timebase_offset[2];
    int64_t flushed;
} extended_timestamp;
enum { LOCATION_CLIENT, LOCATION_SERVER, LOCATION_KERNEL, LOCATION_SERVER_LASTKERNELOK, LOCATION_KERNEL_LASTKERNELOK };

typedef struct { // SingleStateQueue<ExtendedTimestamp>::Shared
    int32_t ack, sequence;
    extended_timestamp value;
} timestamp_shared;

typedef struct {
    int32_t front, rear, flush, stop;
    uint32_t underrun_frames, underrun_count;
} streaming_shared;

typedef struct {
    uint32_t server;
    uint32_t pad1;
    volatile int32_t futex;
    uint32_t minimum;
    uint32_t volume_lr;
    uint32_t sample_rate;
    playback_rate_shared playback_rate;
    uint16_t send_level;
    uint16_t pad2;
    timestamp_shared timestamp;
    volatile uint32_t buffer_size_in_frames;
    volatile uint32_t start_threshold_in_frames;
    volatile int32_t flags;
    _Atomic int32_t state;
    union {
        streaming_shared streaming;
        int32_t align[12]; // the static-buffer queues are larger
    } u;
} cblk_t;

_Static_assert(sizeof(cblk_t) == 232, "audio_track_cblk_t size");
_Static_assert(offsetof(cblk_t, futex) == 8, "mFutex");
_Static_assert(offsetof(cblk_t, volume_lr) == 16, "mVolumeLR");
_Static_assert(offsetof(cblk_t, playback_rate) == 24, "mPlaybackRateQueue");
_Static_assert(offsetof(cblk_t, send_level) == 48, "mSendLevel");
_Static_assert(offsetof(cblk_t, timestamp) == 56, "mExtendedTimestampQueue");
_Static_assert(offsetof(cblk_t, buffer_size_in_frames) == 168, "mBufferSizeInFrames");
_Static_assert(offsetof(cblk_t, flags) == 176, "mFlags");
_Static_assert(offsetof(cblk_t, u) == 184, "union u");

// Client sample formats (pcm of AudioFormatDescription).
enum { FMT_U8 = 0, FMT_I16 = 1, FMT_I32 = 2, FMT_Q8_24 = 3, FMT_F32 = 4, FMT_I24 = 5 };

// ── managed libaaudio ──────────────────────────────────────────────────
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t (*data_callback)(AAudioStream *, void *, void *, int32_t);
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_FORMAT_PCM_FLOAT = 2, AAUDIO_PERFORMANCE_MODE_NONE = 10,
       AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };

static struct {
    int loaded;
    int32_t (*create_builder)(AAudioStreamBuilder **);
    void (*set_direction)(AAudioStreamBuilder *, int32_t);
    void (*set_sample_rate)(AAudioStreamBuilder *, int32_t);
    void (*set_channel_count)(AAudioStreamBuilder *, int32_t);
    void (*set_format)(AAudioStreamBuilder *, int32_t);
    void (*set_performance_mode)(AAudioStreamBuilder *, int32_t);
    void (*set_data_callback)(AAudioStreamBuilder *, data_callback, void *);
    int32_t (*open_stream)(AAudioStreamBuilder *, AAudioStream **);
    int32_t (*delete_builder)(AAudioStreamBuilder *);
    int32_t (*request_start)(AAudioStream *);
    int32_t (*request_pause)(AAudioStream *);
    int32_t (*request_flush)(AAudioStream *);
    int32_t (*close)(AAudioStream *);
} aaudio;
static pthread_once_t aaudio_once = PTHREAD_ONCE_INIT;

static void load_aaudio(void) {
    void *lib = dlopen("libaaudio.so", RTLD_NOW);
    if (!lib) return;
#define SYM(field, name) if (!(*(void **)&aaudio.field = dlsym(lib, name))) return
    SYM(create_builder, "AAudio_createStreamBuilder");
    SYM(set_direction, "AAudioStreamBuilder_setDirection");
    SYM(set_sample_rate, "AAudioStreamBuilder_setSampleRate");
    SYM(set_channel_count, "AAudioStreamBuilder_setChannelCount");
    SYM(set_format, "AAudioStreamBuilder_setFormat");
    SYM(set_performance_mode, "AAudioStreamBuilder_setPerformanceMode");
    SYM(set_data_callback, "AAudioStreamBuilder_setDataCallback");
    SYM(open_stream, "AAudioStreamBuilder_openStream");
    SYM(delete_builder, "AAudioStreamBuilder_delete");
    SYM(request_start, "AAudioStream_requestStart");
    SYM(request_pause, "AAudioStream_requestPause");
    SYM(request_flush, "AAudioStream_requestFlush");
    SYM(close, "AAudioStream_close");
#undef SYM
    aaudio.loaded = 1;
}

// ── minifloat gains (audio_utils/minifloat.c) ──────────────────────────
static float float_from_gain(uint16_t gain) {
    // 3 exponent bits (excess 6) and 13 mantissa bits, with a hidden bit when the exponent is not 0.
    static const float scale[8] = {1.0f / 64, 1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f, 2.0f};
    const int mantissa = gain & ((1 << 13) - 1);
    const int exponent = (gain >> 13) & 7;
    return (exponent > 0 ? (1 << 13) | mantissa : mantissa << 1) * (1.0f / (1 << 14)) * scale[exponent];
}

// ── one track ──────────────────────────────────────────────────────────
typedef struct {
    uint8_t *base;
    size_t region_size;
    cblk_t *cblk;
    uint8_t *buffers;
    uint32_t frame_count, frame_count_p2, frame_size;
    int channels, format, bytes_per_sample;

    pthread_mutex_t lock;
    int state;          // enum track_state
    int ready;          // false while filling: nothing is consumed until the start threshold
    AAudioStream *stream;

    // ServerProxy
    int32_t flush;      // our copy of mStreaming.mFlush
    int32_t stop_last;
    size_t unreleased;
    size_t avail_to_client;
    int64_t released, flushed;
    uint32_t underrun_count;
    int underrunning;
    int shutdown;       // shared memory corrupt
    int32_t timestamp_sequence;
    float peak[2];      // largest converted sample per channel (first two), for tests
} track_t;

static uint32_t roundup_pow2(uint32_t v) {
    uint32_t r = 1;
    while (r < v) r <<= 1;
    return r;
}

static void wake_client(track_t *t, int count) {
    int32_t old = __atomic_fetch_or(&t->cblk->futex, CBLK_FUTEX_WAKE, __ATOMIC_SEQ_CST);
    if (!(old & CBLK_FUTEX_WAKE)) syscall(__NR_futex, &t->cblk->futex, FUTEX_WAKE, count, NULL, NULL, 0);
}

static int32_t load_i32(const volatile int32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store_i32(volatile int32_t *p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

// ServerProxy::flushBufferIfNeeded
static void flush_if_needed(track_t *t) {
    streaming_shared *s = &t->cblk->u.streaming;
    int32_t flush = load_i32(&s->flush);
    if (flush == t->flush) return;
    int32_t rear = load_i32(&s->rear), front = s->front;
    const uint32_t overflow_bit = t->frame_count_p2 << 1, mask = overflow_bit - 1;
    int32_t new_front = (int32_t)(((uint32_t)front & ~mask) | ((uint32_t)flush & mask));
    int64_t filled = (int32_t)((uint32_t)rear - (uint32_t)new_front);
    if (filled >= (int64_t)overflow_bit) { new_front += (int32_t)overflow_bit; filled -= overflow_bit; }
    if (!(0 <= filled && filled <= (int64_t)t->frame_count)) new_front = rear; // corrupt: treat as a full flush
    t->flush = flush;
    store_i32(&s->front, new_front);
    wake_client(t, INT_MAX);
    t->flushed += (int32_t)((uint32_t)new_front - (uint32_t)front) & mask;
}

// AudioTrackServerProxy::getRear: the client's stop position limits what is read.
static int32_t get_rear(track_t *t) {
    streaming_shared *s = &t->cblk->u.streaming;
    int32_t stop = load_i32(&s->stop), rear = load_i32(&s->rear);
    if (stop != t->stop_last) {
        int32_t front = s->front;
        const uint32_t overflow_bit = t->frame_count_p2 << 1, mask = overflow_bit - 1;
        int32_t new_rear = (int32_t)(((uint32_t)rear & ~mask) | ((uint32_t)stop & mask));
        int64_t filled = (int32_t)((uint32_t)new_rear - (uint32_t)front);
        if (filled >= (int64_t)overflow_bit) { new_rear -= (int32_t)overflow_bit; filled -= overflow_bit; }
        if (0 <= filled && filled <= (int64_t)t->frame_count) return new_rear;
    }
    return rear;
}

typedef struct { size_t frames; void *raw; size_t non_contig; } buffer_t;

// ServerProxy::obtainBuffer for an output track. Returns 0, -EAGAIN or -ENODEV (corrupt).
static int obtain(track_t *t, buffer_t *b) {
    streaming_shared *s = &t->cblk->u.streaming;
    if (!t->shutdown) {
        flush_if_needed(t);
        int32_t rear = get_rear(t), front = s->front;
        int64_t filled = (int32_t)((uint32_t)rear - (uint32_t)front);
        if (0 <= filled && filled <= (int64_t)t->frame_count) {
            t->avail_to_client = t->frame_count - (size_t)filled;
            size_t part1 = t->frame_count_p2 - ((uint32_t)front & (t->frame_count_p2 - 1));
            if (part1 > (size_t)filled) part1 = (size_t)filled;
            if (part1 > b->frames) part1 = b->frames;
            b->non_contig = (size_t)filled - part1;
            b->raw = part1 ? t->buffers + (size_t)((uint32_t)front & (t->frame_count_p2 - 1)) * t->frame_size : NULL;
            b->frames = part1;
            t->unreleased = part1;
            return part1 ? 0 : -EAGAIN;
        }
        t->shutdown = 1;
    }
    b->frames = 0; b->raw = NULL; b->non_contig = 0; t->unreleased = 0;
    return -ENODEV;
}

// ServerProxy::releaseBuffer
static void release(track_t *t, size_t frames) {
    streaming_shared *s = &t->cblk->u.streaming;
    if (!frames || t->shutdown || frames > t->unreleased) return;
    t->unreleased -= frames;
    store_i32(&s->front, s->front + (int32_t)frames);
    t->cblk->server += (uint32_t)frames;
    t->released += (int64_t)frames;
    size_t half = t->frame_count / 2;
    if (!half) half = 1;
    size_t minimum = t->cblk->minimum;
    if (!minimum) minimum = half; else if (minimum > half) minimum = half;
    if (t->avail_to_client + frames >= minimum) wake_client(t, INT_MAX);
}

static size_t frames_ready(track_t *t) {
    flush_if_needed(t);
    int64_t filled = (int32_t)((uint32_t)get_rear(t) - (uint32_t)t->cblk->u.streaming.front);
    return (0 <= filled && filled <= (int64_t)t->frame_count) ? (size_t)filled : 0;
}

// AudioTrackServerProxy::tallyUnderrunFrames
static void tally_underrun(track_t *t, uint32_t frames) {
    streaming_shared *s = &t->cblk->u.streaming;
    if (frames > 0) {
        s->underrun_frames += frames;
        if (!t->underrunning) { t->underrun_count++; s->underrun_count = t->underrun_count; t->underrunning = 1; }
        __atomic_fetch_or(&t->cblk->flags, CBLK_UNDERRUN, __ATOMIC_SEQ_CST);
    } else {
        t->underrunning = 0;
    }
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// SingleStateQueue<ExtendedTimestamp>::Mutator::push. Only the server position is
// known: the host engine reports no presentation time, so the kernel location stays
// invalid (time -1) and AudioTrack.getTimestamp reports no timestamp.
static void push_timestamp(track_t *t) {
    timestamp_shared *q = &t->cblk->timestamp;
    extended_timestamp ts;
    for (int i = 0; i < 5; i++) { ts.position[i] = 0; ts.time_ns[i] = -1; }
    ts.timebase_offset[0] = ts.timebase_offset[1] = 0;
    ts.flushed = t->flushed;
    ts.position[LOCATION_SERVER] = t->released;
    ts.time_ns[LOCATION_SERVER] = now_ns();
    t->timestamp_sequence++;
    __atomic_store_n(&q->sequence, t->timestamp_sequence, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // android_atomic_acquire_store
    q->value = ts;
    t->timestamp_sequence++;
    __atomic_store_n(&q->sequence, t->timestamp_sequence, __ATOMIC_RELEASE);
}

// Converts `frames` client frames to float, scaling channel 0 by `left`, channel 1
// by `right` and any others by their mean.
static void convert(const track_t *t, const uint8_t *src, float *dst, size_t frames, float left, float right) {
    const int channels = t->channels;
    const float other = 0.5f * (left + right);
    for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < channels; c++) {
            float v;
            switch (t->format) {
            case FMT_U8: v = ((int)src[0] - 128) * (1.0f / 128.0f); break;
            case FMT_I16: { int16_t s; memcpy(&s, src, 2); v = s * (1.0f / 32768.0f); break; }
            case FMT_I32: { int32_t s; memcpy(&s, src, 4); v = (float)((double)s * (1.0 / 2147483648.0)); break; }
            case FMT_Q8_24: { int32_t s; memcpy(&s, src, 4); v = s * (1.0f / 16777216.0f); break; }
            case FMT_F32: memcpy(&v, src, 4); break;
            case FMT_I24: {
                int32_t s = (int32_t)((uint32_t)src[0] << 8 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 24) >> 8;
                v = s * (1.0f / 8388608.0f);
                break;
            }
            default: v = 0; break;
            }
            src += t->bytes_per_sample;
            v *= c == 0 ? left : c == 1 ? right : other;
            *dst++ = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        }
    }
}

// The host engine's pull: fill `frames` frames of float PCM.
static int32_t on_data(AAudioStream *stream, void *user, void *audio, int32_t frames) {
    (void)stream;
    track_t *t = user;
    float *out = audio;
    const size_t want = frames > 0 ? (size_t)frames : 0;
    size_t produced = 0;
    int32_t result = AAUDIO_CALLBACK_RESULT_CONTINUE;
    pthread_mutex_lock(&t->lock);
    if (t->state == TS_ACTIVE || t->state == TS_STOPPING) {
        // A track is ready once its buffer holds the start threshold (or the client
        // stopped it with a partial buffer, or forced readiness); after an underrun
        // it fills again before sounding.
        if (!t->ready) {
            uint32_t threshold = t->cblk->start_threshold_in_frames;
            if (!threshold || threshold > t->frame_count) threshold = t->frame_count;
            if (frames_ready(t) >= threshold || (t->cblk->flags & CBLK_FORCEREADY) ||
                load_i32(&t->cblk->u.streaming.stop) != t->stop_last) t->ready = 1;
        }
        if (t->ready) {
            const uint32_t volume = t->cblk->volume_lr;
            const float left = float_from_gain(volume & 0xffff), right = float_from_gain(volume >> 16);
            while (produced < want) {
                buffer_t b = {want - produced, NULL, 0};
                if (obtain(t, &b) != 0) break;
                float *dst = out + produced * (size_t)t->channels;
                convert(t, b.raw, dst, b.frames, left, right);
                for (size_t f = 0; f < b.frames; f++)
                    for (int c = 0; c < 2 && c < t->channels; c++) {
                        float v = dst[f * (size_t)t->channels + (size_t)c];
                        if (v < 0) v = -v;
                        if (v > t->peak[c]) t->peak[c] = v;
                    }
                release(t, b.frames);
                produced += b.frames;
            }
            if (produced < want) {
                const int stopping = load_i32(&t->cblk->u.streaming.stop) != t->stop_last;
                if (stopping && frames_ready(t) == 0) {
                    // Everything the client wrote before stop() has played.
                    t->state = TS_STOPPED;
                    atomic_store(&t->cblk->state, TS_STOPPED);
                    result = AAUDIO_CALLBACK_RESULT_STOP;
                } else if (!stopping) {
                    tally_underrun(t, (uint32_t)(want - produced));
                    t->ready = 0;
                }
            } else {
                tally_underrun(t, 0);
            }
        }
        push_timestamp(t);
    }
    pthread_mutex_unlock(&t->lock);
    if (produced < want) memset(out + produced * (size_t)t->channels, 0, (want - produced) * (size_t)t->channels * sizeof(float));
    return result;
}

// ── JNI ────────────────────────────────────────────────────────────────

// A memfd large enough for the control block and the sample ring.
JNIEXPORT jint JNICALL Java_FrameworkAudioTracks_createRegion(JNIEnv *env, jclass type, jlong size) {
    (void)env; (void)type;
    long fd = syscall(__NR_memfd_create, "audiotrack", 0);
    if (fd < 0) return -errno;
    if (ftruncate((int)fd, (off_t)size) != 0) { int error = errno; close((int)fd); return -error; }
    return (jint)fd;
}

JNIEXPORT jlong JNICALL Java_FrameworkAudioTracks_create(JNIEnv *env, jclass type, jint fd, jlong region_size,
                                                         jint frame_count, jint channels, jint format,
                                                         jint sample_rate) {
    (void)env; (void)type;
    pthread_once(&aaudio_once, load_aaudio);
    if (!aaudio.loaded) return -ENOSYS;
    static const int bytes[] = {1, 2, 4, 4, 4, 3};
    if (format < 0 || format > FMT_I24 || channels < 1 || channels > 8 || frame_count < 1 || sample_rate < 1)
        return -EINVAL;
    track_t *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    t->bytes_per_sample = bytes[format];
    t->format = format;
    t->channels = channels;
    t->frame_size = (uint32_t)(channels * t->bytes_per_sample);
    t->frame_count = (uint32_t)frame_count;
    t->frame_count_p2 = roundup_pow2(t->frame_count);
    t->region_size = (size_t)region_size;
    if (t->region_size < sizeof(cblk_t) + (size_t)t->frame_count_p2 * t->frame_size) { free(t); return -EINVAL; }
    void *base = mmap(NULL, t->region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { int error = errno; free(t); return -error; }
    t->base = base;
    t->cblk = base;
    t->buffers = t->base + sizeof(cblk_t);
    pthread_mutex_init(&t->lock, NULL);
    t->state = TS_IDLE;

    // audio_track_cblk_t(), then the ServerProxy constructors. The sample ring starts zeroed
    // (a new memfd is), and the client initializes the queues it observes.
    cblk_t *c = t->cblk;
    memset(c, 0, sizeof(*c));
    c->volume_lr = 0xE000E000u; // GAIN_MINIFLOAT_PACKED_UNITY
    c->buffer_size_in_frames = t->frame_count;
    c->start_threshold_in_frames = t->frame_count;
    c->sample_rate = (uint32_t)sample_rate;

    AAudioStreamBuilder *builder = NULL;
    int32_t result = aaudio.create_builder(&builder);
    if (result == 0) {
        aaudio.set_direction(builder, AAUDIO_DIRECTION_OUTPUT);
        aaudio.set_sample_rate(builder, sample_rate);
        aaudio.set_channel_count(builder, channels);
        aaudio.set_format(builder, AAUDIO_FORMAT_PCM_FLOAT);
        aaudio.set_performance_mode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
        aaudio.set_data_callback(builder, on_data, t);
        result = aaudio.open_stream(builder, &t->stream);
        aaudio.delete_builder(builder);
    }
    if (result != 0) {
        munmap(t->base, t->region_size);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return -EIO;
    }
    return (jlong)(intptr_t)t;
}

JNIEXPORT jint JNICALL Java_FrameworkAudioTracks_start(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    // AudioTrackServerProxy::start: later reads stop at the client's next stop position.
    t->stop_last = load_i32(&t->cblk->u.streaming.stop);
    if (t->state != TS_PAUSED) t->ready = 0; // a resumed track keeps playing; a started one fills first
    t->state = TS_ACTIVE;
    atomic_store(&t->cblk->state, TS_ACTIVE);
    pthread_mutex_unlock(&t->lock);
    return aaudio.request_start(t->stream) == 0 ? 0 : -EIO;
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_stop(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    // The client already wrote its stop position; the callback drains up to it and stops.
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    if (t->state == TS_ACTIVE) { t->state = TS_STOPPING; atomic_store(&t->cblk->state, TS_STOPPING); }
    else if (t->state == TS_PAUSED) { t->state = TS_STOPPED; atomic_store(&t->cblk->state, TS_STOPPED); }
    pthread_mutex_unlock(&t->lock);
    wake_client(t, INT_MAX);
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_pause(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    int active;
    pthread_mutex_lock(&t->lock);
    active = t->state == TS_ACTIVE || t->state == TS_STOPPING;
    if (active) { t->state = TS_PAUSED; atomic_store(&t->cblk->state, TS_PAUSED); }
    pthread_mutex_unlock(&t->lock);
    if (active) aaudio.request_pause(t->stream);
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_flush(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    // Only a stopped or paused track is flushed (IAudioTrack::flush).
    int flushable = t->state != TS_ACTIVE && t->state != TS_STOPPING;
    if (flushable) {
        flush_if_needed(t);
        t->state = TS_FLUSHED;
        atomic_store(&t->cblk->state, TS_FLUSHED);
        t->ready = 0;
    }
    pthread_mutex_unlock(&t->lock);
    if (flushable) aaudio.request_flush(t->stream);
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_signal(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    flush_if_needed(t);
    pthread_mutex_unlock(&t->lock);
}

// out[0] = frames consumed, out[1] = CLOCK_MONOTONIC ns of the report, out[2] = underruns,
// out[3], out[4] = peak output level of channels 0 and 1 in millionths of full scale.
JNIEXPORT jint JNICALL Java_FrameworkAudioTracks_position(JNIEnv *env, jclass type, jlong handle, jlongArray out) {
    (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    jlong values[5];
    pthread_mutex_lock(&t->lock);
    values[0] = t->released;
    values[1] = now_ns();
    values[2] = t->underrun_count;
    values[3] = (jlong)(t->peak[0] * 1e6f);
    values[4] = (jlong)(t->peak[1] * 1e6f);
    pthread_mutex_unlock(&t->lock);
    (*env)->SetLongArrayRegion(env, out, 0, 5, values);
    return 0;
}

// Wakes the client (it may be waiting in obtainBuffer) and frees everything.
JNIEXPORT void JNICALL Java_FrameworkAudioTracks_release(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    t->state = TS_IDLE;
    pthread_mutex_unlock(&t->lock);
    aaudio.close(t->stream); // waits for a running callback
    // CBLK_INVALID (0x04) makes a client blocked in obtainBuffer fail with DEAD_OBJECT.
    __atomic_fetch_or(&t->cblk->flags, 0x04, __ATOMIC_SEQ_CST);
    wake_client(t, INT_MAX);
    munmap(t->base, t->region_size);
    pthread_mutex_destroy(&t->lock);
    free(t);
}
