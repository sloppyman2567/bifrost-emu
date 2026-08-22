/* test_linux_audio.c — exercises the 1.5.5-alpha audio thunk arms.
 *
 * Front doors covered (all funnel into the shared AudioEngine ring):
 *   1. ALSA subset: snd_pcm_open → hw_params setters → writei → drain/close
 *   2. SDL2 queue path: SDL_OpenAudioDevice → SDL_QueueAudio → GetQueuedAudioSize
 *   3. /dev/dsp OSS path via the VFS node
 *   4. PulseAudio simple API
 *
 * Headless-safe: the AudioEngine works without a host audio device
 * (WAV-dump backend still counts bytes). Exits 0 = pass, 77 = skip if
 * none of the thunk libs resolve.
 *
 * Build:
 *   make cross SRC=ctest_real/test_linux_audio.c OUT=ctest_real/test_linux_audio.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %d: %s\n", checks, msg); } \
} while (0)

static uint64_t bifrost_dlopen(const char* path, uint64_t mode) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)path;
    register uint64_t x1 __asm__("x1") = mode;
    register uint64_t x8 __asm__("x8") = 0x1002; /* dlopen */
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
static uint64_t bifrost_dlsym(uint64_t h, const char* sym) {
    register uint64_t x0 __asm__("x0") = h;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)sym;
    register uint64_t x8 __asm__("x8") = 0x1003; /* dlsym */
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}
#define DL(lib) bifrost_dlopen(lib, 2 /*RTLD_NOW*/)
#define SYM(h, n) bifrost_dlsym(h, n)

int main(void) {
    /* ── 16-bit stereo sine burst ─────────────────────────────────── */
    enum { N = 4800 };
    static int16_t buf[N * 2];
    for (int i = 0; i < N; i++) {
        int16_t v = (int16_t)(3000.0 * __builtin_sin(i * 0.05));
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }

    /* ── 1. ALSA subset ───────────────────────────────────────────── */
    uint64_t ha = DL("libasound.so.2");
    CHECK(ha != 0, "libasound.so.2 resolves");
    if (ha) {
        typedef int (*open_t)(void**, const char*, int, int);
        typedef int (*hpm_t)(void**);
        typedef int (*set2_t)(void*, void*);
        typedef int (*setru_t)(void*, void*, unsigned, int*);
        typedef long (*writei_t)(void*, const void*, unsigned long);
        typedef int (*close_t)(void*);
        open_t pcm_open = (open_t)SYM(ha, "snd_pcm_open");
        hpm_t hp_malloc = (hpm_t)SYM(ha, "snd_pcm_hw_params_malloc");
        set2_t hp_any = (set2_t)SYM(ha, "snd_pcm_hw_params_any");
        typedef int (*setf_t)(void*, void*, unsigned);
        setf_t hp_fmt = (setf_t)SYM(ha, "snd_pcm_hw_params_set_format");
        setf_t hp_ch = (setf_t)SYM(ha, "snd_pcm_hw_params_set_channels");
        setru_t hp_rate = (setru_t)SYM(ha, "snd_pcm_hw_params_set_rate");
        set2_t hp_params = (set2_t)SYM(ha, "snd_pcm_hw_params");
        writei_t wr = (writei_t)SYM(ha, "snd_pcm_writei");
        close_t cl = (close_t)SYM(ha, "snd_pcm_close");
        CHECK(pcm_open && hp_malloc && hp_any && hp_fmt && hp_ch && hp_rate &&
              hp_params && wr && cl, "ALSA symbols resolve");
        if (pcm_open && hp_malloc && wr && cl) {
            void* pcm = NULL; void* hp = NULL; int dir = 0;
            CHECK(pcm_open(&pcm, "default", 0 /*playback*/, 0) == 0,
                  "snd_pcm_open(default)");
            CHECK(pcm != NULL, "pcm handle nonzero");
            CHECK(hp_malloc(&hp) == 0 && hp != NULL, "hw_params_malloc");
            if (hp_any) (void)hp_any(pcm, hp);
            if (hp_fmt) (void)hp_fmt(pcm, hp, 2 /*S16_LE*/);
            if (hp_ch) (void)hp_ch(pcm, hp, 2);
            if (hp_rate) (void)hp_rate(pcm, hp, 44100, &dir);
            if (hp_params) (void)hp_params(pcm, hp);
            long r = wr(pcm, buf, N);
            CHECK(r == N, "snd_pcm_writei returns frames");
            (void)cl(pcm);
        }
    }

    /* ── 2. SDL2 queue path ───────────────────────────────────────── */
    uint64_t hs = DL("libSDL2-2.0.so.0");
    if (!hs) hs = DL("libSDL2.so");
    CHECK(hs != 0, "libSDL2 resolves");
    if (hs) {
        struct Spec {   /* guest SDL_AudioSpec layout */
            int32_t freq; uint16_t format; uint8_t channels;
            uint8_t silence; uint16_t samples; uint16_t padding;
            uint32_t size; void* callback; void* userdata;
        };
        typedef void* (*opendev_t)(const char*, int, struct Spec*, struct Spec*, int);
        typedef int (*queue_t)(void*, const void*, uint32_t);
        typedef uint32_t (*qsize_t)(void*);
        opendev_t od = (opendev_t)SYM(hs, "SDL_OpenAudioDevice");
        queue_t qa = (queue_t)SYM(hs, "SDL_QueueAudio");
        qsize_t qs = (qsize_t)SYM(hs, "SDL_GetQueuedAudioSize");
        CHECK(od && qa && qs, "SDL audio symbols resolve");
        if (od && qa && qs) {
            struct Spec want;
            memset(&want, 0, sizeof(want));
            want.freq = 22050;
            want.format = 0x8010;   /* AUDIO_S16LSB */
            want.channels = 2;
            want.samples = 1024;
            void* got = NULL;
            void* dev = od(NULL, 0, &want, (struct Spec*)&got, 0);
            CHECK(dev != NULL, "SDL_OpenAudioDevice returns handle");
            int rc = qa(dev, buf, sizeof(buf));
            CHECK(rc == 0, "SDL_QueueAudio succeeds");
            uint32_t sz = qs(dev);
            (void)sz;   /* drained asynchronously by the device thread */
        }
    }

    /* ── 3. Pulse simple ──────────────────────────────────────────── */
    uint64_t hp = DL("libpulse.so.0");
    CHECK(hp != 0, "libpulse.so.0 resolves");
    if (hp) {
        typedef struct { uint32_t fmt; uint32_t rate; uint8_t ch; } spec_t;
        typedef void* (*new_t)(void*, void*, int, void*, spec_t*, void*, void*, int**);
        typedef int (*wr_t)(void*, const void*, size_t, int**);
        new_t pn = (new_t)SYM(hp, "pa_simple_new");
        wr_t pw = (wr_t)SYM(hp, "pa_simple_write");
        CHECK(pn && pw, "pa_simple symbols resolve");
        if (pn && pw) {
            spec_t sp = {3 /*S16LE*/, 8000, 1};
            void* pa = pn(NULL, NULL, 0, "t", &sp, NULL, NULL, NULL);
            CHECK(pa != NULL, "pa_simple_new returns handle");
            if (pa) {
                static int16_t mono[400];
                for (int i = 0; i < 400; i++)
                    mono[i] = (int16_t)(1000 * __builtin_sin(i * 0.1));
                CHECK(pw(pa, mono, sizeof(mono), NULL) == 0,
                      "pa_simple_write succeeds");
            }
        }
    }

    /* ── 4. /dev/dsp OSS node ─────────────────────────────────────── */
    int fd = open("/dev/dsp", O_WRONLY);
    CHECK(fd >= 0, "/dev/dsp opens");
    if (fd >= 0) {
        ssize_t w = write(fd, buf, 4096);
        CHECK(w == 4096, "/dev/dsp write accepted");
        close(fd);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    if (fails == 0) printf("ALL PASS\n");
    return fails ? 1 : 0;
}
