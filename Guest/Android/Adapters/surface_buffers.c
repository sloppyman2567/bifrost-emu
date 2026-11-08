// The CPU view of a frame that a SurfaceView's queue hands out (FrameworkSurfaces.lockBuffer).
//
// The queue's ImageReader is opaque (ImageFormat.PRIVATE): it checks nothing about a buffer's format, so a codec's YUV and
// a GPU or CPU producer's RGBA both reach it, and its Images have no planes. The frame is read from the Image's
// HardwareBuffer instead: AHardwareBuffer_lockPlanes locks it for CPU reads (the same GraphicBuffer::lockAsyncYCbCr or
// lockAsync that a non-opaque reader's CpuConsumer uses) and describes where each plane is. Y, U and V come in that order
// for YUV, one plane for everything else; the strides are in bytes.
#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

// The HAL formats read here. A CPU producer that names no format gets the queue's default, IMPLEMENTATION_DEFINED, which
// this allocator makes linear RGBA (Include/Bifrost/AndroidGralloc.h).
enum {
    FORMAT_RGBA_8888 = 1,
    FORMAT_IMPLEMENTATION_DEFINED = 0x22,
    FORMAT_YCBCR_420_888 = 0x23,
    FORMAT_YV12 = 0x32315659,
};

// FrameworkSurfaces.lockBuffer's result array: format, width, height, plane count, then per plane the address, row stride,
// pixel stride and the bytes readable from the address.
enum { VALUES_PER_PLANE = 4, FIRST_PLANE = 4, RESULT_LENGTH = FIRST_PLANE + 3 * VALUES_PER_PLANE };

static struct {
    AHardwareBuffer *(*from_java)(JNIEnv *, jobject);
    void (*describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    int (*lock_planes)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *);
    int (*unlock)(AHardwareBuffer *, int32_t *);
    int ready;
} api;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;

// The NDK exports live in libandroid.so and libnativewindow.so; both are public libraries of every app.
static void *symbol(void *first, void *second, const char *name) {
    void *address = first ? dlsym(first, name) : NULL;
    return address || !second ? address : dlsym(second, name);
}

static void load_api(void) {
    void *android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    void *window = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    api.from_java = (AHardwareBuffer * (*)(JNIEnv *, jobject)) symbol(android, window, "AHardwareBuffer_fromHardwareBuffer");
    api.describe = (void (*)(const AHardwareBuffer *, AHardwareBuffer_Desc *)) symbol(window, android, "AHardwareBuffer_describe");
    api.lock_planes = (int (*)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *))
            symbol(window, android, "AHardwareBuffer_lockPlanes");
    api.unlock = (int (*)(AHardwareBuffer *, int32_t *)) symbol(window, android, "AHardwareBuffer_unlock");
    api.ready = api.from_java && api.describe && api.lock_planes && api.unlock;
}

// Bytes from a plane's first sample through its last: `rows` rows of `columns` samples of `sample` bytes each.
static int64_t plane_size(int64_t rows, int64_t columns, int64_t sample, int64_t row_stride, int64_t pixel_stride) {
    return row_stride * (rows - 1) + pixel_stride * (columns - 1) + sample;
}

// Locks the Image's buffer for CPU reads and fills `out`. 0, or a negative errno: -ENOTSUP for a format that is not
// RGBA or YUV 4:2:0 (nothing is locked then), -EINVAL for geometry that cannot be right. A buffer that was locked stays
// locked until unlockBuffer, and a failure leaves none locked. The producer's acquire fence is the caller's to wait for.
JNIEXPORT jint JNICALL Java_FrameworkSurfaces_lockBuffer(JNIEnv *env, jclass type, jobject hardware_buffer, jlongArray out) {
    (void)type;
    if (!hardware_buffer || !out || (*env)->GetArrayLength(env, out) != RESULT_LENGTH) return -EINVAL;
    pthread_once(&api_once, load_api);
    if (!api.ready) return -ENOSYS;
    AHardwareBuffer *buffer = api.from_java(env, hardware_buffer);
    if (!buffer) return -EINVAL;
    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof desc);
    api.describe(buffer, &desc);
    int yuv = desc.format == FORMAT_YCBCR_420_888 || desc.format == FORMAT_YV12;
    if (!yuv && desc.format != FORMAT_RGBA_8888 && desc.format != FORMAT_IMPLEMENTATION_DEFINED) return -ENOTSUP;
    if (desc.layers != 1 || !desc.width || !desc.height || desc.width > 16384 || desc.height > 16384) return -EINVAL;
    AHardwareBuffer_Planes planes;
    memset(&planes, 0, sizeof planes);
    int result = api.lock_planes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &planes);
    if (result) return result < 0 ? result : -result;
    uint32_t count = yuv ? 3 : 1;
    jlong values[RESULT_LENGTH];
    memset(values, 0, sizeof values);
    values[0] = desc.format; values[1] = desc.width; values[2] = desc.height; values[3] = count;
    int ok = planes.planeCount == count;
    for (uint32_t i = 0; ok && i < count; ++i) {
        // Chroma is subsampled by two in both directions, rounded up; RGBA is one 4-byte sample per pixel.
        int64_t columns = i && yuv ? ((int64_t)desc.width + 1) / 2 : desc.width;
        int64_t rows = i && yuv ? ((int64_t)desc.height + 1) / 2 : desc.height;
        AHardwareBuffer_Plane *plane = &planes.planes[i];
        ok = plane->data && plane->rowStride && plane->pixelStride && (yuv || plane->pixelStride == 4)
                && (int64_t)plane->pixelStride * columns <= (int64_t)plane->rowStride + plane->pixelStride;
        if (!ok) break;
        values[FIRST_PLANE + i * VALUES_PER_PLANE] = (jlong)(intptr_t)plane->data;
        values[FIRST_PLANE + i * VALUES_PER_PLANE + 1] = plane->rowStride;
        values[FIRST_PLANE + i * VALUES_PER_PLANE + 2] = plane->pixelStride;
        values[FIRST_PLANE + i * VALUES_PER_PLANE + 3] = plane_size(rows, columns, yuv ? 1 : 4, plane->rowStride, plane->pixelStride);
    }
    if (!ok) {
        api.unlock(buffer, NULL);
        return -EINVAL;
    }
    (*env)->SetLongArrayRegion(env, out, 0, RESULT_LENGTH, values);
    return 0;
}

JNIEXPORT void JNICALL Java_FrameworkSurfaces_unlockBuffer(JNIEnv *env, jclass type, jobject hardware_buffer) {
    (void)type;
    pthread_once(&api_once, load_api);
    if (!api.ready || !hardware_buffer) return;
    AHardwareBuffer *buffer = api.from_java(env, hardware_buffer);
    if (buffer) api.unlock(buffer, NULL);
}
