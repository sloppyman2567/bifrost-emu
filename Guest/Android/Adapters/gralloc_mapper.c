// Android 15 stable-C mapper v5 for the software allocator. No host pointers
// or graphics handles cross this boundary: all mapping happens in the guest.
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include <stdint.h>
// The Android 15 stable-C header uses its fixed-width enum names without
// typedefs. Supply the C23 forward typedefs without changing the pinned header.
enum AIMapper_Error : int32_t;
enum AIMapper_Version : uint32_t;
typedef enum AIMapper_Error AIMapper_Error;
typedef enum AIMapper_Version AIMapper_Version;
#include <android/hardware/graphics/mapper/IMapper.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "Bifrost/AndroidGralloc.h"

static const char standard[] = "android.hardware.graphics.common.StandardMetadataType";
static const char component[] = "android.hardware.graphics.common.PlaneLayoutComponentType";
struct imported {
    struct imported *next;
    native_handle_t *handle;
    struct bf_gralloc_header *mapping;
    size_t size;
    uint64_t id;
    int32_t info[BF_GRALLOC_INTS];
    unsigned locks;
    bool writing;
};
static struct imported *buffers;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static struct imported *find(buffer_handle_t handle) {
    for (struct imported *b = buffers; b; b = b->next)
        if (b->handle == handle) return b;
    return NULL;
}
static AIMapper_Error import_buffer(const native_handle_t *raw, buffer_handle_t *out) {
    *out = NULL;
    if (!raw || raw->version != sizeof(native_handle_t) || raw->numFds != 1
            || raw->numInts != BF_GRALLOC_INTS) return AIMAPPER_ERROR_BAD_BUFFER;
    int32_t info[BF_GRALLOC_INTS];
    const int32_t *transport = raw->data + 1;
    if (!bf_gralloc_layout(transport[BF_WIDTH], transport[BF_HEIGHT], 1,
            transport[BF_REQUESTED], bf_gralloc_u64(transport, BF_USAGE_LO),
            bf_gralloc_u64(transport, BF_RESERVED_LO), info)
            || memcmp(info, transport, sizeof(info))) return AIMAPPER_ERROR_BAD_BUFFER;
    uint64_t size = bf_gralloc_file_size(info);
    int fd = fcntl(raw->data[0], F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return AIMAPPER_ERROR_BAD_BUFFER;
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    if (fstat(fd, &st) || (uint64_t)st.st_size != size || seals < 0
            || (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) != (F_SEAL_GROW | F_SEAL_SHRINK)) {
        close(fd); return AIMAPPER_ERROR_BAD_BUFFER;
    }
    struct bf_gralloc_header *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return AIMAPPER_ERROR_NO_RESOURCES; }
    if (memcmp(map->info, info, sizeof(info)) || map->name[127]) {
        munmap(map, size); close(fd); return AIMAPPER_ERROR_BAD_BUFFER;
    }
    struct imported *b = calloc(1, sizeof(*b));
    native_handle_t *handle = malloc(sizeof(*handle) + (BF_GRALLOC_INTS + 1) * sizeof(int));
    if (!b || !handle) {
        free(b); free(handle); munmap(map, size); close(fd); return AIMAPPER_ERROR_NO_RESOURCES;
    }
    handle->version = sizeof(*handle); handle->numFds = 1; handle->numInts = BF_GRALLOC_INTS;
    handle->data[0] = fd;
    memcpy(handle->data + 1, info, sizeof(info));
    memcpy(b->info, info, sizeof(info));
    b->handle = handle; b->mapping = map; b->size = size; b->id = st.st_ino;
    pthread_mutex_lock(&registry);
    b->next = buffers; buffers = b;
    pthread_mutex_unlock(&registry);
    *out = handle;
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error free_buffer(buffer_handle_t handle) {
    pthread_mutex_lock(&registry);
    struct imported **link = &buffers;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (!*link) { pthread_mutex_unlock(&registry); return AIMAPPER_ERROR_BAD_BUFFER; }
    struct imported *b = *link;
    *link = b->next;
    pthread_mutex_unlock(&registry);
    munmap(b->mapping, b->size); close(b->handle->data[0]); free(b->handle); free(b);
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error transport_size(buffer_handle_t handle, uint32_t *fds, uint32_t *ints) {
    pthread_mutex_lock(&registry);
    bool valid = find(handle) != NULL;
    pthread_mutex_unlock(&registry);
    if (!valid) return AIMAPPER_ERROR_BAD_BUFFER;
    *fds = 1; *ints = BF_GRALLOC_INTS;
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error validate_lock(struct imported *b, uint64_t usage, ARect rect) {
    if (!b) return AIMAPPER_ERROR_BAD_BUFFER;
    uint64_t allocated = bf_gralloc_u64(b->info, BF_USAGE_LO);
    unsigned read = usage & 15, write = usage & 240;
    if (!usage || (usage & ~UINT64_C(0xff)) || (read && read != 2 && read != 3)
            || (write && write != 32 && write != 48)
            || (read && !(allocated & 15)) || (write && !(allocated & 240))
            || rect.left < 0 || rect.top < 0 || rect.right < rect.left || rect.bottom < rect.top
            || rect.right > b->info[BF_WIDTH] || rect.bottom > b->info[BF_HEIGHT])
        return AIMAPPER_ERROR_BAD_VALUE;
    if (b->info[BF_FORMAT] != 33 && (b->writing || (write && b->locks)))
        return AIMAPPER_ERROR_NO_RESOURCES;
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error lock_buffer(buffer_handle_t handle, uint64_t usage, ARect rect,
        int fence, void **out) {
    *out = NULL;
    pthread_mutex_lock(&registry);
    AIMapper_Error error = validate_lock(find(handle), usage, rect);
    pthread_mutex_unlock(&registry);
    // Invalid requests must close their fence without waiting on it. Valid
    // waits happen outside the registry mutex so unrelated buffers can proceed.
    if (error) { if (fence >= 0) close(fence); return error; }
    if (fence >= 0) {
        struct pollfd p = {fence, POLLIN, 0};
        int status;
        do { status = poll(&p, 1, -1); } while (status < 0 && errno == EINTR);
        close(fence);
        if (status <= 0 || !(p.revents & POLLIN)) return AIMAPPER_ERROR_BAD_VALUE;
    }
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    error = validate_lock(b, usage, rect);
    if (!error) {
        ++b->locks; b->writing = (usage & 240) != 0;
        *out = (char *)b->mapping + BF_GRALLOC_OFFSET;
    }
    pthread_mutex_unlock(&registry);
    return error;
}
static AIMapper_Error unlock_buffer(buffer_handle_t handle, int *fence) {
    *fence = -1; // coherent CPU memory has no asynchronous release work
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    AIMapper_Error error = b && b->locks ? AIMAPPER_ERROR_NONE : AIMAPPER_ERROR_BAD_BUFFER;
    if (!error) { --b->locks; b->writing = false; }
    pthread_mutex_unlock(&registry);
    return error;
}
static AIMapper_Error locked_buffer(buffer_handle_t handle) {
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    AIMapper_Error error = b && b->locks ? AIMAPPER_ERROR_NONE : AIMAPPER_ERROR_BAD_BUFFER;
    pthread_mutex_unlock(&registry);
    return error;
}

struct encoding { unsigned char bytes[2048]; size_t size; };
static void bytes(struct encoding *e, const void *p, size_t size) {
    if (size <= sizeof(e->bytes) - e->size) { memcpy(e->bytes + e->size, p, size); e->size += size; }
}
static void i64(struct encoding *e, int64_t v) { bytes(e, &v, sizeof(v)); }
static void i32(struct encoding *e, int32_t v) { bytes(e, &v, sizeof(v)); }
static void string(struct encoding *e, const char *s) { size_t n = strlen(s); i64(e, n); bytes(e, s, n); }
static void extendable(struct encoding *e, const char *name, int64_t value) { string(e, name); i64(e, value); }
static void header(struct encoding *e, int64_t type) { extendable(e, standard, type); }
#define FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
static uint32_t fourcc(int format) {
    switch (format) {
    case 1: return FOURCC('A','B','2','4');
    case 2: return FOURCC('X','B','2','4');
    case 3: return FOURCC('B','G','2','4');
    case 4: return FOURCC('R','G','1','6');
    case 5: return FOURCC('A','R','2','4');
    case 22: return FOURCC('A','B','4','H');
    case 43: return FOURCC('A','B','3','0');
    case 35: case 0x32315659: return FOURCC('Y','V','1','2');
    case 56: case 33: return FOURCC('R','8',' ',' ');
    default: return 0;
    }
}
static void plane(struct encoding *e, int count, const int64_t *types, const int *offsets,
        const int *bits, int64_t offset, int step, int stride, int w, int h, int subsample) {
    i64(e, count);
    for (int i = 0; i < count; ++i) {
        extendable(e, component, types[i]); i64(e, offsets[i]); i64(e, bits[i]);
    }
    i64(e, offset); i64(e, step); i64(e, stride); i64(e, w); i64(e, h);
    i64(e, (int64_t)stride * h); i64(e, subsample); i64(e, subsample);
}
static void planes(struct encoding *e, const int32_t *info) {
    int format = info[BF_FORMAT], w = info[BF_WIDTH], h = info[BF_HEIGHT], stride = info[BF_STRIDE];
    if (format == 35 || format == 0x32315659) {
        int cstride = ((stride / 2) + 15) & ~15;
        int64_t y = 1, cr = 4, cb = 2;
        int zero = 0, eight = 8;
        i64(e, 3);
        plane(e, 1, &y, &zero, &eight, 0, 8, stride, w, h, 1);
        plane(e, 1, &cr, &zero, &eight, (int64_t)stride*h, 8, cstride, w/2, h/2, 2);
        plane(e, 1, &cb, &zero, &eight, (int64_t)stride*h + (int64_t)cstride*h/2,
                8, cstride, w/2, h/2, 2);
    } else {
        int bpp = bf_gralloc_bpp(format), count = format == 33 || format == 56 ? 1 : 3;
        int64_t types[] = {1024, 2048, 4096, 1LL<<30};
        int offsets[] = {0, 8, 16, 24}, bits[] = {8, 8, 8, 8};
        if (format == 1 || format == 5 || format == 22 || format == 43) count = 4;
        if (format == 5) { offsets[0] = 16; offsets[2] = 0; }
        if (format == 4) {
            offsets[0] = 11; offsets[1] = 5; offsets[2] = 0;
            bits[0] = 5; bits[1] = 6; bits[2] = 5;
        }
        if (format == 22 || format == 43) for (int i = 0; i < 4; ++i) {
            bits[i] = format == 22 ? 16 : (i == 3 ? 2 : 10);
            offsets[i] = i * (format == 22 ? 16 : 10);
        }
        if (format == 33) types[0] = 1LL<<20;
        i64(e, 1);
        plane(e, count, types, offsets, bits, 0, bpp*8, stride*bpp, w, h, 1);
    }
}
static int32_t encode(struct imported *b, int64_t type, struct encoding *e) {
    const int32_t *info = b->info;
    if (type < 1 || type > 23) return -AIMAPPER_ERROR_UNSUPPORTED;
    if (type >= 17 && type <= 22) {
        int slot = type - 17;
        uint32_t size = 0;
        bool copied = false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            uint32_t sequence = __atomic_load_n(&b->mapping->sequences[slot], __ATOMIC_ACQUIRE);
            if (sequence & 1) continue;
            size = __atomic_load_n(&b->mapping->lengths[slot], __ATOMIC_RELAXED);
            if (size > sizeof(b->mapping->metadata[slot])) return -AIMAPPER_ERROR_BAD_BUFFER;
            e->size = 0;
            for (uint32_t i = 0; i < size; ++i)
                e->bytes[i] = __atomic_load_n(&b->mapping->metadata[slot][i], __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (sequence == __atomic_load_n(&b->mapping->sequences[slot], __ATOMIC_RELAXED)) {
                e->size = size; copied = true; break;
            }
        }
        if (!copied) return -AIMAPPER_ERROR_NO_RESOURCES;
        if (!size && type <= 18) { header(e, type); i32(e, 0); }
        return e->size;
    }
    header(e, type);
    switch (type) {
    case 1: i64(e, b->id); break;
    case 2: {
        char name[128]; memcpy(name, b->mapping->name, sizeof(name)); name[127] = 0;
        string(e, name); break;
    }
    case 3: i64(e, info[BF_WIDTH]); break;
    case 4: i64(e, info[BF_HEIGHT]); break;
    case 5: i64(e, 1); break;
    case 6: i32(e, info[BF_REQUESTED]); break;
    case 7: i32(e, fourcc(info[BF_FORMAT])); break;
    case 8: i64(e, 0); break; // DRM_FORMAT_MOD_LINEAR
    case 9: i64(e, bf_gralloc_u64(info, BF_USAGE_LO)); break;
    case 10: i64(e, b->size); break;
    case 11: i64(e, 0); break;
    case 12: extendable(e, "android.hardware.graphics.common.Compression", 0); break;
    case 13: extendable(e, "android.hardware.graphics.common.Interlaced", 0); break;
    case 14: extendable(e, "android.hardware.graphics.common.ChromaSiting",
            info[BF_FORMAT] == 35 || info[BF_FORMAT] == 0x32315659 ? 1 : 0); break;
    case 15: planes(e, info); break;
    case 16: {
        int count = info[BF_FORMAT] == 35 || info[BF_FORMAT] == 0x32315659 ? 3 : 1;
        i64(e, count);
        for (int i = 0; i < count; ++i) {
            i32(e, 0); i32(e, 0); i32(e, info[BF_WIDTH] / (i ? 2 : 1));
            i32(e, info[BF_HEIGHT] / (i ? 2 : 1));
        }
        break;
    }
    case 23: i32(e, info[BF_STRIDE]); break;
    }
    return e->size;
}
static int32_t get_standard(buffer_handle_t handle, int64_t type, void *dest, size_t capacity) {
    struct encoding e = {0};
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    int32_t size = b ? encode(b, type, &e) : -AIMAPPER_ERROR_BAD_BUFFER;
    pthread_mutex_unlock(&registry);
    if (size > 0 && dest && capacity >= (size_t)size) memcpy(dest, e.bytes, size);
    return size;
}
static int32_t get_metadata(buffer_handle_t handle, AIMapper_MetadataType type, void *dest, size_t size) {
    if (strcmp(type.name, standard)) return -AIMAPPER_ERROR_UNSUPPORTED;
    return get_standard(handle, type.value, dest, size);
}
static AIMapper_Error set_standard(buffer_handle_t handle, int64_t type, const void *data, size_t size) {
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    AIMapper_Error error = AIMAPPER_ERROR_NONE;
    struct encoding expected = {0};
    header(&expected, type);
    if (!b) error = AIMAPPER_ERROR_BAD_BUFFER;
    else if (type >= 1 && type <= 6) error = AIMAPPER_ERROR_BAD_VALUE;
    else if (type == 9) error = AIMAPPER_ERROR_BAD_VALUE;
    else if (type < 17 || type > 22) error = AIMAPPER_ERROR_UNSUPPORTED;
    else if (size > 512 || (size && (size < expected.size || !data
            || memcmp(data, expected.bytes, expected.size)))) error = AIMAPPER_ERROR_UNSUPPORTED;
    else {
        size_t payload = size ? size - expected.size : 0;
        bool valid = type <= 18 ? size && payload == 4 : !size;
        if (type == 19 && size) valid = payload == 40;
        if (type == 20 && size) valid = payload == 8;
        if ((type == 21 || type == 22) && size && payload >= 8) {
            int64_t length; memcpy(&length, (const char *)data + expected.size, 8);
            valid = length >= 0 && (uint64_t)length == payload - 8;
        }
        if (!valid) error = AIMAPPER_ERROR_UNSUPPORTED;
        else {
            int slot = type - 17;
            uint32_t sequence = __atomic_load_n(&b->mapping->sequences[slot], __ATOMIC_ACQUIRE);
            if ((sequence & 1) || !__atomic_compare_exchange_n(&b->mapping->sequences[slot],
                    &sequence, sequence + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                error = AIMAPPER_ERROR_NO_RESOURCES;
            } else {
                for (size_t i = 0; i < size; ++i)
                    __atomic_store_n(&b->mapping->metadata[slot][i], ((const unsigned char *)data)[i], __ATOMIC_RELAXED);
                __atomic_store_n(&b->mapping->lengths[slot], size, __ATOMIC_RELAXED);
                __atomic_store_n(&b->mapping->sequences[slot], sequence + 2, __ATOMIC_RELEASE);
            }
        }
    }
    pthread_mutex_unlock(&registry);
    return error;
}
static AIMapper_Error set_metadata(buffer_handle_t handle, AIMapper_MetadataType type,
        const void *data, size_t size) {
    if (strcmp(type.name, standard)) return AIMAPPER_ERROR_UNSUPPORTED;
    return set_standard(handle, type.value, data, size);
}
static AIMapper_MetadataTypeDescription descriptions[23];
static pthread_once_t descriptions_once = PTHREAD_ONCE_INIT;
static void init_descriptions(void) {
    for (int i = 0; i < 23; ++i) {
        descriptions[i].metadataType = (AIMapper_MetadataType){standard, i + 1};
        descriptions[i].isGettable = true;
        descriptions[i].isSettable = i >= 16 && i <= 21;
    }
}
static AIMapper_Error list_metadata(const AIMapper_MetadataTypeDescription **out, size_t *count) {
    pthread_once(&descriptions_once, init_descriptions);
    *out = descriptions; *count = 23;
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error dump_buffer(buffer_handle_t handle, AIMapper_DumpBufferCallback callback, void *context) {
    for (int64_t type = 1; type <= 23; ++type) {
        unsigned char data[2048];
        int32_t size = get_standard(handle, type, data, sizeof(data));
        if (size < 0) return (AIMapper_Error)-size;
        callback(context, (AIMapper_MetadataType){standard, type}, data, size);
    }
    return AIMAPPER_ERROR_NONE;
}
static AIMapper_Error dump_all(AIMapper_BeginDumpBufferCallback begin,
        AIMapper_DumpBufferCallback callback, void *context) {
    // Snapshot imported handles under the registry lock; clone imports keep
    // mappings alive while callbacks execute without our registry mutex held.
    pthread_mutex_lock(&registry);
    size_t count = 0;
    for (struct imported *b = buffers; b; b = b->next) ++count;
    native_handle_t **snapshot = calloc(count ? count : 1, sizeof(*snapshot));
    if (!snapshot) { pthread_mutex_unlock(&registry); return AIMAPPER_ERROR_NO_RESOURCES; }
    size_t index = 0;
    for (struct imported *b = buffers; b; b = b->next) {
        size_t size = sizeof(native_handle_t) + (BF_GRALLOC_INTS + 1) * sizeof(int);
        snapshot[index] = malloc(size);
        if (!snapshot[index]) break;
        memcpy(snapshot[index], b->handle, size);
        snapshot[index]->data[0] = fcntl(b->handle->data[0], F_DUPFD_CLOEXEC, 0);
        if (snapshot[index]->data[0] < 0) { free(snapshot[index]); snapshot[index] = NULL; break; }
        ++index;
    }
    pthread_mutex_unlock(&registry);
    AIMapper_Error error = index == count ? AIMAPPER_ERROR_NONE : AIMAPPER_ERROR_NO_RESOURCES;
    for (size_t i = 0; i < index; ++i) {
        buffer_handle_t handle;
        AIMapper_Error status = import_buffer(snapshot[i], &handle);
        close(snapshot[i]->data[0]); free(snapshot[i]);
        if (!status) { begin(context); status = dump_buffer(handle, callback, context); free_buffer(handle); }
        if (status) error = status;
    }
    free(snapshot);
    return error;
}
static AIMapper_Error reserved_region(buffer_handle_t handle, void **out, uint64_t *size) {
    pthread_mutex_lock(&registry);
    struct imported *b = find(handle);
    AIMapper_Error error = b ? AIMAPPER_ERROR_NONE : AIMAPPER_ERROR_BAD_BUFFER;
    if (b) {
        *size = bf_gralloc_u64(b->info, BF_RESERVED_LO);
        *out = *size ? (char *)b->mapping + bf_gralloc_reserved_offset(b->info) : NULL;
    }
    pthread_mutex_unlock(&registry);
    return error;
}
static AIMapper mapper = {.version = AIMAPPER_VERSION_5, .v5 = {
    .importBuffer = import_buffer, .freeBuffer = free_buffer, .getTransportSize = transport_size,
    .lock = lock_buffer, .unlock = unlock_buffer, .flushLockedBuffer = locked_buffer,
    .rereadLockedBuffer = locked_buffer, .getMetadata = get_metadata, .getStandardMetadata = get_standard,
    .setMetadata = set_metadata, .setStandardMetadata = set_standard,
    .listSupportedMetadataTypes = list_metadata, .dumpBuffer = dump_buffer, .dumpAllBuffers = dump_all,
    .getReservedRegion = reserved_region,
}};
AIMapper_Error AIMapper_loadIMapper(AIMapper **out) { *out = &mapper; return AIMAPPER_ERROR_NONE; }
