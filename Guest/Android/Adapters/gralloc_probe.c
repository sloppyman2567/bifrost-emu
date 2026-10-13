#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include <stdint.h>
enum AIMapper_Error : int32_t;
enum AIMapper_Version : uint32_t;
typedef enum AIMapper_Error AIMapper_Error;
typedef enum AIMapper_Version AIMapper_Version;
#include <android/hardware/graphics/mapper/IMapper.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

// Real libui/Gralloc5, NDK parcel/fd transfer and BufferQueue clients. This is
// intentionally independent of the allocator's private descriptor/layout code.
JNIEXPORT jint JNICALL Java_ActivityProbe_00024GrallocProbe_check(JNIEnv *env, jclass type) {
    (void)type;
    void *android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!android) { fprintf(stderr, "gralloc probe: %s\n", dlerror()); return __LINE__; }
    void *media = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    if (!media) { fprintf(stderr, "gralloc probe: %s\n", dlerror()); return __LINE__; }
#define FN(lib, result, name, args) result (*name) args = (result (*) args)dlsym(lib, #name); if (!name) return __LINE__
    FN(android, int, AHardwareBuffer_allocate, (const AHardwareBuffer_Desc *, AHardwareBuffer **));
    FN(android, int, AHardwareBuffer_isSupported, (const AHardwareBuffer_Desc *));
    FN(android, void, AHardwareBuffer_describe, (const AHardwareBuffer *, AHardwareBuffer_Desc *));
    FN(android, void, AHardwareBuffer_release, (AHardwareBuffer *));
    FN(android, int, AHardwareBuffer_lock, (AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **));
    FN(android, int, AHardwareBuffer_lockPlanes, (AHardwareBuffer *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *));
    FN(android, int, AHardwareBuffer_unlock, (AHardwareBuffer *, int32_t *));
    FN(android, int, AHardwareBuffer_sendHandleToUnixSocket, (const AHardwareBuffer *, int));
    FN(android, int, AHardwareBuffer_recvHandleFromUnixSocket, (int, AHardwareBuffer **));
    FN(android, int, AHardwareBuffer_getId, (const AHardwareBuffer *, uint64_t *));
    const uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    const uint32_t formats[] = {1, 2, 3, 4, 5, 22, 43, 56, 33, 35, 0x32315659};
    for (unsigned i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i) {
        AHardwareBuffer_Desc desc = {.width = 32, .height = formats[i] == 33 ? 1 : 18,
            .layers = 1, .format = formats[i], .usage = usage};
        AHardwareBuffer *buffer = NULL, *alias = NULL;
        int sockets[2];
        int result = 0;
#define CHECK(x) do { if (!(x)) { result = __LINE__; goto cleanup; } } while (0)
        sockets[0] = sockets[1] = -1;
        CHECK(AHardwareBuffer_isSupported(&desc) == 1);
        CHECK(AHardwareBuffer_allocate(&desc, &buffer) == 0 && buffer);
        AHardwareBuffer_Desc actual = {0};
        AHardwareBuffer_describe(buffer, &actual);
        CHECK(actual.width == desc.width && actual.height == desc.height && actual.layers == 1
                && actual.format == desc.format && actual.stride >= desc.width);
        CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
        CHECK(AHardwareBuffer_sendHandleToUnixSocket(buffer, sockets[0]) == 0);
        CHECK(AHardwareBuffer_recvHandleFromUnixSocket(sockets[1], &alias) == 0 && alias);
        uint64_t first_id = 0, alias_id = 0;
        CHECK(AHardwareBuffer_getId(buffer, &first_id) == 0
                && AHardwareBuffer_getId(alias, &alias_id) == 0 && first_id == alias_id);
        void *pixels = NULL;
        CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &pixels) == 0 && pixels);
        ((unsigned char *)pixels)[0] = 0x4d;
        CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);
        AHardwareBuffer_release(buffer); buffer = NULL;
        CHECK(AHardwareBuffer_lock(alias, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &pixels) == 0 && pixels);
        CHECK(((unsigned char *)pixels)[0] == 0x4d);
        CHECK(AHardwareBuffer_unlock(alias, NULL) == 0);
        if (desc.format == 35 || desc.format == 0x32315659) {
            AHardwareBuffer_Planes planes = {0};
            CHECK(AHardwareBuffer_lockPlanes(alias, usage, -1, NULL, &planes) == 0);
            CHECK(planes.planeCount == 3 && planes.planes[0].rowStride == actual.stride
                    && planes.planes[1].pixelStride == 1 && planes.planes[2].pixelStride == 1);
            memset(planes.planes[1].data, 0x61, planes.planes[1].rowStride * desc.height / 2);
            memset(planes.planes[2].data, 0x72, planes.planes[2].rowStride * desc.height / 2);
            CHECK(AHardwareBuffer_unlock(alias, NULL) == 0);
        }
cleanup:
        if (alias) AHardwareBuffer_release(alias);
        if (buffer) AHardwareBuffer_release(buffer);
        if (sockets[0] >= 0) close(sockets[0]);
        if (sockets[1] >= 0) close(sockets[1]);
        if (result) return result;
#undef CHECK
    }
    AHardwareBuffer_Desc rejected = {.width = 32, .height = 32, .layers = 1, .format = 1,
        .usage = AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
    if (AHardwareBuffer_isSupported(&rejected) != 0) return __LINE__;

    void *mapper_lib = dlopen("mapper.bifrost.so", RTLD_NOW | RTLD_LOCAL);
    if (!mapper_lib) return __LINE__;
    FN(mapper_lib, AIMapper_Error, AIMapper_loadIMapper, (AIMapper **));
    AIMapper *mapper = NULL;
    if (AIMapper_loadIMapper(&mapper) != AIMAPPER_ERROR_NONE || !mapper || mapper->version != AIMAPPER_VERSION_5)
        return __LINE__;
    jclass probe = (*env)->FindClass(env, "ActivityProbe$GrallocProbe");
    jmethodID allocate = (*env)->GetStaticMethodID(env, probe, "allocateReserved", "()[I");
    jintArray values = (jintArray)(*env)->CallStaticObjectMethod(env, probe, allocate);
    if ((*env)->ExceptionCheck(env) || !values || (*env)->GetArrayLength(env, values) != 13) return __LINE__;
    int storage[16] = {sizeof(native_handle_t), 1, 12};
    (*env)->GetIntArrayRegion(env, values, 0, 13, storage + 3);
    buffer_handle_t imported = NULL, duplicate = NULL;
    int status = mapper->v5.importBuffer((native_handle_t *)storage, &imported);
    close(storage[3]);
    if (status != AIMAPPER_ERROR_NONE || !imported) return __LINE__;
    int extra_result = 0;
#define EXTRA(x) do { if (!(x)) { extra_result = __LINE__; goto extra_cleanup; } } while (0)
    void *reserved = NULL, *duplicate_reserved = NULL;
    uint64_t reserved_size = 0;
    EXTRA(mapper->v5.getReservedRegion(imported, &reserved, &reserved_size) == AIMAPPER_ERROR_NONE);
    EXTRA(reserved_size == 64 && reserved && ((uintptr_t)reserved & 7) == 0);
    memset(reserved, 0xa5, reserved_size);
    EXTRA(mapper->v5.importBuffer(imported, &duplicate) == AIMAPPER_ERROR_NONE);
    EXTRA(mapper->v5.getReservedRegion(duplicate, &duplicate_reserved, &reserved_size) == AIMAPPER_ERROR_NONE);
    EXTRA(reserved_size == 64 && duplicate_reserved != reserved && ((unsigned char *)duplicate_reserved)[63] == 0xa5);
    unsigned char metadata[256];
    int32_t encoded = mapper->v5.getStandardMetadata(imported, 17, metadata, sizeof(metadata));
    EXTRA(encoded > 4 && encoded <= (int32_t)sizeof(metadata));
    int32_t dataspace = 0x8810000;
    memcpy(metadata + encoded - 4, &dataspace, 4);
    EXTRA(mapper->v5.setStandardMetadata(imported, 17, metadata, encoded) == AIMAPPER_ERROR_NONE);
    EXTRA(mapper->v5.getStandardMetadata(duplicate, 17, metadata, sizeof(metadata)) == encoded);
    int32_t observed;
    memcpy(&observed, metadata + encoded - 4, 4);
    EXTRA(observed == dataspace);
    EXTRA(mapper->v5.setStandardMetadata(imported, 17, metadata, encoded - 1) == AIMAPPER_ERROR_UNSUPPORTED);
    EXTRA(mapper->v5.getStandardMetadata(imported, 999, NULL, 0) == -AIMAPPER_ERROR_UNSUPPORTED);
    uint32_t fd_count = 0, int_count = 0;
    EXTRA(mapper->v5.getTransportSize(imported, &fd_count, &int_count) == AIMAPPER_ERROR_NONE && fd_count == 1 && int_count == 12);
    void *invalid_lock = NULL;
    EXTRA(mapper->v5.lock(imported, 0x103, (ARect){0, 0, 1, 1}, -1, &invalid_lock) == AIMAPPER_ERROR_BAD_VALUE);
    int pending_fence = eventfd(0, EFD_CLOEXEC);
    EXTRA(pending_fence >= 0);
    EXTRA(mapper->v5.lock(imported, 0x103, (ARect){0, 0, 1, 1}, pending_fence, &invalid_lock) == AIMAPPER_ERROR_BAD_VALUE);
    EXTRA(fcntl(pending_fence, F_GETFD) == -1 && errno == EBADF);
extra_cleanup:
    if (duplicate) mapper->v5.freeBuffer(duplicate);
    mapper->v5.freeBuffer(imported);
    dlclose(mapper_lib);
    if (extra_result) return extra_result;
#undef EXTRA
    rejected.usage = usage; rejected.layers = 2;
    if (AHardwareBuffer_isSupported(&rejected) != 0) return __LINE__;
    rejected.layers = 1; rejected.format = 0x7fffffff;
    if (AHardwareBuffer_isSupported(&rejected) != 0) return __LINE__;

    FN(media, media_status_t, AImageReader_newWithUsage, (int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **));
    FN(media, void, AImageReader_delete, (AImageReader *));
    FN(media, media_status_t, AImageReader_getWindow, (AImageReader *, ANativeWindow **));
    FN(media, media_status_t, AImageReader_acquireLatestImage, (AImageReader *, AImage **));
    FN(media, void, AImage_delete, (AImage *));
    FN(media, media_status_t, AImage_getHardwareBuffer, (const AImage *, AHardwareBuffer **));
    FN(media, media_status_t, AImage_getTimestamp, (const AImage *, int64_t *));
    FN(android, int32_t, ANativeWindow_lock, (ANativeWindow *, ANativeWindow_Buffer *, ARect *));
    FN(android, int32_t, ANativeWindow_unlockAndPost, (ANativeWindow *));
    AImageReader *reader = NULL;
    AImage *image = NULL;
    int result = 0;
#define CHECK(x) do { if (!(x)) { result = __LINE__; goto image_cleanup; } } while (0)
    CHECK(AImageReader_newWithUsage(32, 18, AIMAGE_FORMAT_RGBA_8888, usage, 3, &reader) == AMEDIA_OK && reader);
    ANativeWindow *window = NULL;
    CHECK(AImageReader_getWindow(reader, &window) == AMEDIA_OK && window);
    // Queue multiple frames to check acquireLatest discards the older buffer.
    for (int frame = 1; frame <= 2; ++frame) {
        ANativeWindow_Buffer target = {0};
        CHECK(ANativeWindow_lock(window, &target, NULL) == 0 && target.bits);
        CHECK(target.width == 32 && target.height == 18 && target.format == 1);
        for (int y = 0; y < target.height; ++y)
            for (int x = 0; x < target.width; ++x)
                ((uint32_t *)target.bits)[y * target.stride + x] = 0xff100000 | frame;
        CHECK(ANativeWindow_unlockAndPost(window) == 0);
    }
    CHECK(AImageReader_acquireLatestImage(reader, &image) == AMEDIA_OK && image);
    AHardwareBuffer *received = NULL;
    CHECK(AImage_getHardwareBuffer(image, &received) == AMEDIA_OK && received);
    void *pixels = NULL;
    CHECK(AHardwareBuffer_lock(received, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &pixels) == 0);
    AHardwareBuffer_Desc received_desc;
    AHardwareBuffer_describe(received, &received_desc);
    int correct = 1;
    for (int y = 0; y < 18; ++y)
        for (int x = 0; x < 32; ++x)
            if (((uint32_t *)pixels)[y * received_desc.stride + x] != 0xff100002) correct = 0;
    CHECK(AHardwareBuffer_unlock(received, NULL) == 0 && correct);
    int64_t timestamp = 0;
    CHECK(AImage_getTimestamp(image, &timestamp) == AMEDIA_OK && timestamp > 0);
image_cleanup:
    if (image) AImage_delete(image);
    if (reader) AImageReader_delete(reader);
    dlclose(media); dlclose(android);
    return result;
#undef CHECK
#undef FN
}
