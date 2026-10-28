// AndroidBufferImages.cpp — see AndroidBufferImages.hpp. Contracts:
// EGL_ANDROID_image_native_buffer (an image of an ANativeWindowBuffer, whose
// handle is a gralloc native_handle), EGL_KHR_gl_texture_2D_image (the host
// image's source) and OES_EGL_image(_external) (binding it to a texture).
#include "System/Graphics/Thunks/GraphicThunk/AndroidBufferImages.hpp"
#include "System/Android/Surface/Surface.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Graphics/Thunks/ThunkCommon.hpp"
#include "System/Memory/Memory.hpp"
#include "Bifrost/AndroidGralloc.h"
#include "Bifrost/Types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
namespace arm64emu {
namespace {
constexpr uint64_t kImageTag = 0xB1F0000000000000ull;  // never a host pointer
constexpr unsigned kNativeBufferTarget = 0x3140;      // EGL_NATIVE_BUFFER_ANDROID
constexpr int32_t kBufferMagic = ('_' << 24) | ('b' << 16) | ('f' << 8) | 'r';
constexpr int32_t kBufferVersion = 112;  // sizeof(ANativeWindowBuffer) on arm64
// EGL / GLES constants.
constexpr int kNone = 0x3038, kImagePreserved = 0x30D2, kProtectedContent = 0x32C0;
constexpr int kTexture2DImage = 0x30B1, kTextureLevel = 0x30BC, kTrue = 1;
constexpr unsigned kTexture2D = 0x0DE1, kTextureBinding2D = 0x8069, kRgba = 0x1908,
                   kUnsignedByte = 0x1401, kMinFilter = 0x2801, kMagFilter = 0x2800,
                   kLinear = 0x2601, kWrapS = 0x2802, kWrapT = 0x2803, kClampToEdge = 0x812F,
                   kUnpackAlignment = 0x0CF5, kUnpackRowLength = 0x0CF2, kUnpackSkipRows = 0x0CF3,
                   kUnpackSkipPixels = 0x0CF4, kUnpackBuffer = 0x88EC, kUnpackBufferBinding = 0x88EF,
                   kVersion = 0x1F02;

struct HostFns {
    void* (*get_current_context)() = nullptr;
    void* (*get_current_display)() = nullptr;
    void* (*create_image)(void*, void*, unsigned, void*, const int*) = nullptr;
    unsigned (*destroy_image)(void*, void*) = nullptr;
    void (*target_texture)(unsigned, void*) = nullptr;
    void (*target_renderbuffer)(unsigned, void*) = nullptr;
    void (*gen_textures)(int, uint32_t*) = nullptr;
    void (*delete_textures)(int, const uint32_t*) = nullptr;
    void (*bind_texture)(unsigned, uint32_t) = nullptr;
    void (*bind_buffer)(unsigned, uint32_t) = nullptr;
    void (*tex_parameteri)(unsigned, unsigned, int) = nullptr;
    void (*tex_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
    void (*tex_sub_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
    void (*pixel_store)(unsigned, int) = nullptr;
    void (*get_integer)(unsigned, int*) = nullptr;
    const unsigned char* (*get_string)(unsigned) = nullptr;
    bool usable = false;
};
const HostFns& host() {
    static const HostFns fns = [] {
        HostFns f;
        auto core = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
        using Proc = void* (*)(const char*);
        auto proc = reinterpret_cast<Proc>(core("eglGetProcAddress"));
        auto ext = [&](const char* name) { return proc ? proc(name) : nullptr; };
        f.get_current_context = reinterpret_cast<decltype(f.get_current_context)>(core("eglGetCurrentContext"));
        f.get_current_display = reinterpret_cast<decltype(f.get_current_display)>(core("eglGetCurrentDisplay"));
        f.create_image = reinterpret_cast<decltype(f.create_image)>(ext("eglCreateImageKHR"));
        f.destroy_image = reinterpret_cast<decltype(f.destroy_image)>(ext("eglDestroyImageKHR"));
        f.target_texture = reinterpret_cast<decltype(f.target_texture)>(ext("glEGLImageTargetTexture2DOES"));
        f.target_renderbuffer = reinterpret_cast<decltype(f.target_renderbuffer)>(
            ext("glEGLImageTargetRenderbufferStorageOES"));
        f.gen_textures = reinterpret_cast<decltype(f.gen_textures)>(core("glGenTextures"));
        f.delete_textures = reinterpret_cast<decltype(f.delete_textures)>(core("glDeleteTextures"));
        f.bind_texture = reinterpret_cast<decltype(f.bind_texture)>(core("glBindTexture"));
        f.bind_buffer = reinterpret_cast<decltype(f.bind_buffer)>(ext("glBindBuffer"));
        f.tex_parameteri = reinterpret_cast<decltype(f.tex_parameteri)>(core("glTexParameteri"));
        f.tex_image = reinterpret_cast<decltype(f.tex_image)>(core("glTexImage2D"));
        f.tex_sub_image = reinterpret_cast<decltype(f.tex_sub_image)>(core("glTexSubImage2D"));
        f.pixel_store = reinterpret_cast<decltype(f.pixel_store)>(core("glPixelStorei"));
        f.get_integer = reinterpret_cast<decltype(f.get_integer)>(core("glGetIntegerv"));
        f.get_string = reinterpret_cast<decltype(f.get_string)>(core("glGetString"));
        f.usable = f.get_current_context && f.get_current_display && f.create_image && f.destroy_image &&
                   f.target_texture && f.gen_textures && f.delete_textures && f.bind_texture &&
                   f.bind_buffer && f.tex_parameteri && f.tex_image && f.tex_sub_image &&
                   f.pixel_store && f.get_integer && f.get_string;
        return f;
    }();
    return fns;
}

bool supported_format(int format) {
    return format == 1 || format == 2 || format == 3 || format == 4 || format == 5 ||
           format == 35 || format == 0x32315659;
}

// The buffer's ADataSpace (StandardMetadataType::DATASPACE: encoded header,
// then an int32), read with the mapper's seqlock; 0 when unset or contended.
int32_t dataspace(const uint8_t* mapping) {
    auto* header = reinterpret_cast<const bf_gralloc_header*>(mapping);
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint32_t sequence = __atomic_load_n(&header->sequences[0], __ATOMIC_ACQUIRE);
        if (sequence & 1) continue;
        uint32_t length = __atomic_load_n(&header->lengths[0], __ATOMIC_RELAXED);
        if (length < 4 || length > sizeof(header->metadata[0])) return 0;
        int32_t value = 0;
        for (int i = 0; i < 4; ++i)
            value |= int32_t(__atomic_load_n(&header->metadata[0][length - 4 + i], __ATOMIC_RELAXED)) << (8 * i);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (sequence == __atomic_load_n(&header->sequences[0], __ATOMIC_RELAXED)) return value;
    }
    return 0;
}

// Why a native-buffer image was refused (BIFROST_THUNK_TRACE).
uint64_t reject(const char* reason) {
    if (dbg().thunk_trace) fprintf(stderr, "[thunk] EGL_NATIVE_BUFFER_ANDROID image refused: %s\n", reason);
    return 0;
}

uint8_t clamp(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

// Saves and restores the unpack/texture state a texture upload touches, so
// the app's context (and GLStateTracker's view of it) is unchanged.
struct UploadState {
    const HostFns& f;
    bool es3;
    int texture = 0, alignment = 4, row_length = 0, skip_rows = 0, skip_pixels = 0, buffer = 0;
    UploadState(const HostFns& fns, bool modern) : f(fns), es3(modern) {
        f.get_integer(kTextureBinding2D, &texture);
        f.get_integer(kUnpackAlignment, &alignment);
        if (es3) {
            f.get_integer(kUnpackRowLength, &row_length);
            f.get_integer(kUnpackSkipRows, &skip_rows);
            f.get_integer(kUnpackSkipPixels, &skip_pixels);
            f.get_integer(kUnpackBufferBinding, &buffer);
            if (buffer) f.bind_buffer(kUnpackBuffer, 0);
            if (row_length) f.pixel_store(kUnpackRowLength, 0);
            if (skip_rows) f.pixel_store(kUnpackSkipRows, 0);
            if (skip_pixels) f.pixel_store(kUnpackSkipPixels, 0);
        }
        if (alignment != 4) f.pixel_store(kUnpackAlignment, 4);
    }
    ~UploadState() {
        if (alignment != 4) f.pixel_store(kUnpackAlignment, alignment);
        if (es3) {
            if (row_length) f.pixel_store(kUnpackRowLength, row_length);
            if (skip_rows) f.pixel_store(kUnpackSkipRows, skip_rows);
            if (skip_pixels) f.pixel_store(kUnpackSkipPixels, skip_pixels);
            if (buffer) f.bind_buffer(kUnpackBuffer, static_cast<uint32_t>(buffer));
        }
        f.bind_texture(kTexture2D, static_cast<uint32_t>(texture));
    }
};
}  // namespace

AndroidBufferImages::~AndroidBufferImages() {
    for (auto& [image, buffer] : buffers_)
        if (buffer.mapping) munmap(const_cast<uint8_t*>(buffer.mapping), buffer.size);
}

std::optional<int64_t> AndroidBufferImages::dispatch(CPU& cpu, const SymbolEntry& entry, Memory& mem) {
    const std::string& name = entry.name;
    if ((name == "eglCreateImageKHR" || name == "eglCreateImage") && cpu.regs[2] == kNativeBufferTarget) {
        cpu.regs[0] = create(mem, cpu.regs[3], cpu.regs[4], name == "eglCreateImage");
        return 0;
    }
    if ((name == "eglDestroyImageKHR" || name == "eglDestroyImage") && (cpu.regs[1] & kImageTag) == kImageTag) {
        cpu.regs[0] = destroy(cpu.regs[1]);  // EGL_FALSE (EGL_BAD_PARAMETER) when unknown
        return 0;
    }
    // HWUI makes a hardware bitmap by uploading into a texture whose storage is the bitmap's gralloc buffer: the
    // host image is a copy refreshed from the buffer at bind time, so the pixels also have to reach the buffer.
    if (name == "glTexSubImage2D" && cpu.regs[0] == kTexture2D && cpu.regs[1] == 0 && cpu.regs[6] == kRgba &&
            cpu.regs[7] == kUnsignedByte)
        capture_upload(cpu, mem);  // the call itself still goes to the host
    bool texture = name == "glEGLImageTargetTexture2DOES";
    if ((texture || name == "glEGLImageTargetRenderbufferStorageOES") && (cpu.regs[1] & kImageTag) == kImageTag) {
        bind(static_cast<uint32_t>(cpu.regs[0]), cpu.regs[1], !texture);
        return 0;
    }
    return std::nullopt;
}

// Returns the guest image handle, or 0 (the adapter reports EGL_BAD_PARAMETER).
uint64_t AndroidBufferImages::create(Memory& mem, uint64_t buffer, uint64_t attribs, bool wide) {
    if (!host().usable) return reject("host EGL/GL entry points missing");
    if (!buffer) return reject("null buffer");
    int32_t info[BF_GRALLOC_INTS];
    int guest_fd;
    try {
        int32_t base[2], dims[4];
        mem.read(buffer, base, sizeof(base));
        if (base[0] != kBufferMagic || base[1] < kBufferVersion) return reject("not an ANativeWindowBuffer");
        mem.read(buffer + 56, dims, sizeof(dims));  // width, height, stride, format
        uint64_t handle = mem.load<uint64_t>(buffer + 96);
        int32_t counts[3];  // native_handle_t: version, numFds, numInts
        mem.read(handle, counts, sizeof(counts));
        if (counts[1] != 1 || counts[2] < BF_GRALLOC_INTS) return reject("not a Bifrost gralloc handle");
        guest_fd = mem.load<int32_t>(handle + 12);
        mem.read(handle + 16, info, sizeof(info));
        if (info[BF_MAGIC] != BF_GRALLOC_MAGIC || info[BF_WIDTH] != dims[0] ||
                info[BF_HEIGHT] != dims[1]) return reject("gralloc descriptor mismatch");
        if (!supported_format(info[BF_FORMAT])) return reject("unsupported pixel format");
        // Only EGL_IMAGE_PRESERVED_KHR applies; protected content cannot be mapped.
        for (int i = 0; attribs && i < 64; i += 2) {
            int64_t key, value;
            if (wide) {
                key = mem.load<int64_t>(attribs + i * 8); value = mem.load<int64_t>(attribs + i * 8 + 8);
            } else {
                key = mem.load<int32_t>(attribs + i * 4); value = mem.load<int32_t>(attribs + i * 4 + 4);
            }
            if (key == kNone) break;
            if (key == kProtectedContent && value) return reject("protected content");
            if (key != kImagePreserved && key != kProtectedContent) return reject("unsupported attribute");
        }
    } catch (const EmuError&) {
        return reject("unreadable guest buffer");
    }
    int fd = frost::AndroidSurfaceManager::instance().host_fd(guest_fd);
    const uint64_t size = bf_gralloc_file_size(info);
    struct stat st{};
    if (fd < 0) return reject("guest fd has no host descriptor");
    if (fstat(fd, &st) || static_cast<uint64_t>(st.st_size) < size) return reject("allocation smaller than its descriptor");
    // Read/write when the memfd allows it: a GL upload into the image is copied to the buffer (capture_upload).
    bool read_write = true;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        read_write = false;
        mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    }
    if (mapping == MAP_FAILED) return reject("mmap failed");
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t image = kImageTag | ++next_;
    Buffer& b = buffers_[image];
    b.mapping = static_cast<const uint8_t*>(mapping);
    b.writable = read_write ? static_cast<uint8_t*>(mapping) : nullptr;
    b.size = size;
    b.width = info[BF_WIDTH]; b.height = info[BF_HEIGHT];
    b.stride = info[BF_STRIDE]; b.format = info[BF_FORMAT];
    return image;
}

// Host images can go without a current context; texture names only in the
// context that owns them (others are freed with their context).
bool AndroidBufferImages::destroy(uint64_t image) {
    const HostFns& f = host();
    std::lock_guard<std::mutex> lock(mu_);
    auto it = buffers_.find(image);
    if (it == buffers_.end()) return false;
    uint64_t current = f.usable ? reinterpret_cast<uint64_t>(f.get_current_context()) : 0;
    for (auto& [context, c] : it->second.contexts) {
        if (c.image) f.destroy_image(c.display, c.image);
        if (!c.texture) continue;
        if (context == current) f.delete_textures(1, &c.texture);
        else stale_textures_[context].push_back(c.texture);
    }
    munmap(const_cast<uint8_t*>(it->second.mapping), it->second.size);
    buffers_.erase(it);
    for (auto& [context, names] : textures_)
        for (auto texture = names.begin(); texture != names.end();) texture = texture->second == image ? names.erase(texture) : std::next(texture);
    return true;
}

void AndroidBufferImages::forget_context(uint64_t context) {
    const HostFns& f = host();
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [image, buffer] : buffers_) {
        auto it = buffer.contexts.find(context);
        if (it == buffer.contexts.end()) continue;
        if (it->second.image) f.destroy_image(it->second.display, it->second.image);
        buffer.contexts.erase(it);
    }
    stale_textures_.erase(context);  // its texture names went with it
    textures_.erase(context);
}

void AndroidBufferImages::bind(uint32_t target, uint64_t image, bool renderbuffer) {
    const HostFns& f = host();
    if (!f.usable) return;
    auto call = [&](void* host_image) {
        if (renderbuffer) { if (f.target_renderbuffer) f.target_renderbuffer(target, host_image); }
        else f.target_texture(target, host_image);
    };
    std::lock_guard<std::mutex> lock(mu_);
    auto it = buffers_.find(image);
    void* context = f.get_current_context();
    if (it == buffers_.end() || !context) {
        call(nullptr);  // GL_INVALID_VALUE (or no context: a no-op), as for any invalid image
        return;
    }
    Buffer& b = it->second;
    if (!renderbuffer && target == kTexture2D) {
        int bound = 0;
        f.get_integer(kTextureBinding2D, &bound);
        if (bound) textures_[reinterpret_cast<uint64_t>(context)][static_cast<uint32_t>(bound)] = image;
    }
    // Free textures of images destroyed elsewhere while this context was not current.
    if (auto stale = stale_textures_.find(reinterpret_cast<uint64_t>(context)); stale != stale_textures_.end()) {
        f.delete_textures(static_cast<int>(stale->second.size()), stale->second.data());
        stale_textures_.erase(stale);
    }
    ContextImage& c = b.contexts[reinterpret_cast<uint64_t>(context)];
    const char* version = reinterpret_cast<const char*>(f.get_string(kVersion));
    bool es3 = version && !std::strstr(version, "OpenGL ES 2") && !std::strstr(version, "OpenGL ES-C");
    // RGBA8 is already in the upload format: read it from the mapping in place (a row length for the gralloc
    // stride where the context has one) instead of copying every frame. Other formats convert into the scratch.
    const bool direct = b.format == 1 && (es3 || b.stride == b.width);
    const void* pixels;
    if (direct) pixels = b.mapping + BF_GRALLOC_OFFSET;
    else { convert(b, scratch_); pixels = scratch_.data(); }
    {
        UploadState saved(f, es3);
        if (direct && es3 && b.stride != b.width) f.pixel_store(kUnpackRowLength, b.stride);
        if (!c.texture) {
            c.display = f.get_current_display();
            f.gen_textures(1, &c.texture);
            f.bind_texture(kTexture2D, c.texture);
            f.tex_parameteri(kTexture2D, kMinFilter, kLinear);
            f.tex_parameteri(kTexture2D, kMagFilter, kLinear);
            f.tex_parameteri(kTexture2D, kWrapS, kClampToEdge);
            f.tex_parameteri(kTexture2D, kWrapT, kClampToEdge);
            f.tex_image(kTexture2D, 0, kRgba, b.width, b.height, 0, kRgba, kUnsignedByte, pixels);
        } else {
            // Sub-image updates keep the storage the EGLImage shares;
            // glTexImage2D would orphan it.
            f.bind_texture(kTexture2D, c.texture);
            f.tex_sub_image(kTexture2D, 0, 0, 0, b.width, b.height, kRgba, kUnsignedByte, pixels);
        }
        // UploadState restores a row length only when the app had one set.
        if (direct && es3 && b.stride != b.width) f.pixel_store(kUnpackRowLength, 0);
    }
    if (!c.image) {
        const int attribs[] = {kTextureLevel, 0, kImagePreserved, kTrue, kNone};
        c.image = f.create_image(c.display, context, kTexture2DImage,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(c.texture)), attribs);
    }
    call(c.image);
}

// glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels) while the bound texture is the app's
// view of an RGBA8 image: copy the rectangle from guest memory into the buffer, honouring the unpack row length,
// alignment and skips. Anything else (a pixel unpack buffer, another format, a read-only mapping) is left to the host.
void AndroidBufferImages::capture_upload(CPU& cpu, Memory& mem) {
    const HostFns& f = host();
    if (!f.usable) return;
    void* context = f.get_current_context();
    if (!context) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto names = textures_.find(reinterpret_cast<uint64_t>(context));
    if (names == textures_.end()) return;
    int bound = 0;
    f.get_integer(kTextureBinding2D, &bound);
    auto named = names->second.find(static_cast<uint32_t>(bound));
    if (named == names->second.end()) return;
    auto found = buffers_.find(named->second);
    if (found == buffers_.end()) return;
    Buffer& b = found->second;
    const int64_t x = static_cast<int64_t>(cpu.regs[2]), y = static_cast<int64_t>(cpu.regs[3]);
    const int64_t w = static_cast<int64_t>(cpu.regs[4]), h = static_cast<int64_t>(cpu.regs[5]);
    if (!b.writable || b.format != 1 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > b.width || y + h > b.height) return;
    const char* version = reinterpret_cast<const char*>(f.get_string(kVersion));
    const bool es3 = version && !std::strstr(version, "OpenGL ES 2") && !std::strstr(version, "OpenGL ES-C");
    int alignment = 4, row_length = 0, skip_rows = 0, skip_pixels = 0, buffer = 0;
    f.get_integer(kUnpackAlignment, &alignment);
    if (es3) {
        f.get_integer(kUnpackBufferBinding, &buffer);
        f.get_integer(kUnpackRowLength, &row_length);
        f.get_integer(kUnpackSkipRows, &skip_rows);
        f.get_integer(kUnpackSkipPixels, &skip_pixels);
    }
    if (buffer || alignment < 1 || alignment > 8) return;  // the pixels are in a host buffer object
    try {
        const uint64_t pixels = mem.load<uint64_t>(cpu.sp);  // the ninth argument is on the stack
        if (!pixels) return;
        const int64_t row_bytes = static_cast<int64_t>(row_length > 0 ? row_length : w) * 4;
        const int64_t pitch = (row_bytes + alignment - 1) / alignment * alignment;
        const uint64_t start = pixels + static_cast<uint64_t>(skip_rows) * pitch + static_cast<uint64_t>(skip_pixels) * 4;
        for (int64_t row = 0; row < h; ++row) {
            const size_t offset = BF_GRALLOC_OFFSET + (static_cast<size_t>(y + row) * b.stride + x) * 4;
            if (offset + static_cast<size_t>(w) * 4 > b.size) return;
            mem.read(start + static_cast<uint64_t>(row) * pitch, b.writable + offset, static_cast<size_t>(w) * 4);
        }
    } catch (const EmuError&) {
        // An unreadable source is the host call's error to report.
    }
}

// Linear gralloc pixels (after the metadata page) to tightly packed RGBA8.
// YUV420 is planar Y, V, U with 2x2 chroma (gralloc_mapper.c planes()); its
// matrix and range follow the buffer's dataspace, as FrameworkSurfaces does.
void AndroidBufferImages::convert(Buffer& b, std::vector<uint8_t>& scratch) {
    const uint8_t* pixels = b.mapping + BF_GRALLOC_OFFSET;
    const int w = b.width, h = b.height, stride = b.stride;
    scratch.resize(size_t(w) * h * 4);
    uint8_t* out = scratch.data();
    if (b.format == 35 || b.format == 0x32315659) {
        const int32_t space = dataspace(b.mapping);
        const int standard = (space >> 16) & 63;
        const bool full = ((space >> 27) & 7) == 1;
        const double kr = standard == 1 ? .2126 : (standard == 6 || standard == 7) ? .2627 : .299;
        const double kb = standard == 1 ? .0722 : (standard == 6 || standard == 7) ? .0593 : .114;
        const double kg = 1 - kr - kb, cs = full ? 1 : 255.0 / 224;
        const int ys = static_cast<int>(std::lround((full ? 1 : 255.0 / 219) * 65536)), yo = full ? 0 : 16;
        const int rv = static_cast<int>(std::lround(2 * (1 - kr) * cs * 65536));
        const int gu = static_cast<int>(std::lround(-2 * kb * (1 - kb) / kg * cs * 65536));
        const int gv = static_cast<int>(std::lround(-2 * kr * (1 - kr) / kg * cs * 65536));
        const int bu = static_cast<int>(std::lround(2 * (1 - kb) * cs * 65536));
        const int cstride = ((stride / 2) + 15) & ~15;
        const uint8_t* yp = pixels;
        const uint8_t* vp = pixels + size_t(stride) * h;
        const uint8_t* up = vp + size_t(cstride) * (h / 2);
        for (int y = 0; y < h; ++y) {
            const uint8_t* yr = yp + size_t(y) * stride;
            const uint8_t* ur = up + size_t(y / 2) * cstride;
            const uint8_t* vr = vp + size_t(y / 2) * cstride;
            uint8_t* o = out + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                const int luma = (yr[x] - yo) * ys + 32768, u = ur[x / 2] - 128, v = vr[x / 2] - 128;
                o[0] = clamp((luma + rv * v) >> 16);
                o[1] = clamp((luma + gu * u + gv * v) >> 16);
                o[2] = clamp((luma + bu * u) >> 16);
                o[3] = 255;
                o += 4;
            }
        }
        return;
    }
    const int bpp = bf_gralloc_bpp(b.format);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = pixels + size_t(y) * stride * bpp;
        uint8_t* o = out + size_t(y) * w * 4;
        switch (b.format) {
        case 1: std::memcpy(o, row, size_t(w) * 4); break;
        case 2:
            for (int x = 0; x < w; ++x, o += 4) { std::memcpy(o, row + x * 4, 3); o[3] = 255; }
            break;
        case 5:
            for (int x = 0; x < w; ++x, o += 4) {
                o[0] = row[x * 4 + 2]; o[1] = row[x * 4 + 1]; o[2] = row[x * 4]; o[3] = row[x * 4 + 3];
            }
            break;
        case 3:
            for (int x = 0; x < w; ++x, o += 4) { std::memcpy(o, row + x * 3, 3); o[3] = 255; }
            break;
        case 4:
            for (int x = 0; x < w; ++x, o += 4) {
                const unsigned p = row[x * 2] | (row[x * 2 + 1] << 8);
                o[0] = uint8_t(((p >> 11) & 31) * 255 / 31);
                o[1] = uint8_t(((p >> 5) & 63) * 255 / 63);
                o[2] = uint8_t((p & 31) * 255 / 31);
                o[3] = 255;
            }
            break;
        }
    }
}
}  // namespace arm64emu
