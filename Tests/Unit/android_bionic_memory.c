// Bionic's memcpy, memmove and memset under the guest's own linker (linker64): the host-native path
// (Source/System/Core/NativeLibc.hpp) finds them through the mapped libc.so's symbol table. Freestanding, like
// android_bionic_bootstrap.c: the actual pinned Bionic DSOs are linked, no host headers or startup objects.
// Results are checked against byte loops (built with -fno-builtin so they stay loops); Scripts/Android/
// test_android_native_libc.sh checks that the native path ran, and that BIFROST_NO_NATIVE_LIBC=1 gives the same results.
extern int printf(const char*, ...);
extern void* malloc(unsigned long);
extern void* memset(void*, int, unsigned long);
extern void* memcpy(void*, const void*, unsigned long);
extern void* memmove(void*, const void*, unsigned long);

static void* (*volatile call_memset)(void*, int, unsigned long) = memset;
static void* (*volatile call_memcpy)(void*, const void*, unsigned long) = memcpy;
static void* (*volatile call_memmove)(void*, const void*, unsigned long) = memmove;

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

typedef unsigned char u8;
static void ref_set(u8* d, int c, unsigned long n) { for (unsigned long i = 0; i < n; i++) d[i] = (u8)c; }
static void ref_copy(u8* d, const u8* s, unsigned long n) { for (unsigned long i = 0; i < n; i++) d[i] = s[i]; }
static int same(const u8* a, const u8* b, unsigned long n) { for (unsigned long i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int all_equal(const u8* a, u8 v, unsigned long n) { for (unsigned long i = 0; i < n; i++) if (a[i] != v) return 0; return 1; }

static unsigned rng = 4242;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

enum { PAD = 64, MAX = (1 << 20) + 4096 };
static const unsigned long sizes[] = {1024, 1025, 2048, 4095, 4096, 8192, 12345, 65536, 100003, 1 << 20};
static const int offsets[] = {0, 1, 7, 8, 15, 33};

int main(void) {
    u8* src = malloc(MAX + 4 * PAD);
    u8* dst = malloc(MAX + 4 * PAD);
    u8* model = malloc(MAX + 4 * PAD);
    if (!src || !dst || !model) { printf("android_bionic_memory: out of memory\n"); return 1; }
    const int nsizes = (int)(sizeof sizes / sizeof sizes[0]), noffsets = (int)(sizeof offsets / sizeof offsets[0]);
    for (int s = 0; s < nsizes; s++)
        for (int o = 0; o < noffsets; o++) {
            const unsigned long n = sizes[s];
            u8* d = dst + PAD + offsets[o];
            const u8* sp = src + PAD + offsets[(o + 2) % noffsets];
            for (unsigned long i = 0; i < n + 2 * PAD + 64; i++) src[i] = (u8)rnd();
            for (int v = 0; v < 3; v++) {
                const int value = v == 0 ? 0 : v == 1 ? 0x5A : 0x1A5;   // memset converts the value to an unsigned char
                ref_set(dst, 0xAA, n + 2 * PAD + 64);
                void* r = call_memset(d, value, n);
                CHECK(r == d && all_equal(d, (u8)value, n) && all_equal(dst, 0xAA, PAD + offsets[o]) && all_equal(d + n, 0xAA, PAD));
            }
            ref_set(dst, 0xAA, n + 2 * PAD + 64);
            void* r = call_memcpy(d, sp, n);
            CHECK(r == d && same(d, sp, n) && all_equal(dst, 0xAA, PAD + offsets[o]) && all_equal(d + n, 0xAA, PAD));
            ref_set(dst, 0xAA, n + 2 * PAD + 64);
            r = call_memmove(d, sp, n);
            CHECK(r == d && same(d, sp, n) && all_equal(d + n, 0xAA, PAD));
        }
    // memmove over its own source, both directions, against a copy through another buffer.
    static const unsigned long shifts[] = {1, 7, 16, 64, 1000, 4000};
    for (int s = 0; s < nsizes - 1; s++)
        for (int h = 0; h < 6; h++)
            for (int dir = 0; dir < 2; dir++) {
                const unsigned long n = sizes[s], shift = shifts[h];
                u8* base = dst + PAD;
                for (unsigned long i = 0; i < n + shift + PAD; i++) base[i] = (u8)rnd();
                u8* from = dir ? base + shift : base;
                u8* to = dir ? base : base + shift;
                ref_copy(model, from, n);
                void* r = call_memmove(to, from, n);
                CHECK(r == to && same(to, model, n));
            }
    // An overlapping memcpy is undefined: it must return its destination and stay inside the two ranges' union.
    {
        const unsigned long n = 4096;
        u8* base = dst + 4 * PAD;
        for (unsigned long i = 0; i < n + 8 * PAD; i++) dst[i] = (u8)rnd();
        u8 before[PAD], after[PAD];
        ref_copy(before, base - PAD, PAD);
        ref_copy(after, base + n + 8, PAD);
        void* r = call_memcpy(base + 8, base, n);
        CHECK(r == base + 8 && same(base - PAD, before, PAD) && same(base + n + 8, after, PAD));
    }
    // A call's result feeds the next one.
    u8* r = call_memset(call_memcpy(dst + 100, src, 3000), 0x21, 2000);
    CHECK(r == dst + 100 && all_equal(r, 0x21, 2000));

    if (failures) { printf("android_bionic_memory: %d of %d checks failed\n", failures, checks); return 1; }
    printf("android_bionic_memory: %d checks: ALL PASS\n", checks);
    return 0;
}
