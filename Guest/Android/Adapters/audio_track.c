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
// (ServerProxy, AudioTrackServerProxy, StaticAudioTrackServerProxy),
// media/libnbaio SingleStateQueue.h and services/audioflinger Tracks.cpp/Threads.cpp.
//
// A static track (MODE_STATIC, SoundPool) plays the client's own shared buffer: its
// region holds only the control block, whose union carries the loop/position queue
// from the client and the position/loop-count queue back. Both kinds follow the
// client's sample rate (mSampleRate) by resampling to the stream's rate.
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// ── audio_track_cblk_t (arm64 layout, 232 bytes) ───────────────────────
#define CBLK_UNDERRUN 0x01
#define CBLK_FORCEREADY 0x02
#define CBLK_INVALID 0x04
#define CBLK_LOOP_CYCLE 0x20
#define CBLK_LOOP_FINAL 0x40
#define CBLK_BUFFER_END 0x80
#define CBLK_FUTEX_WAKE 1
#define MIN_LOOP 16 // frames in one loop iteration, at least

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

typedef struct { // StaticAudioTrackState
    uint32_t loop_start, loop_end;
    int32_t loop_count;
    uint32_t loop_sequence, position, position_sequence;
} static_state;

typedef struct { // StaticAudioTrackPosLoop
    uint32_t buffer_position;
    int32_t loop_count;
} pos_loop;

typedef struct { // AudioTrackSharedStatic: two SingleStateQueue<T>::Shared
    struct { volatile int32_t ack, sequence; static_state value; } state;  // client -> server
    struct { volatile int32_t ack, sequence; pos_loop value; } pos_loop;   // server -> client
} static_shared;

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
        static_shared statics;
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
_Static_assert(sizeof(static_shared) == 48, "AudioTrackSharedStatic size");
_Static_assert(offsetof(cblk_t, u.statics.pos_loop) == 216, "mPosLoopQueue");

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
    int32_t (*request_stop)(AAudioStream *);
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
    SYM(request_stop, "AAudioStream_requestStop");
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
    uint32_t stream_rate; // the host stream's rate: the track's rate at creation

    // StaticAudioTrackServerProxy, for a track playing the client's shared buffer
    int is_static;
    uint8_t *shared_map;
    size_t shared_map_size;
    static_state static_now;    // mState
    int32_t static_observed;    // the state queue's Observer::mSequence
    int32_t pos_loop_sequence;  // the position queue's Mutator::mSequence
    int64_t static_ready;       // mFramesReady; INT64_MAX while looping forever

    // Linear resampler from the client's current rate to stream_rate
    int primed;         // input frames held in history (0..2)
    double phase;       // position between history frames 0 and 1
    float history[2][8];

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
    int64_t output_frames; // frames handed to the host stream while playing, for tests
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
static int streaming_obtain(track_t *t, buffer_t *b) {
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
static void streaming_release(track_t *t, size_t frames) {
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

static size_t streaming_frames_ready(track_t *t) {
    flush_if_needed(t);
    int64_t filled = (int32_t)((uint32_t)get_rear(t) - (uint32_t)t->cblk->u.streaming.front);
    return (0 <= filled && filled <= (int64_t)t->frame_count) ? (size_t)filled : 0;
}

// AudioTrackServerProxy::tallyUnderrunFrames
static void streaming_tally_underrun(track_t *t, uint32_t frames) {
    streaming_shared *s = &t->cblk->u.streaming;
    if (frames > 0) {
        s->underrun_frames += frames;
        if (!t->underrunning) { t->underrun_count++; s->underrun_count = t->underrun_count; t->underrunning = 1; }
        __atomic_fetch_or(&t->cblk->flags, CBLK_UNDERRUN, __ATOMIC_SEQ_CST);
    } else {
        t->underrunning = 0;
    }
}

// ── static tracks (StaticAudioTrackServerProxy) ────────────────────────

// SingleStateQueue<StaticAudioTrackState>::Observer::poll: the client's latest loop and
// position request, read between two equal even sequence numbers.
static int poll_static_state(track_t *t, static_state *value) {
    __typeof__(t->cblk->u.statics.state) *q = &t->cblk->u.statics.state;
    int32_t before = __atomic_load_n(&q->sequence, __ATOMIC_RELAXED);
    if (before == t->static_observed) return 0;
    for (int tries = 0;;) {
        if (before & 1) {
            if (++tries >= 5) return 0;
            before = __atomic_load_n(&q->sequence, __ATOMIC_RELAXED);
            continue;
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        static_state copy;
        memcpy(&copy, (const void *)&q->value, sizeof(copy));
        __atomic_thread_fence(__ATOMIC_SEQ_CST); // android_atomic_release_load
        int32_t after = __atomic_load_n(&q->sequence, __ATOMIC_RELAXED);
        if (after == before) {
            *value = copy;
            q->ack = before;
            t->static_observed = before;
            return 1;
        }
        if (++tries >= 5) return 0;
        before = after;
    }
}

// Observer::done: the client may read the position queue again.
static void static_state_done(track_t *t) {
    __typeof__(t->cblk->u.statics.state) *q = &t->cblk->u.statics.state;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&q->ack, q->ack + 1, __ATOMIC_RELAXED);
}

// SingleStateQueue<StaticAudioTrackPosLoop>::Mutator::push
static void push_pos_loop(track_t *t) {
    __typeof__(t->cblk->u.statics.pos_loop) *q = &t->cblk->u.statics.pos_loop;
    t->pos_loop_sequence++;
    __atomic_store_n(&q->sequence, t->pos_loop_sequence, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->value.buffer_position = t->static_now.position;
    q->value.loop_count = t->static_now.loop_count;
    t->pos_loop_sequence++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&q->sequence, t->pos_loop_sequence, __ATOMIC_RELAXED);
}

// updateStateWithLoop
static int update_loop(const track_t *t, static_state *local, const static_state *update) {
    if (local->loop_sequence == update->loop_sequence) return 0;
    uint32_t position = local->position;
    int valid = update->loop_count == 0;
    if (update->loop_count >= -1 && update->loop_count != 0 && update->loop_start < update->loop_end &&
        update->loop_end <= t->frame_count && update->loop_end - update->loop_start >= MIN_LOOP) {
        if (position >= update->loop_end) position = update->loop_start; // wraps to the loop start
        valid = 1;
    }
    if (!valid || position > t->frame_count) return -1;
    local->position = position;
    local->loop_count = update->loop_count;
    local->loop_end = update->loop_end;
    local->loop_start = update->loop_start;
    local->loop_sequence = update->loop_sequence;
    return 0;
}

// updateStateWithPosition
static int update_position(const track_t *t, static_state *local, const static_state *update) {
    if (local->position_sequence == update->position_sequence) return 0;
    if (update->position > t->frame_count) return -1;
    if (local->loop_count != 0 && update->position >= local->loop_end) local->loop_count = 0;
    local->position = update->position;
    local->position_sequence = update->position_sequence;
    return 0;
}

// pollPosition: applies the client's latest request, in the order it made them (the newer
// sequence number is the later change). An invalid request shuts the track down.
static int static_poll(track_t *t) {
    if (t->shutdown) return -1;
    static_state update;
    if (poll_static_state(t, &update)) {
        static_state next = t->static_now;
        int loop_first = (int32_t)update.loop_sequence - (int32_t)update.position_sequence < 0;
        int bad = loop_first ? update_loop(t, &next, &update) || update_position(t, &next, &update)
                             : update_position(t, &next, &update) || update_loop(t, &next, &update);
        if (bad) {
            static_state_done(t);
            t->shutdown = 1;
            return -1;
        }
        t->static_now = next;
        if (next.loop_count == -1) t->static_ready = INT64_MAX;
        else if (next.loop_count == 0) t->static_ready = (int64_t)t->frame_count - next.position;
        else t->static_ready = (int64_t)next.loop_count * (next.loop_end - next.loop_start) +
                               t->frame_count - next.position;
        push_pos_loop(t);
        static_state_done(t);
    }
    return 0;
}

static size_t static_frames_ready(track_t *t) {
    if (static_poll(t) != 0) return 0;
    return t->static_ready > (int64_t)SIZE_MAX ? SIZE_MAX : (size_t)t->static_ready;
}

// obtainBuffer: frames up to the loop end, or the buffer end without a loop.
static int static_obtain(track_t *t, buffer_t *b) {
    if (static_poll(t) != 0) {
        b->frames = 0; b->raw = NULL; b->non_contig = 0; t->unreleased = 0;
        return -ENODEV;
    }
    const uint32_t position = t->static_now.position;
    const uint32_t end = t->static_now.loop_count != 0 ? t->static_now.loop_end : t->frame_count;
    size_t avail = position < end ? end - position : 0;
    if (avail > b->frames) avail = b->frames;
    b->frames = avail;
    b->raw = avail ? t->buffers + (size_t)position * t->frame_size : NULL;
    b->non_contig = 0;
    t->unreleased = avail;
    return avail ? 0 : -EAGAIN;
}

// releaseBuffer: advances the position, wraps at the loop end and flags the loop and buffer ends.
static void static_release(track_t *t, size_t frames) {
    if (!frames || frames > t->unreleased) return;
    t->unreleased -= frames;
    const uint32_t position = t->static_now.position;
    uint32_t next = position + (uint32_t)frames;
    int32_t flags = 0;
    if (!(position <= next && next <= t->frame_count)) {
        next = t->frame_count;
    } else if (t->static_now.loop_count != 0 && next == t->static_now.loop_end) {
        next = t->static_now.loop_start;
        flags = t->static_now.loop_count == -1 || --t->static_now.loop_count != 0 ? CBLK_LOOP_CYCLE : CBLK_LOOP_FINAL;
    }
    if (next == t->frame_count) flags |= CBLK_BUFFER_END;
    t->static_now.position = next;
    if (t->static_ready != INT64_MAX) t->static_ready -= (int64_t)frames;
    t->cblk->server += (uint32_t)frames;
    t->released += (int64_t)frames;
    push_pos_loop(t);
    if (flags) __atomic_fetch_or(&t->cblk->flags, flags, __ATOMIC_SEQ_CST);
}

// ── either kind ────────────────────────────────────────────────────────

static int obtain(track_t *t, buffer_t *b) { return t->is_static ? static_obtain(t, b) : streaming_obtain(t, b); }

static void release(track_t *t, size_t frames) {
    if (t->is_static) static_release(t, frames); else streaming_release(t, frames);
}

static size_t frames_ready(track_t *t) { return t->is_static ? static_frames_ready(t) : streaming_frames_ready(t); }

// A static track has no underrun counters (StaticAudioTrackServerProxy::tallyUnderrunFrames).
static void tally_underrun(track_t *t, uint32_t frames) {
    if (!t->is_static) streaming_tally_underrun(t, frames);
    else if (frames) __atomic_fetch_or(&t->cblk->flags, CBLK_UNDERRUN, __ATOMIC_SEQ_CST);
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

// Copies up to `want` frames from the client's buffer at the stream's own rate.
static size_t pull_direct(track_t *t, float *out, size_t want, float left, float right) {
    size_t produced = 0;
    while (produced < want) {
        buffer_t b = {want - produced, NULL, 0};
        if (obtain(t, &b) != 0) break;
        convert(t, b.raw, out + produced * (size_t)t->channels, b.frames, left, right);
        release(t, b.frames);
        produced += b.frames;
    }
    return produced;
}

// One client frame at a time out of obtained buffers; release() takes what was read.
typedef struct { buffer_t b; size_t used; } reader_t;

static int read_frame(track_t *t, reader_t *r, float *dst, float left, float right) {
    if (r->used == r->b.frames) {
        if (r->b.frames) release(t, r->b.frames);
        r->b = (buffer_t){256, NULL, 0};
        r->used = 0;
        if (obtain(t, &r->b) != 0) { r->b.frames = 0; return 0; }
    }
    convert(t, (const uint8_t *)r->b.raw + r->used * t->frame_size, dst, 1, left, right);
    r->used++;
    return 1;
}

// Linear interpolation from the client's rate to the stream's: `step` client frames per output
// frame. The two frames around the read position carry over between callbacks.
static size_t pull_resampled(track_t *t, float *out, size_t want, float left, float right, double step) {
    const size_t channels = (size_t)t->channels, bytes = channels * sizeof(float);
    float *a = t->history[0], *b = t->history[1];
    reader_t reader = {{0, NULL, 0}, 0};
    size_t produced = 0;
    while (produced < want) {
        while (t->primed < 2 || t->phase >= 1.0) {
            float next[8];
            if (!read_frame(t, &reader, next, left, right)) goto done;
            if (t->primed == 2) { memcpy(a, b, bytes); memcpy(b, next, bytes); t->phase -= 1.0; }
            else memcpy(t->primed++ ? b : a, next, bytes);
        }
        const float phase = (float)t->phase;
        for (size_t c = 0; c < channels; c++) out[produced * channels + c] = a[c] + (b[c] - a[c]) * phase;
        produced++;
        t->phase += step;
    }
done:
    if (reader.b.frames) release(t, reader.used);
    return produced;
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
        // A streaming track is ready once its buffer holds the start threshold (or the client
        // stopped it with a partial buffer, or forced readiness); after an underrun it fills
        // again before sounding. A static track's buffer is full from the start.
        if (!t->ready) {
            uint32_t threshold = t->cblk->start_threshold_in_frames;
            if (!threshold || threshold > t->frame_count) threshold = t->frame_count;
            if (frames_ready(t) >= threshold || (t->cblk->flags & CBLK_FORCEREADY) ||
                load_i32(&t->cblk->u.streaming.stop) != t->stop_last) t->ready = 1;
        }
        if (t->ready) {
            const uint32_t volume = t->cblk->volume_lr;
            const float left = float_from_gain(volume & 0xffff), right = float_from_gain(volume >> 16);
            // AudioTrack::setSampleRate writes mSampleRate; AudioFlinger's mixer resamples from it.
            // Like its resampler, at most 256 client frames are read per output frame.
            uint32_t rate = __atomic_load_n(&t->cblk->sample_rate, __ATOMIC_RELAXED);
            if (!rate) rate = t->stream_rate;
            double step = (double)rate / t->stream_rate;
            if (step > 256) step = 256;
            produced = rate == t->stream_rate && !t->primed ? pull_direct(t, out, want, left, right)
                                                             : pull_resampled(t, out, want, left, right, step);
            t->output_frames += (int64_t)produced;
            for (size_t f = 0; f < produced; f++)
                for (int c = 0; c < 2 && c < t->channels; c++) {
                    float v = out[f * (size_t)t->channels + (size_t)c];
                    if (v < 0) v = -v;
                    if (v > t->peak[c]) t->peak[c] = v;
                }
            if (produced < want && t->is_static) {
                // The buffer (or its last loop) has played: AudioFlinger takes the track off its
                // mixer and leaves it ACTIVE until the client stops it or sets a new position.
                tally_underrun(t, (uint32_t)(want - produced));
                if (frames_ready(t) == 0) result = AAUDIO_CALLBACK_RESULT_STOP;
            } else if (produced < want) {
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

// Maps the client's shared buffer of a static track read-only: `size` bytes at `offset` in `fd`.
static int map_shared(track_t *t, int fd, int64_t offset, int64_t size) {
    struct stat info;
    const int64_t page = sysconf(_SC_PAGESIZE);
    if (offset < 0 || size <= 0 || page <= 0 || fstat(fd, &info) != 0 || offset > info.st_size - size) return -EINVAL;
    const int64_t start = offset & ~(page - 1);
    t->shared_map_size = (size_t)(offset - start + size);
    void *map = mmap(NULL, t->shared_map_size, PROT_READ, MAP_SHARED, fd, (off_t)start);
    if (map == MAP_FAILED) return -errno;
    t->shared_map = map;
    t->buffers = t->shared_map + (offset - start);
    return 0;
}

// A track over a control block region from createRegion. Streaming: the region also holds the
// ring (shared_fd < 0). Static: the client's buffer is `shared_size` bytes at `shared_offset`
// in `shared_fd`, which may be closed afterwards.
JNIEXPORT jlong JNICALL Java_FrameworkAudioTracks_create(JNIEnv *env, jclass type, jint fd, jlong region_size,
                                                         jint frame_count, jint channels, jint format,
                                                         jint sample_rate, jint shared_fd, jlong shared_offset,
                                                         jlong shared_size) {
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
    t->is_static = shared_fd >= 0;
    t->stream_rate = (uint32_t)sample_rate;
    const size_t ring = t->is_static ? 0 : (size_t)t->frame_count_p2 * t->frame_size;
    if (t->region_size < sizeof(cblk_t) + ring ||
        (t->is_static && (uint64_t)shared_size / t->frame_size != (uint64_t)frame_count)) { free(t); return -EINVAL; }
    if (t->is_static) {
        int error = map_shared(t, shared_fd, shared_offset, shared_size);
        if (error) { free(t); return error; }
        t->static_ready = t->frame_count;
    }
    void *base = mmap(NULL, t->region_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        int error = errno;
        if (t->shared_map) munmap(t->shared_map, t->shared_map_size);
        free(t);
        return -error;
    }
    t->base = base;
    t->cblk = base;
    if (!t->is_static) t->buffers = t->base + sizeof(cblk_t);
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
        if (t->shared_map) munmap(t->shared_map, t->shared_map_size);
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
    // AudioTrackServerProxy::start: later reads stop at the client's next stop position. A static
    // track's buffer is already filled (FS_FILLED); a resumed track keeps playing; a started
    // streaming track fills first.
    if (!t->is_static) t->stop_last = load_i32(&t->cblk->u.streaming.stop);
    if (t->state != TS_PAUSED) {
        t->ready = t->is_static;
        t->primed = 0;
        t->phase = 0;
    }
    t->state = TS_ACTIVE;
    atomic_store(&t->cblk->state, TS_ACTIVE);
    pthread_mutex_unlock(&t->lock);
    return aaudio.request_start(t->stream) == 0 ? 0 : -EIO;
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_stop(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    // A streaming client already wrote its stop position; the callback drains up to it and stops.
    // A static track ends at once (Track::framesReady is zero once it is stopped).
    track_t *t = (track_t *)(intptr_t)handle;
    int halt = 0;
    pthread_mutex_lock(&t->lock);
    if (t->is_static && (t->state == TS_ACTIVE || t->state == TS_PAUSED)) {
        halt = 1;
        t->state = TS_STOPPED;
        atomic_store(&t->cblk->state, TS_STOPPED);
    } else if (t->state == TS_ACTIVE) { t->state = TS_STOPPING; atomic_store(&t->cblk->state, TS_STOPPING); }
    else if (t->state == TS_PAUSED) { t->state = TS_STOPPED; atomic_store(&t->cblk->state, TS_STOPPED); }
    pthread_mutex_unlock(&t->lock);
    if (halt) aaudio.request_stop(t->stream);
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
    // Only a stopped or paused streaming track is flushed (IAudioTrack::flush); AudioTrack never
    // flushes a static one, whose control block has no streaming positions.
    int flushable = !t->is_static && t->state != TS_ACTIVE && t->state != TS_STOPPING;
    if (flushable) {
        flush_if_needed(t);
        t->state = TS_FLUSHED;
        atomic_store(&t->cblk->state, TS_FLUSHED);
        t->ready = 0;
        t->primed = 0;
        t->phase = 0;
    }
    pthread_mutex_unlock(&t->lock);
    if (flushable) aaudio.request_flush(t->stream);
}

JNIEXPORT void JNICALL Java_FrameworkAudioTracks_signal(JNIEnv *env, jclass type, jlong handle) {
    (void)env; (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    pthread_mutex_lock(&t->lock);
    if (!t->is_static) flush_if_needed(t);
    pthread_mutex_unlock(&t->lock);
}

// out[0] = frames consumed, out[1] = CLOCK_MONOTONIC ns of the report, out[2] = underruns,
// out[3], out[4] = peak output level of channels 0 and 1 in millionths of full scale,
// out[5] = frames handed to the host stream.
JNIEXPORT jint JNICALL Java_FrameworkAudioTracks_position(JNIEnv *env, jclass type, jlong handle, jlongArray out) {
    (void)type;
    track_t *t = (track_t *)(intptr_t)handle;
    jlong values[6];
    pthread_mutex_lock(&t->lock);
    values[0] = t->released;
    values[1] = now_ns();
    values[2] = t->underrun_count;
    values[3] = (jlong)(t->peak[0] * 1e6f);
    values[4] = (jlong)(t->peak[1] * 1e6f);
    values[5] = t->output_frames;
    pthread_mutex_unlock(&t->lock);
    (*env)->SetLongArrayRegion(env, out, 0, 6, values);
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
    // CBLK_INVALID makes a client blocked in obtainBuffer fail with DEAD_OBJECT.
    __atomic_fetch_or(&t->cblk->flags, CBLK_INVALID, __ATOMIC_SEQ_CST);
    wake_client(t, INT_MAX);
    munmap(t->base, t->region_size);
    if (t->shared_map) munmap(t->shared_map, t->shared_map_size);
    pthread_mutex_destroy(&t->lock);
    free(t);
}
