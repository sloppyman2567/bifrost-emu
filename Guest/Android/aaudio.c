#define _GNU_SOURCE // pthread_setname_np
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

// Managed libaaudio's data-callback thread. Real libaaudio runs the callback
// on a pthread it owns; this does the same so callbacks get Bionic TLS. Every
// other export is a thunk shim (Tools/Android/build-android-adapters.py). The types
// follow Android 15 <aaudio/AAudio.h>, which the pinned headers do not ship.

typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t aaudio_result_t;
typedef int32_t (*AAudioStream_dataCallback)(AAudioStream *stream, void *user,
                                             void *audio, int32_t frames);
enum { AAUDIO_OK = 0, AAUDIO_ERROR_NO_MEMORY = -887, AAUDIO_ERROR_INTERNAL = -896 };

// Thunk exports under hidden shim labels.
aaudio_result_t thunk_AAudioStreamBuilder_openStream(AAudioStreamBuilder *, AAudioStream **);
aaudio_result_t thunk_AAudioStream_close(AAudioStream *);
int64_t thunk_bifrost_aaudio_claim_callback(AAudioStream *, uint64_t info[2]);
int64_t thunk_bifrost_aaudio_wait_callback(AAudioStream *, int32_t *frames);
int64_t thunk_bifrost_aaudio_finish_callback(AAudioStream *, int32_t result);

struct callback_thread {
    AAudioStream *stream;
    AAudioStream_dataCallback callback;
    void *user;
    pthread_t thread;
    int detached; // closed from its own callback: the thread frees itself
    struct callback_thread *next;
};
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static struct callback_thread *threads;

static void *run_callbacks(void *arg) {
    struct callback_thread *self = arg;
    for (;;) {
        int32_t frames = 0;
        int64_t audio = thunk_bifrost_aaudio_wait_callback(self->stream, &frames);
        if (audio < 0)
            break; // closed
        if (audio == 0)
            continue; // not started, or the ring is full
        int32_t result = self->callback(self->stream, self->user, (void *)(uintptr_t)audio, frames);
        if (thunk_bifrost_aaudio_finish_callback(self->stream, result) < 0)
            break;
    }
    if (self->detached)
        free(self);
    return NULL;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder,
                                               AAudioStream **stream) {
    aaudio_result_t result = thunk_AAudioStreamBuilder_openStream(builder, stream);
    if (result != AAUDIO_OK)
        return result;
    uint64_t info[2];
    int64_t claimed = thunk_bifrost_aaudio_claim_callback(*stream, info);
    if (claimed == 0)
        return AAUDIO_OK; // no data callback: the app calls AAudioStream_write
    struct callback_thread *entry = claimed > 0 ? calloc(1, sizeof(*entry)) : NULL;
    if (entry) {
        entry->stream = *stream;
        entry->callback = (AAudioStream_dataCallback)(uintptr_t)info[0];
        entry->user = (void *)(uintptr_t)info[1];
        if (pthread_create(&entry->thread, NULL, run_callbacks, entry) == 0) {
            pthread_setname_np(entry->thread, "AAudio");
            pthread_mutex_lock(&threads_lock);
            entry->next = threads;
            threads = entry;
            pthread_mutex_unlock(&threads_lock);
            return AAUDIO_OK;
        }
        free(entry);
        result = AAUDIO_ERROR_INTERNAL;
    } else {
        result = claimed < 0 ? (aaudio_result_t)claimed : AAUDIO_ERROR_NO_MEMORY;
    }
    thunk_AAudioStream_close(*stream);
    *stream = NULL;
    return result;
}

aaudio_result_t AAudioStream_close(AAudioStream *stream) {
    pthread_mutex_lock(&threads_lock);
    struct callback_thread **link = &threads;
    while (*link && (*link)->stream != stream)
        link = &(*link)->next;
    struct callback_thread *entry = *link;
    if (entry)
        *link = entry->next;
    pthread_mutex_unlock(&threads_lock);
    // Waits for a running callback, then ends the thread's wait loop.
    aaudio_result_t result = thunk_AAudioStream_close(stream);
    if (entry) {
        if (pthread_equal(entry->thread, pthread_self())) {
            entry->detached = 1;
            pthread_detach(entry->thread);
        } else {
            pthread_join(entry->thread, NULL);
            free(entry);
        }
    }
    return result;
}
