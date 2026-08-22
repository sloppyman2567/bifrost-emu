/* test_android_audio.c — exercises the AAudio + OpenSL ES thunk arms.
 *
 * Headless-safe: no host audio device needed — the AudioEngine counts
 * bytes in its WAV-dump backend. Exits 0 = ALL PASS; 77 = skip when
 * libaaudio/libOpenSLES don't resolve (non-Android rootfs).
 *
 * Build:
 *   make cross SRC=ctest_real/test_android_audio.c OUT=ctest_real/test_android_audio.elf
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#define DL(lib) bifrost_dlopen(lib, 2)
#define SYM(h, n) bifrost_dlsym(h, n)

/* guest data callback: fills the bounce with a square wave */
static uint64_t cb_hits = 0;
static void data_cb(void* stream, void* userdata, void* audioData,
                    int32_t numFrames) {
    (void)stream; (void)userdata;
    int16_t* out = (int16_t*)audioData;
    for (int32_t i = 0; i < numFrames * 2; i++)
        out[i] = (cb_hits & 1) ? 2000 : -2000;
    cb_hits++;
}
/* OpenSL buffer-queue callback */
static uint64_t bq_hits = 0;
static void bq_cb(void* itf, void* ctx) { (void)itf; (void)ctx; bq_hits++; }

int main(void) {
    static int16_t pcm[4800 * 2];
    for (unsigned i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++)
        pcm[i] = (int16_t)(2500 * __builtin_sin(i * 0.03));

    /* ══ AAudio ══════════════════════════════════════════════════ */
    uint64_t ha = DL("libaaudio.so");
    CHECK(ha != 0, "libaaudio.so resolves");
    if (ha) {
        typedef int (*newb_t)(void**);
        typedef void (*seti_t)(void*, int32_t);
        typedef void (*setcb_t)(void*, void*, void*);
        typedef int (*open_t)(void*, void**);
        typedef int (*req_t)(void*);
        typedef int (*wr_t)(void*, const void*, int32_t, int64_t);
        typedef int (*close_t)(void*);
        typedef int32_t (*geti_t)(void*);
        newb_t bnew = (newb_t)SYM(ha, "AAudioStreamBuilder_new");
        seti_t sf = (seti_t)SYM(ha, "AAudioStreamBuilder_setFormat");
        seti_t sc = (seti_t)SYM(ha, "AAudioStreamBuilder_setChannelCount");
        seti_t sr = (seti_t)SYM(ha, "AAudioStreamBuilder_setSampleRate");
        setcb_t sdcb = (setcb_t)SYM(ha, "AAudioStreamBuilder_setDataCallback");
        open_t op = (open_t)SYM(ha, "AAudioStreamBuilder_openStream");
        req_t st = (req_t)SYM(ha, "AAudioStream_requestStart");
        wr_t wr = (wr_t)SYM(ha, "AAudioStream_write");
        geti_t gst = (geti_t)SYM(ha, "AAudioStream_getState");
        close_t cls = (close_t)SYM(ha, "AAudioStream_close");
        CHECK(bnew && sf && sc && sr && sdcb && op && st && wr && gst && cls,
              "AAudio symbols resolve");
        if (bnew && op && wr) {
            void* b = NULL;
            CHECK(bnew(&b) == 0 && b != NULL, "builder_new");
            sf(b, 1);       /* I16 */
            sc(b, 2);
            sr(b, 44100);
            sdcb(b, (void*)data_cb, NULL);
            void* stream = NULL;
            CHECK(op(b, &stream) == 0 && stream != NULL, "openStream");
            if (st) (void)st(stream);
            int32_t state = gst ? gst(stream) : -1;
            CHECK(state == 4 /*Started*/, "getState == Started");
            /* blocking write also works alongside the callback pump */
            CHECK(wr(stream, pcm, 4800, 0) == 4800, "blocking write");
            if (cls) { (void)st(stream); (void)cls(stream); }
        }
    }

    /* ══ OpenSL ES ═══════════════════════════════════════════════ */
    uint64_t hs = DL("libOpenSLES.so");
    CHECK(hs != 0, "libOpenSLES.so resolves");
    if (hs) {
        typedef int (*createeng_t)(void**, int, void*, int, void*, void*);
        typedef int (*realize_t)(void*, int, void*);
        typedef int (*getif_t)(void*, void*, void**);
        typedef int (*createmix_t)(void*, void**, int, void*, void*);
        typedef int (*createplayer_t)(void*, void**, void*, void*);
        typedef int (*enqueue_t)(void*, const void*, uint32_t);
        typedef int (*regcb_t)(void*, void*, void*);
        createeng_t ce = (createeng_t)SYM(hs, "slCreateEngine");
        realize_t rz = (realize_t)SYM(hs, "__osl_realize");
        getif_t gi = (getif_t)SYM(hs, "__osl_getinterface");
        createmix_t cm = (createmix_t)SYM(hs, "__osl_eng_createmix");
        createplayer_t cp = (createplayer_t)SYM(hs, "__osl_eng_createplayer");
        enqueue_t enq = (enqueue_t)SYM(hs, "__osl_bq_enqueue");
        regcb_t rc = (regcb_t)SYM(hs, "__osl_bq_register");
        uint64_t iid_engine = SYM(hs, "SL_IID_ENGINE");
        uint64_t iid_mix = SYM(hs, "SL_IID_OUTPUTMIX");
        uint64_t iid_player = SYM(hs, "SL_IID_BUFFERQUEUE");
        uint64_t iid_play = SYM(hs, "SL_IID_PLAY");
        CHECK(ce && rz && gi && cm && cp && enq && rc &&
              iid_engine && iid_mix && iid_player && iid_play,
              "OpenSL symbols resolve");
        if (ce && rz && gi && cm && cp && enq && rc) {
            void* eng = NULL;
            CHECK(ce(&eng, 0, NULL, 0, NULL, NULL) == 0 && eng != NULL,
                  "slCreateEngine");
            CHECK(rz(eng, 0, NULL) == 0, "engine Realize");
            void* engItf = NULL;
            CHECK(gi(eng, (void*)iid_engine, &engItf) == 0 && engItf != NULL,
                  "GetInterface(ENGINE)");
            void* mix = NULL;
            CHECK(cm(engItf, &mix, 0, NULL, NULL) == 0 && mix != NULL,
                  "CreateOutputMix");
            CHECK(rz(mix, 0, NULL) == 0, "mix Realize");
            void* player = NULL;
            CHECK(cp(engItf, &player, NULL, NULL) == 0 && player != NULL,
                  "CreatePlayer");
            CHECK(rz(player, 0, NULL) == 0, "player Realize");
            void* bq = NULL;
            CHECK(gi(player, (void*)iid_player, &bq) == 0 && bq != NULL,
                  "player GetInterface(BUFFERQUEUE)");
            void* play = NULL;
            CHECK(gi(player, (void*)iid_play, &play) == 0 && play != NULL,
                  "player GetInterface(PLAY)");
            CHECK(rc(bq, (void*)bq_cb, NULL) == 0, "RegisterCallback");
            /* two buffers → two callbacks */
            CHECK(enq(bq, pcm, sizeof(pcm)) == 0, "Enqueue #1");
            CHECK(enq(bq, pcm, sizeof(pcm)) == 0, "Enqueue #2");
            CHECK(bq_hits >= 2, "buffer-queue callback fired");
        }
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    if (fails == 0) printf("ALL PASS\n");
    return fails ? 1 : 0;
}
