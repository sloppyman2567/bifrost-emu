#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct surface_identity { jweak object; uint64_t id; struct surface_identity *next; };
static struct surface_identity *surfaces;
static uint64_t next_id;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *thunk_ANativeWindow_fromSurface(JNIEnv *, void *);

void *ANativeWindow_fromSurface(JNIEnv *env, jobject object) {
    if (!env || !object) return NULL;
    pthread_mutex_lock(&lock);
    struct surface_identity **link = &surfaces;
    while (*link) {
        struct surface_identity *entry = *link;
        if ((*env)->IsSameObject(env, entry->object, NULL)) {
            *link = entry->next;
            (*env)->DeleteWeakGlobalRef(env, entry->object);
            free(entry);
            continue;
        }
        if ((*env)->IsSameObject(env, entry->object, object)) {
            uint64_t id = entry->id;
            pthread_mutex_unlock(&lock);
            return thunk_ANativeWindow_fromSurface(env, (void *)(uintptr_t)id);
        }
        link = &entry->next;
    }
    struct surface_identity *entry = calloc(1, sizeof(*entry));
    if (!entry) { pthread_mutex_unlock(&lock); return NULL; }
    entry->object = (*env)->NewWeakGlobalRef(env, object);
    if (!entry->object) { free(entry); pthread_mutex_unlock(&lock); return NULL; }
    entry->id = ++next_id;
    entry->next = surfaces;
    surfaces = entry;
    uint64_t id = entry->id;
    pthread_mutex_unlock(&lock);
    return thunk_ANativeWindow_fromSurface(env, (void *)(uintptr_t)id);
}
